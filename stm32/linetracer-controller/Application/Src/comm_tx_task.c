#include "comm_tx_task.h"

#include <stddef.h>
#include <string.h>

#include "app_messages.h"
#include "app_queues.h"
#include "app_timing.h"
#include "cmsis_os2.h"
#include "comm_tx_config.h"
#include "comm_tx_logic.h"
#include "control_task.h"
#include "sensor_task.h"
#include "usart.h"

typedef enum {
    COMM_TX_STATE_IDLE = 0,
    COMM_TX_STATE_IN_FLIGHT,
    COMM_TX_STATE_ABORTING
} comm_tx_state_t;

#define COMM_TX_WAIT_FLAGS                                                                                         \
    (APP_COMM_TX_NOTIFY_QUEUE_READY | APP_COMM_TX_NOTIFY_TX_COMPLETE | APP_COMM_TX_NOTIFY_ABORT_COMPLETE)

static osThreadId_t commTxTaskId;
static volatile comm_tx_state_t commTxState = COMM_TX_STATE_IDLE;
static uint8_t commTxCurrentError = UART_ERROR_NONE;
static uint8_t commTxBuffer[UART_MAX_FRAME_SIZE];
static comm_tx_logic_t commTxLogic;
static comm_tx_observed_state_t commTxObservedState;
static comm_tx_stats_t commTxStats;

static uint8_t CommTxTask_TimeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1U : 0U;
}

static uint32_t CommTxTask_TimeUntil(uint32_t now_ms, uint32_t deadline_ms) {
    return (CommTxTask_TimeReached(now_ms, deadline_ms) != 0U) ? 0U : (deadline_ms - now_ms);
}

static uint32_t CommTxTask_MinWait(uint32_t first, uint32_t second) {
    return (first < second) ? first : second;
}

static void CommTxTask_PublishHealth(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = {0};

    if (healthEventQueue == NULL) {
        return;
    }

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_COMM_TX;
    (void)osMessageQueuePut(healthEventQueue, &event, 0U, 0U);
}

static void CommTxTask_AdvanceDeadline(uint32_t* deadline_ms, uint32_t period_ms, uint32_t now_ms) {
    if (deadline_ms == NULL || period_ms == 0U) {
        return;
    }

    do {
        *deadline_ms += period_ms;
    } while (CommTxTask_TimeReached(now_ms, *deadline_ms) != 0U);
}

static uint8_t CommTxTask_AbortTransmit(uint32_t now_ms) {
    uint32_t flags;

    commTxState = COMM_TX_STATE_ABORTING;
    (void)osThreadFlagsClear(APP_COMM_TX_NOTIFY_ABORT_COMPLETE);
    if (HAL_UART_AbortTransmit_IT(&huart6) != HAL_OK) {
        commTxState = COMM_TX_STATE_IDLE;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, (uint32_t)huart6.ErrorCode, now_ms);
        return 0U;
    }

    flags = osThreadFlagsWait(APP_COMM_TX_NOTIFY_ABORT_COMPLETE, osFlagsWaitAny, COMM_TX_ABORT_TIMEOUT_MS);
    commTxState = COMM_TX_STATE_IDLE;
    if ((flags & osFlagsError) != 0U) {
        ++commTxStats.abort_timeouts;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, flags, osKernelGetTickCount());
        return 0U;
    }
    return 1U;
}

static uint8_t CommTxTask_Transmit(const uint8_t* data, size_t length) {
    HAL_StatusTypeDef hal_status;
    uint32_t flags;
    uint32_t now_ms;

    if (data == NULL || length == 0U || length > UINT16_MAX) {
        return 0U;
    }

    (void)osThreadFlagsClear(APP_COMM_TX_NOTIFY_TX_COMPLETE | APP_COMM_TX_NOTIFY_ABORT_COMPLETE);
    commTxState = COMM_TX_STATE_IN_FLIGHT;
    hal_status = HAL_UART_Transmit_DMA(&huart6, (const uint8_t*)data, (uint16_t)length);
    if (hal_status != HAL_OK) {
        commTxState = COMM_TX_STATE_IDLE;
        ++commTxStats.dma_start_errors;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, (uint32_t)hal_status, osKernelGetTickCount());
        return 0U;
    }

    ++commTxStats.frames_started;
    flags = osThreadFlagsWait(APP_COMM_TX_NOTIFY_TX_COMPLETE, osFlagsWaitAny, COMM_TX_DMA_TIMEOUT_MS);
    now_ms = osKernelGetTickCount();

    if ((flags & osFlagsError) == 0U && (flags & APP_COMM_TX_NOTIFY_TX_COMPLETE) != 0U) {
        commTxState = COMM_TX_STATE_IDLE;
        commTxCurrentError = UART_ERROR_NONE;
        ++commTxStats.frames_completed;
        return 1U;
    }

    if (flags == osFlagsErrorTimeout) {
        ++commTxStats.dma_timeouts;
        commTxCurrentError = UART_ERROR_TIMEOUT;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_UART_TX_TIMEOUT, (uint32_t)length, now_ms);
    } else {
        commTxCurrentError = UART_ERROR_INTERNAL;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, flags, now_ms);
    }

    (void)CommTxTask_AbortTransmit(now_ms);
    return 0U;
}

static void CommTxTask_BuildHeartbeat(uint32_t now_ms, comm_tx_heartbeat_t* heartbeat) {
#if defined(CONTROL_TASK_SNAPSHOT_API_AVAILABLE)
    app_control_snapshot_t control_snapshot;
#endif
    app_sensor_snapshot_t sensor_snapshot;

    if (SensorTask_GetLatest(&sensor_snapshot)) {
        CommTxLogic_ObserveSensor(&commTxObservedState, &sensor_snapshot);
    }
#if defined(CONTROL_TASK_SNAPSHOT_API_AVAILABLE)
    if (ControlTask_GetLatest(&control_snapshot)) {
        CommTxLogic_ObserveControl(&commTxObservedState, &control_snapshot);
    }
#endif
    CommTxLogic_MakeHeartbeat(&commTxObservedState, now_ms, commTxCurrentError, heartbeat);
}

static uint8_t CommTxTask_SendHeartbeat(uint32_t now_ms) {
    comm_tx_heartbeat_t heartbeat;
    size_t length = 0U;
    uart_codec_result_t result;

    CommTxTask_BuildHeartbeat(now_ms, &heartbeat);
    result = CommTxLogic_EncodeHeartbeat(&commTxLogic, &heartbeat, commTxBuffer, sizeof(commTxBuffer), &length);
    if (result != UART_CODEC_OK) {
        ++commTxStats.encode_errors;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, (uint32_t)(-(int32_t)result), now_ms);
        return 0U;
    }

    if (CommTxTask_Transmit(commTxBuffer, length) == 0U) {
        return 0U;
    }
    ++commTxStats.heartbeats_sent;
    return 1U;
}

static void CommTxTask_RequeueFailedEvent(app_tx_event_t* event, uint32_t now_ms) {
    if (event == NULL) {
        return;
    }

    ++event->retry_count;
    if (event->retry_count <= COMM_TX_MAX_RETRIES) {
        osDelay(COMM_TX_RETRY_DELAY_MS);
        if (AppQueues_TryPutTx(event) == osOK) {
            return;
        }
    }

    ++commTxStats.dropped_events;
    CommTxTask_PublishHealth(APP_HEALTH_EVENT_UART_TX_TIMEOUT, (uint32_t)event->type, now_ms);
}

static uint8_t CommTxTask_SendSingleEvent(app_tx_event_t* event, uint32_t now_ms) {
    size_t length = 0U;
    uart_codec_result_t result;

    result = CommTxLogic_EncodeEvent(&commTxLogic, event, commTxBuffer, sizeof(commTxBuffer), &length);
    if (result != UART_CODEC_OK) {
        ++commTxStats.encode_errors;
        ++commTxStats.dropped_events;
        CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, (uint32_t)(-(int32_t)result), now_ms);
        return 0U;
    }

    if (CommTxTask_Transmit(commTxBuffer, length) == 0U) {
        CommTxTask_RequeueFailedEvent(event, osKernelGetTickCount());
        return 0U;
    }
    ++commTxStats.events_sent;
    return 1U;
}

static void CommTxTask_SendEvent(app_tx_event_t* event, uint32_t now_ms) {
    CommTxLogic_ObserveEvent(&commTxObservedState, event);
    (void)CommTxTask_SendSingleEvent(event, now_ms);
}

void CommTxTask_NotifyQueueReady(void) {
    if (commTxTaskId != NULL) {
        (void)osThreadFlagsSet(commTxTaskId, APP_COMM_TX_NOTIFY_QUEUE_READY);
    }
}

void CommTxTask_GetStats(comm_tx_stats_t* stats) {
    uint32_t primask;

    if (stats == NULL) {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    *stats = commTxStats;
    if (primask == 0U) {
        __enable_irq();
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    if (huart != NULL && huart->Instance == USART6 && commTxTaskId != NULL) {
        (void)osThreadFlagsSet(commTxTaskId, APP_COMM_TX_NOTIFY_TX_COMPLETE);
    }
}

void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef* huart) {
    if (huart != NULL && huart->Instance == USART6 && commTxTaskId != NULL) {
        (void)osThreadFlagsSet(commTxTaskId, APP_COMM_TX_NOTIFY_ABORT_COMPLETE);
    }
}

void StartCommTxTask(void* argument) {
    app_tx_event_t event;
    uint32_t now_ms;
    uint32_t next_heartbeat_ms;
    uint32_t next_alive_ms;

    (void)argument;
    commTxTaskId = osThreadGetId();
    commTxState = COMM_TX_STATE_IDLE;
    commTxCurrentError = UART_ERROR_NONE;
    (void)memset(&commTxStats, 0, sizeof(commTxStats));
    CommTxLogic_Init(&commTxLogic);
    CommTxLogic_InitObservedState(&commTxObservedState);

    now_ms = osKernelGetTickCount();
    next_heartbeat_ms = now_ms + COMM_TX_HEARTBEAT_INTERVAL_MS;
    next_alive_ms = now_ms + APP_TIMING_HEALTH_PERIOD_MS;

    for (;;) {
        uint32_t wait_ms;
        uint32_t flags;

        now_ms = osKernelGetTickCount();
        if (CommTxTask_TimeReached(now_ms, next_alive_ms) != 0U) {
            CommTxTask_PublishHealth(APP_HEALTH_EVENT_TASK_ALIVE, commTxStats.frames_completed, now_ms);
            CommTxTask_AdvanceDeadline(&next_alive_ms, APP_TIMING_HEALTH_PERIOD_MS, now_ms);
        }

        if (CommTxTask_TimeReached(now_ms, next_heartbeat_ms) != 0U) {
            (void)CommTxTask_SendHeartbeat(now_ms);
            now_ms = osKernelGetTickCount();
            CommTxTask_AdvanceDeadline(&next_heartbeat_ms, COMM_TX_HEARTBEAT_INTERVAL_MS, now_ms);
            continue;
        }

        if (AppQueues_TryGetNextTx(&event) == osOK) {
            CommTxTask_SendEvent(&event, now_ms);
            continue;
        }

        wait_ms = CommTxTask_MinWait(CommTxTask_TimeUntil(now_ms, next_heartbeat_ms),
                                     CommTxTask_TimeUntil(now_ms, next_alive_ms));
        flags = osThreadFlagsWait(COMM_TX_WAIT_FLAGS, osFlagsWaitAny, wait_ms);
        if ((flags & osFlagsError) != 0U && flags != osFlagsErrorTimeout) {
            CommTxTask_PublishHealth(APP_HEALTH_EVENT_INTERNAL_ERROR, flags, osKernelGetTickCount());
        }
    }
}
