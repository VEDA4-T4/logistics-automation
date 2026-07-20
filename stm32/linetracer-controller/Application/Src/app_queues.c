#include "app_queues.h"

osMessageQueueId_t controlCommandQueue;
osMessageQueueId_t sensorSnapshotQueue;
osMessageQueueId_t safetyEventQueue;
osMessageQueueId_t unloadCommandQueue;
osMessageQueueId_t txEventQueue;
osMessageQueueId_t healthEventQueue;

static const osMessageQueueAttr_t controlCommandQueueAttributes = {
    .name = "controlCommandQueue",
};

static const osMessageQueueAttr_t sensorSnapshotQueueAttributes = {
    .name = "sensorSnapshotQueue",
};

static const osMessageQueueAttr_t safetyEventQueueAttributes = {
    .name = "safetyEventQueue",
};

static const osMessageQueueAttr_t unloadCommandQueueAttributes = {
    .name = "unloadCommandQueue",
};

static const osMessageQueueAttr_t txEventQueueAttributes = {
    .name = "txEventQueue",
};

static const osMessageQueueAttr_t healthEventQueueAttributes = {
    .name = "healthEventQueue",
};

uint8_t AppQueues_AreReady(void) {
    return (controlCommandQueue != NULL && sensorSnapshotQueue != NULL && safetyEventQueue != NULL &&
            unloadCommandQueue != NULL && txEventQueue != NULL && healthEventQueue != NULL)
               ? 1U
               : 0U;
}

uint8_t AppQueues_Init(void) {
    if (AppQueues_AreReady() != 0U) {
        return 1U;
    }

    controlCommandQueue = osMessageQueueNew(APP_CONTROL_COMMAND_QUEUE_DEPTH, sizeof(app_control_command_t),
                                             &controlCommandQueueAttributes);
    sensorSnapshotQueue = osMessageQueueNew(APP_SENSOR_SNAPSHOT_QUEUE_DEPTH, sizeof(app_sensor_snapshot_t),
                                             &sensorSnapshotQueueAttributes);
    safetyEventQueue = osMessageQueueNew(APP_SAFETY_EVENT_QUEUE_DEPTH, sizeof(app_safety_event_t),
                                          &safetyEventQueueAttributes);
    unloadCommandQueue = osMessageQueueNew(APP_UNLOAD_COMMAND_QUEUE_DEPTH, sizeof(app_unload_command_t),
                                            &unloadCommandQueueAttributes);
    txEventQueue = osMessageQueueNew(APP_TX_EVENT_QUEUE_DEPTH, sizeof(app_tx_event_t), &txEventQueueAttributes);
    healthEventQueue = osMessageQueueNew(APP_HEALTH_EVENT_QUEUE_DEPTH, sizeof(app_health_event_t),
                                          &healthEventQueueAttributes);

    return AppQueues_AreReady();
}
