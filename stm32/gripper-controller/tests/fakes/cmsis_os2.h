#ifndef TEST_CMSIS_OS2_H
#define TEST_CMSIS_OS2_H

#include <stdint.h>

typedef void* osMessageQueueId_t;
typedef void* osThreadId_t;

#define osFlagsWaitAny 0x00000000U
#define osFlagsError 0x80000000U
#define osFlagsErrorTimeout 0xFFFFFFFEU

typedef enum { osOK = 0, osError = -1 } osStatus_t;

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void* message, uint8_t priority, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void* message, uint8_t* priority, uint32_t timeout);
osThreadId_t osThreadGetId(void);
uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags);
uint32_t osThreadFlagsClear(uint32_t flags);
uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout);
void osDelay(uint32_t ticks);

#endif /* TEST_CMSIS_OS2_H */
