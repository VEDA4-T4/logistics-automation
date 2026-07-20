#ifndef TEST_FAKES_CMSIS_OS2_H
#define TEST_FAKES_CMSIS_OS2_H

#include <stdint.h>

typedef void* osMessageQueueId_t;
typedef int32_t osStatus_t;

typedef struct {
    const char* name;
    uint32_t attr_bits;
    void* cb_mem;
    uint32_t cb_size;
    void* mq_mem;
    uint32_t mq_size;
} osMessageQueueAttr_t;

#define osOK ((osStatus_t)0)
#define osError ((osStatus_t) - 1)
#define osErrorTimeout ((osStatus_t) - 2)
#define osErrorResource ((osStatus_t) - 3)
#define osErrorParameter ((osStatus_t) - 4)
#define osErrorNoMemory ((osStatus_t) - 5)

#define osWaitForever 0xFFFFFFFFU

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t* attr);
osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void* msg_ptr, uint8_t* msg_prio, uint32_t timeout);
osStatus_t osDelay(uint32_t ticks);

#endif /* TEST_FAKES_CMSIS_OS2_H */
