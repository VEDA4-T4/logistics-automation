#include "app_queues.h"

#include <string.h>

#include "comm_tx_task.h"

osMessageQueueId_t controlCommandQueue;
osMessageQueueId_t sensorSnapshotQueue;
osMessageQueueId_t safetyEventQueue;
osMessageQueueId_t controlSafetyQueue;
osMessageQueueId_t unloadCommandQueue;
osMessageQueueId_t txSafetyQueue;
osMessageQueueId_t txResponseQueue;
osMessageQueueId_t txEventQueue;
osMessageQueueId_t healthEventQueue;
static volatile uint32_t healthEventDropCounts[APP_TASK_COUNT];

static const osMessageQueueAttr_t controlCommandQueueAttributes = {
    .name = "controlCommandQueue",
};

static const osMessageQueueAttr_t sensorSnapshotQueueAttributes = {
    .name = "sensorSnapshotQueue",
};

static const osMessageQueueAttr_t safetyEventQueueAttributes = {
    .name = "safetyEventQueue",
};

static const osMessageQueueAttr_t controlSafetyQueueAttributes = {
    .name = "controlSafetyQueue",
};

static const osMessageQueueAttr_t unloadCommandQueueAttributes = {
    .name = "unloadCommandQueue",
};

static const osMessageQueueAttr_t txSafetyQueueAttributes = {
    .name = "txSafetyQueue",
};

static const osMessageQueueAttr_t txResponseQueueAttributes = {
    .name = "txResponseQueue",
};

static const osMessageQueueAttr_t txEventQueueAttributes = {
    .name = "txEventQueue",
};

static const osMessageQueueAttr_t healthEventQueueAttributes = {
    .name = "healthEventQueue",
};

uint8_t AppQueues_AreReady(void) {
    return (controlCommandQueue != NULL && sensorSnapshotQueue != NULL && safetyEventQueue != NULL &&
            controlSafetyQueue != NULL && unloadCommandQueue != NULL && txSafetyQueue != NULL &&
            txResponseQueue != NULL &&
            txEventQueue != NULL && healthEventQueue != NULL)
               ? 1U
               : 0U;
}

uint8_t AppQueues_Init(void) {
    if (AppQueues_AreReady() != 0U) {
        return 1U;
    }

    (void)memset((void*)healthEventDropCounts, 0, sizeof(healthEventDropCounts));
    controlCommandQueue = osMessageQueueNew(APP_CONTROL_COMMAND_QUEUE_DEPTH, sizeof(app_control_command_t),
                                            &controlCommandQueueAttributes);
    sensorSnapshotQueue = osMessageQueueNew(APP_SENSOR_SNAPSHOT_QUEUE_DEPTH, sizeof(app_sensor_snapshot_t),
                                            &sensorSnapshotQueueAttributes);
    safetyEventQueue =
        osMessageQueueNew(APP_SAFETY_EVENT_QUEUE_DEPTH, sizeof(app_safety_event_t), &safetyEventQueueAttributes);
    controlSafetyQueue = osMessageQueueNew(APP_CONTROL_SAFETY_QUEUE_DEPTH, sizeof(app_control_safety_event_t),
                                           &controlSafetyQueueAttributes);
    unloadCommandQueue =
        osMessageQueueNew(APP_UNLOAD_COMMAND_QUEUE_DEPTH, sizeof(app_unload_command_t), &unloadCommandQueueAttributes);
    txSafetyQueue = osMessageQueueNew(APP_TX_SAFETY_QUEUE_DEPTH, sizeof(app_tx_event_t), &txSafetyQueueAttributes);
    txResponseQueue =
        osMessageQueueNew(APP_TX_RESPONSE_QUEUE_DEPTH, sizeof(app_tx_event_t), &txResponseQueueAttributes);
    txEventQueue = osMessageQueueNew(APP_TX_EVENT_QUEUE_DEPTH, sizeof(app_tx_event_t), &txEventQueueAttributes);
    healthEventQueue =
        osMessageQueueNew(APP_HEALTH_EVENT_QUEUE_DEPTH, sizeof(app_health_event_t), &healthEventQueueAttributes);

    return AppQueues_AreReady();
}

osStatus_t AppQueues_TryPutTx(const app_tx_event_t* event) {
    osMessageQueueId_t queue;
    osStatus_t status;

    if (event == NULL) {
        return osErrorParameter;
    }

    if (app_tx_event_is_emergency(event->type) != 0U) {
        queue = txSafetyQueue;
    } else if (app_tx_event_is_response(event->type) != 0U) {
        queue = txResponseQueue;
    } else {
        queue = txEventQueue;
    }
    if (queue == NULL) {
        return osErrorResource;
    }

    status = osMessageQueuePut(queue, event, app_tx_event_priority(event->type), 0U);
    if (status == osOK) {
        CommTxTask_NotifyQueueReady();
    }
    return status;
}

osStatus_t AppQueues_TryGetNextTx(app_tx_event_t* event) {
    osStatus_t status;

    if (event == NULL) {
        return osErrorParameter;
    }

    if (txSafetyQueue == NULL || txResponseQueue == NULL || txEventQueue == NULL) {
        return osErrorResource;
    }

    status = osMessageQueueGet(txSafetyQueue, event, NULL, 0U);
    if (status == osOK) {
        return osOK;
    }

    status = osMessageQueueGet(txResponseQueue, event, NULL, 0U);
    if (status == osOK) {
        return osOK;
    }

    return osMessageQueueGet(txEventQueue, event, NULL, 0U);
}

osStatus_t AppQueues_TryPutHealth(const app_health_event_t* event) {
    osStatus_t status;

    if (event == NULL || (uint32_t)event->source_task >= (uint32_t)APP_TASK_COUNT) {
        return osErrorParameter;
    }

    if (healthEventQueue == NULL) {
        ++healthEventDropCounts[event->source_task];
        return osErrorResource;
    }

    status = osMessageQueuePut(healthEventQueue, event, 0U, 0U);
    if (status != osOK) {
        ++healthEventDropCounts[event->source_task];
    }
    return status;
}

uint32_t AppQueues_GetHealthDropCount(app_task_id_t source_task) {
    if ((uint32_t)source_task >= (uint32_t)APP_TASK_COUNT) {
        return 0U;
    }

    return healthEventDropCounts[source_task];
}
