#ifndef APP_QUEUES_H
#define APP_QUEUES_H

#include <stdint.h>

#include "app_messages.h"
#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONTROL_COMMAND_QUEUE_DEPTH 8U
#define APP_SENSOR_SNAPSHOT_QUEUE_DEPTH 4U
#define APP_SAFETY_EVENT_QUEUE_DEPTH 8U
#define APP_UNLOAD_COMMAND_QUEUE_DEPTH 4U
#define APP_TX_EVENT_QUEUE_DEPTH 16U
#define APP_HEALTH_EVENT_QUEUE_DEPTH 8U

extern osMessageQueueId_t controlCommandQueue;
extern osMessageQueueId_t sensorSnapshotQueue;
extern osMessageQueueId_t safetyEventQueue;
extern osMessageQueueId_t unloadCommandQueue;
extern osMessageQueueId_t txEventQueue;
extern osMessageQueueId_t healthEventQueue;

uint8_t AppQueues_Init(void);
uint8_t AppQueues_AreReady(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_QUEUES_H */
