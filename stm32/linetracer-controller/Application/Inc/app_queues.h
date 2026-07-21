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
#define APP_CONTROL_SAFETY_QUEUE_DEPTH 4U
#define APP_UNLOAD_COMMAND_QUEUE_DEPTH 4U
#define APP_TX_RESPONSE_QUEUE_DEPTH 8U
#define APP_TX_EVENT_QUEUE_DEPTH 16U
#define APP_HEALTH_EVENT_QUEUE_DEPTH 8U

/* Producers retain a response locally and retry without blocking control. */
#define APP_TX_RESPONSE_MAX_RETRIES 3U
#define APP_TX_RESPONSE_RETRY_DELAY_MS 10U

extern osMessageQueueId_t controlCommandQueue;
extern osMessageQueueId_t sensorSnapshotQueue;
/* SensorTask, CommRxTask and ControlTask produce; SafetyTask consumes. */
extern osMessageQueueId_t safetyEventQueue;
/* SafetyTask produces; ControlTask consumes before normal commands. */
extern osMessageQueueId_t controlSafetyQueue;
extern osMessageQueueId_t unloadCommandQueue;
/* ACK and STATUS responses; CommTxTask always services this queue first. */
extern osMessageQueueId_t txResponseQueue;
/* Heartbeat, state, job and fault events. */
extern osMessageQueueId_t txEventQueue;
extern osMessageQueueId_t healthEventQueue;

uint8_t AppQueues_Init(void);
uint8_t AppQueues_AreReady(void);
/* Routes ACK/STATUS to txResponseQueue and all other messages to txEventQueue. */
osStatus_t AppQueues_TryPutTx(const app_tx_event_t* event);
/* CommTxTask calls this repeatedly; command responses are returned first. */
osStatus_t AppQueues_TryGetNextTx(app_tx_event_t* event);

#ifdef __cplusplus
}
#endif

#endif /* APP_QUEUES_H */
