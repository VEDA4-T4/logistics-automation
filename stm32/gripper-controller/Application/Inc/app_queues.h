#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include "cmsis_os2.h"

extern osMessageQueueId_t uartRxQueueHandle;
extern osMessageQueueId_t gripperControlQueueHandle;
extern osMessageQueueId_t safetyCommandQueueHandle;
extern osMessageQueueId_t commTxQueueHandle;

osStatus_t app_queues_init(void);

#endif /* APP_QUEUES_H */
