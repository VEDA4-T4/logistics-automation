#include "control_task.h"

#include <stdint.h>

#include "app_queues.h"
#include "cmsis_os2.h"
#include "control_config.h"
#include "control_logic.h"

static control_context_t controlTaskContext;

static void ControlTask_PublishHealthEvent(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_CONTROL;
    (void)osMessageQueuePut(healthEventQueue, &event, 0U, 0U);
}

static void ControlTask_PublishTxEvent(const app_tx_event_t* event, uint32_t now_ms) {
    if (osMessageQueuePut(txEventQueue, event, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)event->type, now_ms);
    }
}

static app_tx_event_t ControlTask_MakeTxEvent(app_tx_event_type_t type, const app_control_command_t* command,
                                              const control_command_result_t* result, uint32_t now_ms) {
    app_tx_event_t event = { 0 };

    event.type = type;
    event.created_at_ms = now_ms;
    event.job_id = controlTaskContext.active_job_id;
    event.route_id = controlTaskContext.active_route;
    event.state = linetracer_control_state_to_uart_state(controlTaskContext.state);
    event.request_sequence = command->sequence;
    event.original_command = ControlLogic_CommandToUartCommand(command->type);
    event.status = result->status;
    event.error_code = result->error_code;
    return event;
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

    for (;;) {
        uint32_t now_ms;

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
