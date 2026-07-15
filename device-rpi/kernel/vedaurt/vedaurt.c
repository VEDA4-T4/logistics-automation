/*
 * vedaurt.c - serdev based UART byte-stream character driver
 *
 * Raspberry Pi <-> STM32 UART byte-stream driver
 *
 * UART frame parsing, CRC validation, ACK and retry policy
 * are handled in user space.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#define DEVICE_NAME                 "vedaurt"
#define CLASS_NAME                  "vedaurt_class"

#define VEDAURT_BAUDRATE            115200U
#define VEDAURT_WRITE_TIMEOUT_MS    1000U

/*
 * One maximum UART frame is approximately 135 bytes.
 * This buffer allows burst reception and prevents short-term overflow.
 */
#define VEDAURT_RX_BUFFER_SIZE      65536U
#define VEDAURT_MAX_WRITE_SIZE      4096U

struct vedaurt_data
{
    struct serdev_device *serdev;

    struct cdev cdev;
    struct class *class;
    struct device *device;
    dev_t devt;

    /*
     * RX ring buffer.
     * One byte is reserved to distinguish full and empty states.
     */
    u8 *rx_buffer;
    size_t rx_head;
    size_t rx_tail;

    spinlock_t rx_lock;
    struct mutex rx_read_lock;
    struct wait_queue_head rx_wait_queue;

    atomic64_t rx_received_count;
    atomic64_t rx_read_count;
    atomic64_t rx_overflow_count;
    atomic64_t rx_discarded_closed_count;

    /*
     * Prevent complete writes from different writers
     * from being interleaved.
     */
    struct mutex tx_lock;
    wait_queue_head_t tx_wait_queue;

    /*
     * Object lifetime management.
     */
    struct kref refcount;
    atomic_t open_count;

    bool online;
};


/* ------------------------------------------------------------------------- */
/* RX ring buffer helpers                                                    */
/* ------------------------------------------------------------------------- */

static size_t vedaurt_rx_count_locked(
    const struct vedaurt_data *d
)
{
    if (d->rx_head >= d->rx_tail)
        return d->rx_head - d->rx_tail;

    return VEDAURT_RX_BUFFER_SIZE
         - d->rx_tail
         + d->rx_head;
}


static size_t vedaurt_rx_space_locked(
    const struct vedaurt_data *d
)
{
    return (VEDAURT_RX_BUFFER_SIZE - 1U)
         - vedaurt_rx_count_locked(d);
}


static bool vedaurt_rx_push_locked(
    struct vedaurt_data *d,
    u8 byte
)
{
    size_t next_head;

    if (vedaurt_rx_space_locked(d) == 0U)
        return false;

    d->rx_buffer[d->rx_head] = byte;

    next_head = d->rx_head + 1U;

    if (next_head >= VEDAURT_RX_BUFFER_SIZE)
        next_head = 0U;

    d->rx_head = next_head;

    return true;
}


static void vedaurt_rx_reset_locked(
    struct vedaurt_data *d
)
{
    d->rx_head = 0U;
    d->rx_tail = 0U;
}


static void vedaurt_rx_peek_locked(
    const struct vedaurt_data *d,
    u8 *destination,
    size_t length
)
{
    size_t index;
    size_t i;

    index = d->rx_tail;

    for (i = 0U; i < length; i++)
    {
        destination[i] = d->rx_buffer[index];

        index++;

        if (index >= VEDAURT_RX_BUFFER_SIZE)
            index = 0U;
    }
}


static void vedaurt_rx_drop_locked(
    struct vedaurt_data *d,
    size_t length
)
{
    d->rx_tail += length;

    if (d->rx_tail >= VEDAURT_RX_BUFFER_SIZE)
        d->rx_tail -= VEDAURT_RX_BUFFER_SIZE;
}


static size_t vedaurt_rx_available(
    struct vedaurt_data *d
)
{
    unsigned long flags;
    size_t count;

    spin_lock_irqsave(&d->rx_lock, flags);

    count = vedaurt_rx_count_locked(d);

    spin_unlock_irqrestore(&d->rx_lock, flags);

    return count;
}


/* ------------------------------------------------------------------------- */
/* Object lifetime                                                           */
/* ------------------------------------------------------------------------- */

static void vedaurt_release_kref(
    struct kref *refcount
)
{
    struct vedaurt_data *d;

    d = container_of(
        refcount,
        struct vedaurt_data,
        refcount
    );

    kvfree(d->rx_buffer);
    kfree(d);
}


/* ------------------------------------------------------------------------- */
/* serdev callbacks                                                          */
/* ------------------------------------------------------------------------- */

static size_t vedaurt_receive_buf(
    struct serdev_device *serdev,
    const u8 *data,
    size_t count
)
{
    struct vedaurt_data *d;
    unsigned long flags;
    size_t i;
    size_t dropped;

    d = serdev_device_get_drvdata(serdev);

    if (d == NULL)
        return count;

    if (!READ_ONCE(d->online))
        return count;

    /*
     * When no user-space process has the device open,
     * do not accumulate stale bytes in the RX FIFO.
     */
    if (atomic_read(&d->open_count) == 0)
    {
        atomic64_add(
            count,
            &d->rx_discarded_closed_count
        );

        return count;
    }

    atomic64_add(
        count,
        &d->rx_received_count
    );

    dropped = 0U;

    spin_lock_irqsave(&d->rx_lock, flags);

    for (i = 0U; i < count; i++)
    {
        if (!vedaurt_rx_push_locked(d, data[i]))
            dropped++;
    }

    if (dropped > 0U)
    {
        atomic64_add(
            dropped,
            &d->rx_overflow_count
        );
    }

    spin_unlock_irqrestore(&d->rx_lock, flags);

    if (dropped > 0U)
    {
        dev_warn_ratelimited(
            &serdev->dev,
            "RX buffer overflow: dropped %zu bytes, total=%lld\n",
            dropped,
            atomic64_read(&d->rx_overflow_count)
        );
    }

    if (count > 0U)
        wake_up_interruptible(&d->rx_wait_queue);

    /*
     * All bytes were consumed by the serdev callback.
     * Bytes that did not fit in the local FIFO are counted as dropped.
     */
    return count;
}


static void vedaurt_write_wakeup(
    struct serdev_device *serdev
)
{
    struct vedaurt_data *d;

    d = serdev_device_get_drvdata(serdev);

    if (d != NULL)
        wake_up_interruptible(&d->tx_wait_queue);
}


static const struct serdev_device_ops vedaurt_serdev_ops =
{
    .receive_buf = vedaurt_receive_buf,
    .write_wakeup = vedaurt_write_wakeup,
};


/* ------------------------------------------------------------------------- */
/* Character device operations                                               */
/* ------------------------------------------------------------------------- */

static int vedaurt_open(
    struct inode *inode,
    struct file *filp
)
{
    struct vedaurt_data *d;
    unsigned long flags;

    d = container_of(
        inode->i_cdev,
        struct vedaurt_data,
        cdev
    );

    if (!READ_ONCE(d->online))
        return -ENODEV;

    if (!kref_get_unless_zero(&d->refcount))
        return -ENODEV;

    /*
     * Only one user-space reader is allowed.
     * Multiple readers would steal bytes from one another.
     */
    if (atomic_cmpxchg(&d->open_count, 0, 1) != 0)
    {
        kref_put(
            &d->refcount,
            vedaurt_release_kref
        );

        return -EBUSY;
    }

    if (!READ_ONCE(d->online))
    {
        atomic_set(&d->open_count, 0);

        kref_put(
            &d->refcount,
            vedaurt_release_kref
        );

        return -ENODEV;
    }

    /*
     * Discard stale data from a previous test or connection.
     */
    spin_lock_irqsave(&d->rx_lock, flags);
    vedaurt_rx_reset_locked(d);
    spin_unlock_irqrestore(&d->rx_lock, flags);

    filp->private_data = d;
    filp->f_pos = 0;

    return 0;
}


static int vedaurt_release(
    struct inode *inode,
    struct file *filp
)
{
    struct vedaurt_data *d;
    unsigned long flags;

    (void)inode;

    d = filp->private_data;

    if (d == NULL)
        return 0;

    /*
     * Stop buffering new data before flushing the FIFO.
     */
    atomic_set(&d->open_count, 0);

    spin_lock_irqsave(&d->rx_lock, flags);
    vedaurt_rx_reset_locked(d);
    spin_unlock_irqrestore(&d->rx_lock, flags);

    wake_up_interruptible(&d->rx_wait_queue);
    wake_up_interruptible(&d->tx_wait_queue);

    kref_put(
        &d->refcount,
        vedaurt_release_kref
    );

    filp->private_data = NULL;

    return 0;
}


static ssize_t vedaurt_read(
    struct file *filp,
    char __user *user_buffer,
    size_t count,
    loff_t *position
)
{
    struct vedaurt_data *d;
    u8 *temporary_buffer;
    unsigned long flags;
    size_t available;
    size_t read_length;
    ssize_t ret;

    (void)position;

    d = filp->private_data;

    if (d == NULL)
        return -ENODEV;

    if (count == 0U)
        return 0;

    if (count > VEDAURT_RX_BUFFER_SIZE - 1U)
        count = VEDAURT_RX_BUFFER_SIZE - 1U;

    ret = mutex_lock_interruptible(
        &d->rx_read_lock
    );

    if (ret)
        return ret;

    temporary_buffer = kmalloc(
        count,
        GFP_KERNEL
    );

    if (temporary_buffer == NULL)
    {
        ret = -ENOMEM;
        goto out_unlock;
    }

    for (;;)
    {
        if (!READ_ONCE(d->online))
        {
            ret = -ENODEV;
            goto out_free;
        }

        if (atomic_read(&d->open_count) == 0)
        {
            ret = -EPIPE;
            goto out_free;
        }

        available = vedaurt_rx_available(d);

        if (available > 0U)
            break;

        if (filp->f_flags & O_NONBLOCK)
        {
            ret = -EAGAIN;
            goto out_free;
        }

        ret = wait_event_interruptible(
            d->rx_wait_queue,
            !READ_ONCE(d->online)
            || atomic_read(&d->open_count) == 0
            || vedaurt_rx_available(d) > 0U
        );

        if (ret)
            goto out_free;
    }

    read_length = min(
        count,
        available
    );

    /*
     * Copy without consuming first.
     * This prevents data loss when copy_to_user() fails.
     */
    spin_lock_irqsave(&d->rx_lock, flags);

    vedaurt_rx_peek_locked(
        d,
        temporary_buffer,
        read_length
    );

    spin_unlock_irqrestore(&d->rx_lock, flags);

    if (copy_to_user(
            user_buffer,
            temporary_buffer,
            read_length
        ))
    {
        ret = -EFAULT;
        goto out_free;
    }

    spin_lock_irqsave(&d->rx_lock, flags);

    vedaurt_rx_drop_locked(
        d,
        read_length
    );

    spin_unlock_irqrestore(&d->rx_lock, flags);

    atomic64_add(
        read_length,
        &d->rx_read_count
    );

    ret = (ssize_t)read_length;

out_free:
    kfree(temporary_buffer);

out_unlock:
    mutex_unlock(&d->rx_read_lock);

    return ret;
}


static ssize_t vedaurt_write(
    struct file *filp,
    const char __user *user_buffer,
    size_t count,
    loff_t *position
)
{
    struct vedaurt_data *d;
    u8 *kernel_buffer;
    size_t offset;
    ssize_t ret;

    (void)position;

    d = filp->private_data;

    if (d == NULL)
        return -ENODEV;

    if (count == 0U)
        return 0;

    if (count > VEDAURT_MAX_WRITE_SIZE)
        return -EMSGSIZE;

    if (!READ_ONCE(d->online))
        return -ENODEV;

    kernel_buffer = memdup_user(
        user_buffer,
        count
    );

    if (IS_ERR(kernel_buffer))
        return PTR_ERR(kernel_buffer);

    ret = mutex_lock_interruptible(
        &d->tx_lock
    );

    if (ret)
        goto out_free;

    offset = 0U;

    while (offset < count)
    {
        ret = serdev_device_write(
            d->serdev,
            kernel_buffer + offset,
            count - offset,
            msecs_to_jiffies(
                VEDAURT_WRITE_TIMEOUT_MS
            )
        );

        if (ret < 0)
        {
            if (offset > 0U)
                ret = (ssize_t)offset;

            goto out_unlock;
        }

        if (ret == 0)
        {
            ret = offset > 0U
                ? (ssize_t)offset
                : -EIO;

            goto out_unlock;
        }

        offset += (size_t)ret;
    }

    ret = (ssize_t)offset;

out_unlock:
    mutex_unlock(&d->tx_lock);

out_free:
    kfree(kernel_buffer);

    return ret;
}


static __poll_t vedaurt_poll(
    struct file *filp,
    poll_table *wait
)
{
    struct vedaurt_data *d;
    __poll_t mask;

    d = filp->private_data;
    mask = 0;

    if (d == NULL)
        return POLLERR | POLLHUP;

    poll_wait(
        filp,
        &d->rx_wait_queue,
        wait
    );

    if (!READ_ONCE(d->online))
        return POLLERR | POLLHUP;

    if (atomic_read(&d->open_count) == 0)
        return POLLERR | POLLHUP;

    if (vedaurt_rx_available(d) > 0U)
        mask |= POLLIN | POLLRDNORM;

    return mask;
}


static const struct file_operations vedaurt_fops =
{
    .owner = THIS_MODULE,
    .open = vedaurt_open,
    .release = vedaurt_release,
    .read = vedaurt_read,
    .write = vedaurt_write,
    .poll = vedaurt_poll,
    .llseek = noop_llseek,
};


/* ------------------------------------------------------------------------- */
/* serdev probe/remove                                                       */
/* ------------------------------------------------------------------------- */

static int vedaurt_probe(
    struct serdev_device *serdev
)
{
    struct vedaurt_data *d;
    unsigned int actual_baudrate;
    int ret;

    d = kzalloc(
        sizeof(*d),
        GFP_KERNEL
    );

    if (d == NULL)
        return -ENOMEM;

    d->rx_buffer = kvzalloc(
        VEDAURT_RX_BUFFER_SIZE,
        GFP_KERNEL
    );

    if (d->rx_buffer == NULL)
    {
        kfree(d);
        return -ENOMEM;
    }

    d->serdev = serdev;

    spin_lock_init(&d->rx_lock);
    mutex_init(&d->rx_read_lock);
    mutex_init(&d->tx_lock);

    init_waitqueue_head(
        &d->rx_wait_queue
    );

    init_waitqueue_head(
        &d->tx_wait_queue
    );

    atomic64_set(
        &d->rx_received_count,
        0
    );

    atomic64_set(
        &d->rx_read_count,
        0
    );

    atomic64_set(
        &d->rx_overflow_count,
        0
    );

    atomic64_set(
        &d->rx_discarded_closed_count,
        0
    );

    atomic_set(
        &d->open_count,
        0
    );

    kref_init(
        &d->refcount
    );

    WRITE_ONCE(
        d->online,
        false
    );

    serdev_device_set_drvdata(
        serdev,
        d
    );

    serdev_device_set_client_ops(
        serdev,
        &vedaurt_serdev_ops
    );

    ret = serdev_device_open(
        serdev
    );

    if (ret)
        goto out_free;

    actual_baudrate = serdev_device_set_baudrate(
        serdev,
        VEDAURT_BAUDRATE
    );

    if (actual_baudrate != VEDAURT_BAUDRATE)
    {
        dev_err(
            &serdev->dev,
            "requested baudrate %u, got %u\n",
            VEDAURT_BAUDRATE,
            actual_baudrate
        );

        ret = -EINVAL;
        goto out_close;
    }

    serdev_device_set_flow_control(
        serdev,
        false
    );

    ret = serdev_device_set_parity(
        serdev,
        SERDEV_PARITY_NONE
    );

    if (ret)
        goto out_close;

    ret = alloc_chrdev_region(
        &d->devt,
        0,
        1,
        DEVICE_NAME
    );

    if (ret)
        goto out_close;

    cdev_init(
        &d->cdev,
        &vedaurt_fops
    );

    d->cdev.owner = THIS_MODULE;

    ret = cdev_add(
        &d->cdev,
        d->devt,
        1
    );

    if (ret)
        goto out_unregister;

    d->class = class_create(
        CLASS_NAME
    );

    if (IS_ERR(d->class))
    {
        ret = PTR_ERR(d->class);
        goto out_cdev;
    }

    d->device = device_create(
        d->class,
        &serdev->dev,
        d->devt,
        d,
        DEVICE_NAME
    );

    if (IS_ERR(d->device))
    {
        ret = PTR_ERR(d->device);
        goto out_class;
    }

    WRITE_ONCE(
        d->online,
        true
    );

    dev_info(
        &serdev->dev,
        "%s ready at %u baud, RX buffer=%u bytes\n",
        DEVICE_NAME,
        VEDAURT_BAUDRATE,
        VEDAURT_RX_BUFFER_SIZE
    );

    return 0;

out_class:
    class_destroy(
        d->class
    );

out_cdev:
    cdev_del(
        &d->cdev
    );

out_unregister:
    unregister_chrdev_region(
        d->devt,
        1
    );

out_close:
    serdev_device_close(
        serdev
    );

out_free:
    kref_put(
        &d->refcount,
        vedaurt_release_kref
    );

    return ret;
}


static void vedaurt_remove(
    struct serdev_device *serdev
)
{
    struct vedaurt_data *d;
    unsigned long flags;

    d = serdev_device_get_drvdata(
        serdev
    );

    if (d == NULL)
        return;

    WRITE_ONCE(
        d->online,
        false
    );

    atomic_set(
        &d->open_count,
        0
    );

    spin_lock_irqsave(
        &d->rx_lock,
        flags
    );

    vedaurt_rx_reset_locked(
        d
    );

    spin_unlock_irqrestore(
        &d->rx_lock,
        flags
    );

    wake_up_interruptible(
        &d->rx_wait_queue
    );

    wake_up_interruptible(
        &d->tx_wait_queue
    );

    device_destroy(
        d->class,
        d->devt
    );

    class_destroy(
        d->class
    );

    cdev_del(
        &d->cdev
    );

    unregister_chrdev_region(
        d->devt,
        1
    );

    serdev_device_close(
        serdev
    );

    dev_info(
        &serdev->dev,
        "%s removed: received=%lld read=%lld overflow=%lld closed_discard=%lld\n",
        DEVICE_NAME,
        atomic64_read(&d->rx_received_count),
        atomic64_read(&d->rx_read_count),
        atomic64_read(&d->rx_overflow_count),
        atomic64_read(&d->rx_discarded_closed_count)
    );

    kref_put(
        &d->refcount,
        vedaurt_release_kref
    );
}


/* ------------------------------------------------------------------------- */
/* Device-tree matching                                                      */
/* ------------------------------------------------------------------------- */

static const struct of_device_id vedaurt_dt_ids[] =
{
    {
        .compatible = "vedaurt"
    },
    {
    }
};

MODULE_DEVICE_TABLE(
    of,
    vedaurt_dt_ids
);


static struct serdev_device_driver vedaurt_driver =
{
    .driver =
    {
        .name = DEVICE_NAME,
        .of_match_table = vedaurt_dt_ids,
    },

    .probe = vedaurt_probe,
    .remove = vedaurt_remove,
};


module_serdev_device_driver(
    vedaurt_driver
);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lee Jaebaek");
MODULE_DESCRIPTION(
    "serdev UART byte-stream character driver with RX FIFO protection"
);
