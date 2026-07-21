#include "control_task.h"

#include <stdint.h>

#include "app_queues.h"
#include "cmsis_os2.h"
#include "control_config.h"
#include "control_logic.h"

static control_context_t controlTaskContext;
static uart_linetracer_load_state_t controlTaskLoadState = UART_LINETRACER_LOAD_EMPTY;

typedef struct {
    app_tx_event_t event;
    uint32_t retry_at_ms;
    uint8_t active;
} control_pending_response_t;

static control_pending_response_t controlPendingResponses[CONTROL_TASK_PENDING_RESPONSE_CAPACITY];

static void ControlTask_PublishHealthEvent(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_CONTROL;
    (void)osMessageQueuePut(healthEventQueue, &event, 0U, 0U);
}

static uint8_t ControlTask_TimeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1U : 0U;
}

static uint8_t ControlTask_ScheduleResponseRetry(const app_tx_event_t* event, uint32_t now_ms) {
    uint32_t index;

    for (index = 0U; index < CONTROL_TASK_PENDING_RESPONSE_CAPACITY; ++index) {
        if (controlPendingResponses[index].active == 0U) {
            controlPendingResponses[index].event = *event;
            controlPendingResponses[index].event.retry_count = 0U;
            controlPendingResponses[index].retry_at_ms = now_ms + APP_TX_RESPONSE_RETRY_DELAY_MS;
            controlPendingResponses[index].active = 1U;
            return 1U;
        }
    }

    return 0U;
}

static void ControlTask_PublishTxEvent(const app_tx_event_t* event, uint32_t now_ms) {
    if (AppQueues_TryPutTx(event) == osOK) {
        return;
    }

    if (app_tx_event_is_response(event->type) != 0U && ControlTask_ScheduleResponseRetry(event, now_ms) != 0U) {
        return;
    }

    ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)event->type, now_ms);
}

static void ControlTask_ProcessPendingResponses(uint32_t now_ms) {
    uint32_t index;

    for (index = 0U; index < CONTROL_TASK_PENDING_RESPONSE_CAPACITY; ++index) {
        control_pending_response_t* pending = &controlPendingResponses[index];

        if (pending->active == 0U || ControlTask_TimeReached(now_ms, pending->retry_at_ms) == 0U) {
            continue;
        }

        ++pending->event.retry_count;
        if (AppQueues_TryPutTx(&pending->event) == osOK) {
            pending->active = 0U;
            continue;
        }

        if (pending->event.retry_count >= APP_TX_RESPONSE_MAX_RETRIES) {
            pending->active = 0U;
            ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_UART_TX_TIMEOUT, (uint32_t)pending->event.type, now_ms);
            continue;
        }

        pending->retry_at_ms = now_ms + APP_TX_RESPONSE_RETRY_DELAY_MS;
    }
}

static app_tx_event_t ControlTask_MakeTxEvent(app_tx_event_type_t type, const app_control_command_t* command,
                                              const control_command_result_t* result, uint32_t now_ms) {
    app_tx_event_t event = { 0 };

    event.type = type;
    event.created_at_ms = now_ms;
    event.job_id = controlTaskContext.active_job_id;
    event.original_payload_crc = command->original_payload_crc;
    event.route_id = controlTaskContext.active_route;
    event.state = linetracer_control_state_to_uart_state(controlTaskContext.state);
    event.load_state = controlTaskLoadState;
    event.request_sequence = command->sequence;
    event.original_command = command->original_command;
    if (event.original_command == UART_CMD_NONE) {
        event.original_command = ControlLogic_CommandToUartCommand(command->type);
    }
    event.original_payload_length = command->original_payload_length;
    event.status = result->status;
    event.error_code = result->error_code;
    return event;
}

static void ControlTask_PublishSafetyResetResult(const app_control_safety_event_t* safety_event, uint32_t now_ms) {
    app_tx_event_t event = { 0 };

    if (safety_event->type != APP_CONTROL_SAFETY_RESET_APPROVED &&
        safety_event->type != APP_CONTROL_SAFETY_RESET_REJECTED) {
        return;
    }

    event.type = APP_TX_EVENT_COMMAND_ACK;
    event.created_at_ms = now_ms;
    event.job_id = controlTaskContext.active_job_id;
    event.original_payload_crc = safety_event->original_payload_crc;
    event.route_id = controlTaskContext.active_route;
    event.state = linetracer_control_state_to_uart_state(controlTaskContext.state);
    event.load_state = controlTaskLoadState;
    event.request_sequence = safety_event->request_sequence;
    event.original_command = safety_event->original_command;
    if (event.original_command == UART_CMD_NONE) {
        event.original_command = UART_CMD_LINETRACER_RESET_SYSTEM;
    }
    event.original_payload_length = safety_event->original_payload_length;
    event.status = (safety_event->type == APP_CONTROL_SAFETY_RESET_APPROVED) ? UART_STATUS_ACK : UART_STATUS_NACK;
    event.error_code =
        (safety_event->type == APP_CONTROL_SAFETY_RESET_APPROVED) ? UART_ERROR_NONE : safety_event->error_code;
    if (safety_event->type == APP_CONTROL_SAFETY_RESET_REJECTED && event.error_code == UART_ERROR_NONE) {
        event.error_code = (controlTaskContext.safety_error_code != UART_ERROR_NONE)
                               ? controlTaskContext.safety_error_code
                               : UART_ERROR_BUSY;
    }

    ControlTask_PublishTxEvent(&event, now_ms);
}

static void ControlTask_ProcessSafetyEvents(void) {
    app_control_safety_event_t event;
    uint32_t processed = 0U;

    while (processed < APP_CONTROL_SAFETY_QUEUE_DEPTH &&
           osMessageQueueGet(controlSafetyQueue, &event, NULL, 0U) == osOK) {
        uint32_t now_ms = osKernelGetTickCount();

        if (ControlLogic_ApplySafetyEvent(&controlTaskContext, &event, now_ms) != 0U) {
            ControlTask_PublishSafetyResetResult(&event, now_ms);
        }
        ++processed;
    }
}

static void ControlTask_PublishLifecycleEvent(app_tx_event_type_t type, uint16_t job_id,
                                              uart_linetracer_route_t route_id, uart_status_t status,
                                              uart_error_t error_code, uint32_t now_ms) {
    app_tx_event_t event = { 0 };

    event.type = type;
    event.created_at_ms = now_ms;
    event.job_id = job_id;
    event.route_id = route_id;
    event.state = linetracer_control_state_to_uart_state(controlTaskContext.state);
    event.load_state = controlTaskLoadState;
    event.status = status;
    event.error_code = error_code;
    ControlTask_PublishTxEvent(&event, now_ms);
}

static void ControlTask_PublishStateChanged(uint32_t now_ms) {
    if (uart_linetracer_job_id_is_valid(controlTaskContext.active_job_id) == 0U ||
        uart_linetracer_route_is_valid(controlTaskContext.active_route) == 0U) {
        return;
    }

    ControlTask_PublishLifecycleEvent(APP_TX_EVENT_STATE_CHANGED, controlTaskContext.active_job_id,
                                      controlTaskContext.active_route, UART_STATUS_SUCCESS, UART_ERROR_NONE, now_ms);
}

static void ControlTask_PublishSafetyFault(linetracer_stop_reason_t reason, uint32_t now_ms) {
    app_safety_event_t safety_event = { 0 };
    app_safety_event_type_t type;

    if (reason == LINETRACER_STOP_REASON_LOAD_LOST) {
        type = APP_SAFETY_EVENT_LOAD_LOST;
    } else {
        type = APP_SAFETY_EVENT_MARKER_SEQUENCE;
    }

    safety_event.type = type;
    safety_event.occurred_at_ms = now_ms;
    safety_event.reason = reason;
    safety_event.source_task = APP_TASK_CONTROL;
    safety_event.error_code = UART_ERROR_SENSOR;
    safety_event.active = 1U;
    if (osMessageQueuePut(safetyEventQueue, &safety_event, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)type, now_ms);
    }

    ControlTask_PublishLifecycleEvent(APP_TX_EVENT_FAULT, controlTaskContext.active_job_id,
                                      controlTaskContext.active_route, UART_STATUS_ERROR, UART_ERROR_SENSOR, now_ms);
}

static void ControlTask_StartUnload(uint32_t now_ms) {
    app_unload_command_t command = { 0 };

    command.type = APP_UNLOAD_COMMAND_START;
    command.requested_at_ms = now_ms;
    command.job_id = controlTaskContext.active_job_id;
    command.route_id = controlTaskContext.active_route;
    if (osMessageQueuePut(unloadCommandQueue, &command, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)command.type, now_ms);
    }
}

static void ControlTask_ProcessRouteAction(route_action_t action, linetracer_control_state_t previous_state,
                                           const control_job_completion_t* completion, uint32_t now_ms) {
    if (previous_state != controlTaskContext.state && action != ROUTE_ACTION_JOB_COMPLETE) {
        ControlTask_PublishStateChanged(now_ms);
    }

    switch (action) {
        case ROUTE_ACTION_STOP_AT_PICKUP:
            ControlTask_PublishLifecycleEvent(APP_TX_EVENT_ARRIVED, controlTaskContext.active_job_id,
                                              controlTaskContext.active_route, UART_STATUS_SUCCESS, UART_ERROR_NONE,
                                              now_ms);
            break;

        case ROUTE_ACTION_TURN_AROUND:
            ControlTask_PublishLifecycleEvent(APP_TX_EVENT_LOAD_DETECTED, controlTaskContext.active_job_id,
                                              controlTaskContext.active_route, UART_STATUS_SUCCESS, UART_ERROR_NONE,
                                              now_ms);
            break;

        case ROUTE_ACTION_STOP_AT_DEST:
            ControlTask_PublishLifecycleEvent(APP_TX_EVENT_ARRIVED, controlTaskContext.active_job_id,
                                              controlTaskContext.active_route, UART_STATUS_SUCCESS, UART_ERROR_NONE,
                                              now_ms);
            ControlTask_StartUnload(now_ms);
            break;

        case ROUTE_ACTION_JOB_COMPLETE:
            if (completion != NULL && completion->completed != 0U) {
                ControlTask_PublishLifecycleEvent(APP_TX_EVENT_UNLOAD_COMPLETE, completion->job_id,
                                                  completion->route_id, UART_STATUS_SUCCESS, UART_ERROR_NONE, now_ms);
            }
            break;

        case ROUTE_ACTION_LOAD_LOST:
            ControlTask_PublishSafetyFault(LINETRACER_STOP_REASON_LOAD_LOST, now_ms);
            break;

        case ROUTE_ACTION_ERROR:
            ControlTask_PublishSafetyFault(LINETRACER_STOP_REASON_MARKER_SEQUENCE, now_ms);
            break;

        case ROUTE_ACTION_NONE:
        case ROUTE_ACTION_GO_STRAIGHT:
        case ROUTE_ACTION_TURN_LEFT:
        case ROUTE_ACTION_TURN_RIGHT:
        default:
            break;
    }
}

static void ControlTask_ProcessSensorSnapshots(void) {
    app_sensor_snapshot_t snapshot;
    uint32_t processed = 0U;

    while (processed < APP_SENSOR_SNAPSHOT_QUEUE_DEPTH &&
           osMessageQueueGet(sensorSnapshotQueue, &snapshot, NULL, 0U) == osOK) {
        uint32_t now_ms = osKernelGetTickCount();

        if (uart_linetracer_load_state_is_valid(snapshot.load_state) != 0U) {
            controlTaskLoadState = snapshot.load_state;
        }

        if ((snapshot.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U &&
            snapshot.line_state == LINETRACER_LINE_CENTERED) {
            linetracer_control_state_t previous_state = controlTaskContext.state;

            if (ControlLogic_CompleteTurn(&controlTaskContext, now_ms) != 0U &&
                previous_state != controlTaskContext.state) {
                ControlTask_PublishStateChanged(now_ms);
            }
        }

        if ((snapshot.event_flags & APP_SENSOR_EVENT_MARKER) != 0U && controlTaskContext.route_active != 0U) {
            linetracer_control_state_t previous_state = controlTaskContext.state;
            route_action_t action = ControlLogic_HandleMarker(&controlTaskContext, now_ms);

            ControlTask_ProcessRouteAction(action, previous_state, NULL, now_ms);
        }

        if (controlTaskContext.state == LINETRACER_CONTROL_WAITING_LOAD &&
            snapshot.load_state == UART_LINETRACER_LOAD_PRESENT) {
            linetracer_control_state_t previous_state = controlTaskContext.state;
            route_action_t action = ControlLogic_HandleLoadOn(&controlTaskContext, now_ms);

            ControlTask_ProcessRouteAction(action, previous_state, NULL, now_ms);
        }

        if ((controlTaskContext.state == LINETRACER_CONTROL_MOVING_TO_DEST ||
             controlTaskContext.state == LINETRACER_CONTROL_UNLOADING) &&
            snapshot.load_state == UART_LINETRACER_LOAD_EMPTY) {
            control_job_completion_t completion;
            linetracer_control_state_t previous_state = controlTaskContext.state;
            route_action_t action = ControlLogic_HandleLoadOff(&controlTaskContext, now_ms, &completion);

            ControlTask_ProcessRouteAction(action, previous_state, &completion, now_ms);
        }
        ++processed;
    }
}

static void ControlTask_PublishCommandResult(const app_control_command_t* command,
                                             const control_command_result_t* result, uint32_t now_ms) {
    app_tx_event_t event = ControlTask_MakeTxEvent(APP_TX_EVENT_COMMAND_ACK, command, result, now_ms);

    ControlTask_PublishTxEvent(&event, now_ms);

    if (result->status_requested != 0U && result->accepted != 0U) {
        event.type = APP_TX_EVENT_STATUS;
        ControlTask_PublishTxEvent(&event, now_ms);
    }

    if (result->state_changed != 0U && uart_linetracer_job_id_is_valid(controlTaskContext.active_job_id) != 0U &&
        uart_linetracer_route_is_valid(controlTaskContext.active_route) != 0U) {
        event.type = APP_TX_EVENT_STATE_CHANGED;
        event.status = UART_STATUS_SUCCESS;
        ControlTask_PublishTxEvent(&event, now_ms);
    }
}

static void ControlTask_PublishUnloadCommand(const control_command_result_t* result, uint32_t now_ms) {
    app_unload_command_t command = { 0 };

    if (result->unload_command == APP_UNLOAD_COMMAND_NONE) {
        return;
    }

    command.type = result->unload_command;
    command.requested_at_ms = now_ms;
    command.job_id = result->action_job_id;
    command.route_id = result->action_route_id;

    if (osMessageQueuePut(unloadCommandQueue, &command, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)result->unload_command, now_ms);
    }
}

static void ControlTask_ProcessCommands(void) {
    app_control_command_t command;
    uint32_t processed_commands = 0U;

    while (processed_commands < CONTROL_TASK_MAX_COMMANDS_PER_CYCLE &&
           osMessageQueueGet(controlCommandQueue, &command, NULL, 0U) == osOK) {
        uint32_t now_ms = osKernelGetTickCount();
        control_command_result_t result = ControlLogic_HandleCommand(&controlTaskContext, &command, now_ms);

        ControlTask_PublishCommandResult(&command, &result, now_ms);
        ControlTask_PublishUnloadCommand(&result, now_ms);
        processed_commands++;
    }
}

void StartControlTask(void* argument) {
    uint32_t next_wake_tick;
    uint32_t last_alive_tick;

    (void)argument;
    next_wake_tick = osKernelGetTickCount();
    last_alive_tick = next_wake_tick;
    ControlLogic_Init(&controlTaskContext, next_wake_tick);
    controlTaskLoadState = UART_LINETRACER_LOAD_EMPTY;

    for (;;) {
        uint32_t now_ms;

        ControlTask_ProcessSafetyEvents();
        now_ms = osKernelGetTickCount();
        ControlTask_ProcessPendingResponses(now_ms);
        ControlTask_ProcessSensorSnapshots();
        ControlTask_ProcessCommands();
        now_ms = osKernelGetTickCount();

        if ((uint32_t)(now_ms - last_alive_tick) >= CONTROL_TASK_ALIVE_INTERVAL_MS) {
            ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_TASK_ALIVE, (uint32_t)controlTaskContext.state, now_ms);
            last_alive_tick = now_ms;
        }

        next_wake_tick += APP_TIMING_CONTROL_PERIOD_MS;
        if (osDelayUntil(next_wake_tick) != osOK) {
            next_wake_tick = osKernelGetTickCount();
        }
    }
}
