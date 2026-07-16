// SPDX-License-Identifier: GPL-2.0-only
/*
 * vedauart.c - serdev-based UART byte-stream character driver
 *
 * Raspberry Pi <-> STM32 UART byte-stream driver. UART frame parsing,
 * CRC validation, ACK, and retry policy are handled in user space.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#define DEVICE_NAME "vedauart"
#define CLASS_NAME "vedauart_class"

#define VEDAUART_BAUDRATE 115200U
#define VEDAUART_BAUDRATE_TOLERANCE_PERCENT 2U
#define VEDAUART_WRITE_TIMEOUT_MS 1000U

/* One maximum UART frame is 135 bytes. Keep enough room for RX bursts. */
#define VEDAUART_RX_BUFFER_SIZE 65536U
#define VEDAUART_MAX_WRITE_SIZE 4096U

/* serdev receive_buf() return type changed across kernel versions. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
typedef size_t vedauart_receive_result_t;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
typedef ssize_t vedauart_receive_result_t;
#else
typedef int vedauart_receive_result_t;
#endif

struct vedauart_data {
	struct serdev_device *serdev;

	struct cdev cdev;
	struct class *class;
	struct device *device;
	dev_t devt;

	/* One byte is reserved to distinguish full and empty states. */
	u8 *rx_buffer;
	size_t rx_head;
	size_t rx_tail;
	spinlock_t rx_lock;
	struct mutex rx_read_lock;
	wait_queue_head_t rx_wait_queue;

	atomic64_t rx_received_count;
	atomic64_t rx_read_count;
	atomic64_t rx_overflow_count;
	atomic64_t rx_discarded_closed_count;

	/* Preserve write boundaries and expose TX readiness to poll users. */
	struct mutex tx_lock;
	wait_queue_head_t tx_wait_queue;
	atomic_t tx_ready;

	struct kref refcount;
	atomic_t open_count;
	bool online;
};

static size_t vedauart_rx_count_locked(const struct vedauart_data *data)
{
	if (data->rx_head >= data->rx_tail)
		return data->rx_head - data->rx_tail;

	return VEDAUART_RX_BUFFER_SIZE - data->rx_tail + data->rx_head;
}

static size_t vedauart_rx_space_locked(const struct vedauart_data *data)
{
	return VEDAUART_RX_BUFFER_SIZE - 1U -
	       vedauart_rx_count_locked(data);
}

static bool vedauart_rx_push_locked(struct vedauart_data *data, u8 byte)
{
	size_t next_head;

	if (vedauart_rx_space_locked(data) == 0U)
		return false;

	data->rx_buffer[data->rx_head] = byte;
	next_head = data->rx_head + 1U;
	if (next_head >= VEDAUART_RX_BUFFER_SIZE)
		next_head = 0U;
	data->rx_head = next_head;

	return true;
}

static void vedauart_rx_reset_locked(struct vedauart_data *data)
{
	data->rx_head = 0U;
	data->rx_tail = 0U;
}

static void vedauart_rx_peek_locked(const struct vedauart_data *data,
				    u8 *destination, size_t length)
{
	size_t index = data->rx_tail;
	size_t i;

	for (i = 0U; i < length; i++) {
		destination[i] = data->rx_buffer[index];
		index++;
		if (index >= VEDAUART_RX_BUFFER_SIZE)
			index = 0U;
	}
}

static void vedauart_rx_drop_locked(struct vedauart_data *data, size_t length)
{
	data->rx_tail += length;
	if (data->rx_tail >= VEDAUART_RX_BUFFER_SIZE)
		data->rx_tail -= VEDAUART_RX_BUFFER_SIZE;
}

static size_t vedauart_rx_available(struct vedauart_data *data)
{
	unsigned long flags;
	size_t count;

	spin_lock_irqsave(&data->rx_lock, flags);
	count = vedauart_rx_count_locked(data);
	spin_unlock_irqrestore(&data->rx_lock, flags);

	return count;
}

static void vedauart_release_kref(struct kref *refcount)
{
	struct vedauart_data *data;

	data = container_of(refcount, struct vedauart_data, refcount);
	kvfree(data->rx_buffer);
	kfree(data);
}

static vedauart_receive_result_t
vedauart_receive_buf(struct serdev_device *serdev, const u8 *buffer,
		     size_t count)
{
	struct vedauart_data *data = serdev_device_get_drvdata(serdev);
	unsigned long flags;
	size_t dropped = 0U;
	size_t accepted = 0U;
	size_t i;

	if (!data)
		return (vedauart_receive_result_t)count;

	atomic64_add(count, &data->rx_received_count);

	if (!READ_ONCE(data->online) ||
	    atomic_read(&data->open_count) == 0) {
		atomic64_add(count, &data->rx_discarded_closed_count);
		return (vedauart_receive_result_t)count;
	}

	spin_lock_irqsave(&data->rx_lock, flags);

	/* Recheck under the FIFO lock to close the release/remove race. */
	if (!READ_ONCE(data->online) ||
	    atomic_read(&data->open_count) == 0) {
		spin_unlock_irqrestore(&data->rx_lock, flags);
		atomic64_add(count, &data->rx_discarded_closed_count);
		return (vedauart_receive_result_t)count;
	}

	for (i = 0U; i < count; i++) {
		if (vedauart_rx_push_locked(data, buffer[i]))
			accepted++;
		else
			dropped++;
	}

	spin_unlock_irqrestore(&data->rx_lock, flags);

	if (dropped > 0U) {
		atomic64_add(dropped, &data->rx_overflow_count);
		dev_warn_ratelimited(&serdev->dev,
				     "RX buffer overflow: dropped %zu bytes, total=%lld\n",
				     dropped,
				     atomic64_read(&data->rx_overflow_count));
	}

	if (accepted > 0U)
		wake_up_interruptible(&data->rx_wait_queue);

	/* The callback consumed every byte, including bytes counted as dropped. */
	return (vedauart_receive_result_t)count;
}

static void vedauart_write_wakeup(struct serdev_device *serdev)
{
	struct vedauart_data *data = serdev_device_get_drvdata(serdev);

	/* Required by serdev_device_write() to complete synchronous writes. */
	serdev_device_write_wakeup(serdev);

	if (!data)
		return;

	atomic_set(&data->tx_ready, 1);
	wake_up_interruptible(&data->tx_wait_queue);
}

static const struct serdev_device_ops vedauart_serdev_ops = {
	.receive_buf = vedauart_receive_buf,
	.write_wakeup = vedauart_write_wakeup,
};

static int vedauart_open(struct inode *inode, struct file *file)
{
	struct vedauart_data *data;
	unsigned long flags;

	data = container_of(inode->i_cdev, struct vedauart_data, cdev);
	if (!READ_ONCE(data->online))
		return -ENODEV;

	if (!kref_get_unless_zero(&data->refcount))
		return -ENODEV;

	/* Multiple readers would consume bytes from each other. */
	if (atomic_cmpxchg(&data->open_count, 0, 1) != 0) {
		kref_put(&data->refcount, vedauart_release_kref);
		return -EBUSY;
	}

	if (!READ_ONCE(data->online)) {
		atomic_set(&data->open_count, 0);
		kref_put(&data->refcount, vedauart_release_kref);
		return -ENODEV;
	}

	spin_lock_irqsave(&data->rx_lock, flags);
	vedauart_rx_reset_locked(data);
	spin_unlock_irqrestore(&data->rx_lock, flags);

	file->private_data = data;
	return nonseekable_open(inode, file);
}

static int vedauart_release(struct inode *inode, struct file *file)
{
	struct vedauart_data *data = file->private_data;
	unsigned long flags;

	(void)inode;

	if (!data)
		return 0;

	atomic_set(&data->open_count, 0);

	mutex_lock(&data->rx_read_lock);
	spin_lock_irqsave(&data->rx_lock, flags);
	vedauart_rx_reset_locked(data);
	spin_unlock_irqrestore(&data->rx_lock, flags);
	mutex_unlock(&data->rx_read_lock);

	wake_up_interruptible(&data->rx_wait_queue);
	wake_up_interruptible(&data->tx_wait_queue);

	file->private_data = NULL;
	kref_put(&data->refcount, vedauart_release_kref);

	return 0;
}

static ssize_t vedauart_read(struct file *file, char __user *user_buffer,
			     size_t count, loff_t *position)
{
	struct vedauart_data *data = file->private_data;
	unsigned long flags;
	u8 *temporary_buffer;
	size_t available;
	size_t read_length;
	ssize_t ret;

	(void)position;

	if (!data)
		return -ENODEV;
	if (count == 0U)
		return 0;
	if (count > VEDAUART_RX_BUFFER_SIZE - 1U)
		count = VEDAUART_RX_BUFFER_SIZE - 1U;

	ret = mutex_lock_interruptible(&data->rx_read_lock);
	if (ret)
		return ret;

	temporary_buffer = kmalloc(count, GFP_KERNEL);
	if (!temporary_buffer) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	for (;;) {
		if (!READ_ONCE(data->online)) {
			ret = -ENODEV;
			goto out_free;
		}
		if (atomic_read(&data->open_count) == 0) {
			ret = -EPIPE;
			goto out_free;
		}

		spin_lock_irqsave(&data->rx_lock, flags);
		available = vedauart_rx_count_locked(data);
		if (available > 0U) {
			read_length = min(count, available);
			vedauart_rx_peek_locked(data, temporary_buffer,
					       read_length);
			spin_unlock_irqrestore(&data->rx_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&data->rx_lock, flags);

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_free;
		}

		ret = wait_event_interruptible(
			data->rx_wait_queue,
			!READ_ONCE(data->online) ||
				atomic_read(&data->open_count) == 0 ||
				vedauart_rx_available(data) > 0U);
		if (ret)
			goto out_free;
	}

	/* Do not consume bytes when the user-space copy fails. */
	if (copy_to_user(user_buffer, temporary_buffer, read_length)) {
		ret = -EFAULT;
		goto out_free;
	}

	spin_lock_irqsave(&data->rx_lock, flags);
	vedauart_rx_drop_locked(data, read_length);
	spin_unlock_irqrestore(&data->rx_lock, flags);

	atomic64_add(read_length, &data->rx_read_count);
	ret = (ssize_t)read_length;

out_free:
	kfree(temporary_buffer);
out_unlock:
	mutex_unlock(&data->rx_read_lock);
	return ret;
}

static ssize_t vedauart_write(struct file *file,
			      const char __user *user_buffer, size_t count,
			      loff_t *position)
{
	struct vedauart_data *data = file->private_data;
	u8 *kernel_buffer;
	ssize_t ret;

	(void)position;

	if (!data)
		return -ENODEV;
	if (count == 0U)
		return 0;
	if (count > VEDAUART_MAX_WRITE_SIZE)
		return -EMSGSIZE;

	kernel_buffer = memdup_user(user_buffer, count);
	if (IS_ERR(kernel_buffer))
		return PTR_ERR(kernel_buffer);

	ret = mutex_lock_interruptible(&data->tx_lock);
	if (ret)
		goto out_free;

	/* remove() sets online=false before waiting for this lock. */
	if (!READ_ONCE(data->online)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	atomic_set(&data->tx_ready, 0);
	if (file->f_flags & O_NONBLOCK) {
		ret = serdev_device_write_buf(data->serdev, kernel_buffer,
					      count);
		if (ret == 0)
			ret = -EAGAIN;
	} else {
		/* This API already loops until completion, timeout, or signal. */
		ret = serdev_device_write(
			data->serdev, kernel_buffer, count,
			msecs_to_jiffies(VEDAUART_WRITE_TIMEOUT_MS));
	}

	if ((ret < 0 && ret != -EAGAIN) || (size_t)ret == count) {
		atomic_set(&data->tx_ready, 1);
		wake_up_interruptible(&data->tx_wait_queue);
	}

out_unlock:
	mutex_unlock(&data->tx_lock);
out_free:
	kfree(kernel_buffer);
	return ret;
}

static __poll_t vedauart_poll(struct file *file, poll_table *wait)
{
	struct vedauart_data *data = file->private_data;
	__poll_t mask = 0;

	if (!data)
		return POLLERR | POLLHUP;

	poll_wait(file, &data->rx_wait_queue, wait);
	poll_wait(file, &data->tx_wait_queue, wait);

	if (!READ_ONCE(data->online) ||
	    atomic_read(&data->open_count) == 0)
		return POLLERR | POLLHUP;

	if (vedauart_rx_available(data) > 0U)
		mask |= POLLIN | POLLRDNORM;
	if (atomic_read(&data->tx_ready) != 0)
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static const struct file_operations vedauart_fops = {
	.owner = THIS_MODULE,
	.open = vedauart_open,
	.release = vedauart_release,
	.read = vedauart_read,
	.write = vedauart_write,
	.poll = vedauart_poll,
	.llseek = noop_llseek,
};

static bool vedauart_baudrate_is_acceptable(unsigned int requested,
					    unsigned int actual)
{
	unsigned int difference;
	unsigned int tolerance;

	if (actual == 0U)
		return false;

	difference = requested > actual ? requested - actual : actual - requested;
	tolerance = requested * VEDAUART_BAUDRATE_TOLERANCE_PERCENT / 100U;

	return difference <= tolerance;
}

static int vedauart_probe(struct serdev_device *serdev)
{
	struct vedauart_data *data;
	unsigned int actual_baudrate;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->rx_buffer = kvzalloc(VEDAUART_RX_BUFFER_SIZE, GFP_KERNEL);
	if (!data->rx_buffer) {
		kfree(data);
		return -ENOMEM;
	}

	data->serdev = serdev;
	spin_lock_init(&data->rx_lock);
	mutex_init(&data->rx_read_lock);
	mutex_init(&data->tx_lock);
	init_waitqueue_head(&data->rx_wait_queue);
	init_waitqueue_head(&data->tx_wait_queue);
	atomic64_set(&data->rx_received_count, 0);
	atomic64_set(&data->rx_read_count, 0);
	atomic64_set(&data->rx_overflow_count, 0);
	atomic64_set(&data->rx_discarded_closed_count, 0);
	atomic_set(&data->tx_ready, 1);
	atomic_set(&data->open_count, 0);
	kref_init(&data->refcount);
	WRITE_ONCE(data->online, false);

	serdev_device_set_drvdata(serdev, data);
	serdev_device_set_client_ops(serdev, &vedauart_serdev_ops);

	ret = serdev_device_open(serdev);
	if (ret)
		goto out_clear_drvdata;

	actual_baudrate =
		serdev_device_set_baudrate(serdev, VEDAUART_BAUDRATE);
	if (!vedauart_baudrate_is_acceptable(VEDAUART_BAUDRATE,
					      actual_baudrate)) {
		dev_err(&serdev->dev, "requested baudrate %u, got %u\n",
			VEDAUART_BAUDRATE, actual_baudrate);
		ret = -EINVAL;
		goto out_close;
	}

	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		goto out_close;

	ret = alloc_chrdev_region(&data->devt, 0, 1, DEVICE_NAME);
	if (ret)
		goto out_close;

	cdev_init(&data->cdev, &vedauart_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_add(&data->cdev, data->devt, 1);
	if (ret)
		goto out_unregister;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	data->class = class_create(THIS_MODULE, CLASS_NAME);
#else
	data->class = class_create(CLASS_NAME);
#endif
	if (IS_ERR(data->class)) {
		ret = PTR_ERR(data->class);
		goto out_cdev;
	}

	data->device = device_create(data->class, &serdev->dev, data->devt,
				     data, DEVICE_NAME);
	if (IS_ERR(data->device)) {
		ret = PTR_ERR(data->device);
		goto out_class;
	}

	WRITE_ONCE(data->online, true);
	dev_info(&serdev->dev,
		 "%s ready at %u baud, RX buffer=%u bytes\n", DEVICE_NAME,
		 actual_baudrate, VEDAUART_RX_BUFFER_SIZE);

	return 0;

out_class:
	class_destroy(data->class);
out_cdev:
	cdev_del(&data->cdev);
out_unregister:
	unregister_chrdev_region(data->devt, 1);
out_close:
	serdev_device_close(serdev);
out_clear_drvdata:
	serdev_device_set_drvdata(serdev, NULL);
	kref_put(&data->refcount, vedauart_release_kref);
	return ret;
}

static void vedauart_remove(struct serdev_device *serdev)
{
	struct vedauart_data *data = serdev_device_get_drvdata(serdev);
	unsigned long flags;

	if (!data)
		return;

	/* Stop new I/O and release waiters before waiting for active calls. */
	WRITE_ONCE(data->online, false);
	wake_up_interruptible(&data->rx_wait_queue);
	wake_up_interruptible(&data->tx_wait_queue);

	device_destroy(data->class, data->devt);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devt, 1);
	class_destroy(data->class);

	/* A synchronous write can remain inside serdev for up to its timeout. */
	mutex_lock(&data->tx_lock);
	serdev_device_write_flush(serdev);
	serdev_device_close(serdev);
	mutex_unlock(&data->tx_lock);

	/* A blocked reader wakes on online=false and drops rx_read_lock. */
	mutex_lock(&data->rx_read_lock);
	spin_lock_irqsave(&data->rx_lock, flags);
	vedauart_rx_reset_locked(data);
	spin_unlock_irqrestore(&data->rx_lock, flags);
	mutex_unlock(&data->rx_read_lock);

	dev_info(&serdev->dev,
		 "%s removed: received=%lld read=%lld overflow=%lld closed_discard=%lld\n",
		 DEVICE_NAME, atomic64_read(&data->rx_received_count),
		 atomic64_read(&data->rx_read_count),
		 atomic64_read(&data->rx_overflow_count),
		 atomic64_read(&data->rx_discarded_closed_count));

	serdev_device_set_drvdata(serdev, NULL);
	kref_put(&data->refcount, vedauart_release_kref);
}

static const struct of_device_id vedauart_dt_ids[] = {
	{ .compatible = "veda,vedauart" },
	{ }
};
MODULE_DEVICE_TABLE(of, vedauart_dt_ids);

static struct serdev_device_driver vedauart_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = vedauart_dt_ids,
	},
	.probe = vedauart_probe,
	.remove = vedauart_remove,
};
module_serdev_device_driver(vedauart_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lee Jaebaek");
MODULE_DESCRIPTION("serdev UART byte-stream character driver");
