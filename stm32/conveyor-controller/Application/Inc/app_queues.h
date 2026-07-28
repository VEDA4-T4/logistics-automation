#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include "cmsis_os2.h"

extern osMessageQueueId_t uartRxQueueHandle;
extern osMessageQueueId_t inputControlQueueHandle;
extern osMessageQueueId_t sortingControlQueueHandle;
extern osMessageQueueId_t safetyCommandQueueHandle;

osStatus_t app_queues_init(void);

#endif /* APP_QUEUES_H */
