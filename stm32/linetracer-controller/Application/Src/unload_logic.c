#include "unload_logic.h"

#include <string.h>

#include "unload_config.h"

static uint8_t UnloadLogic_TimeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1U : 0U;
}

static void UnloadLogic_PublishResult(unload_logic_context_t* context, app_unload_result_type_t type,
                                      uint32_t now_ms, uart_error_t error_code) {
    if (context == NULL || context->result_pending != 0U) {
        return;
    }

    context->pending_result.type = type;
    context->pending_result.requested_at_ms = context->active_command.requested_at_ms;
    context->pending_result.completed_at_ms = now_ms;
    context->pending_result.job_id = context->active_command.job_id;
    context->pending_result.route_id = context->active_command.route_id;
    context->pending_result.error_code = (uint8_t)error_code;
    context->result_pending = 1U;
}

void UnloadLogic_Init(unload_logic_context_t* context, uint32_t now_ms) {
    if (context == NULL) {
        return;
    }

    (void)memset(context, 0, sizeof(*context));
    context->state = UNLOAD_LOGIC_IDLE;
    context->state_entered_at_ms = now_ms;
    context->active_command.route_id = UART_LINETRACER_ROUTE_NONE;
    context->pending_result.route_id = UART_LINETRACER_ROUTE_NONE;
}

uint8_t UnloadLogic_Start(unload_logic_context_t* context, const app_unload_command_t* command, uint32_t now_ms) {
    if (context == NULL || command == NULL || command->type != APP_UNLOAD_COMMAND_START || context->active != 0U ||
        context->state != UNLOAD_LOGIC_IDLE || context->result_pending != 0U ||
        uart_linetracer_job_id_is_valid(command->job_id) == 0U ||
        uart_linetracer_route_is_valid(command->route_id) == 0U) {
        return 0U;
    }

    context->active_command = *command;
    context->state = UNLOAD_LOGIC_MOVING_TO_RELEASE;
    context->started_at_ms = now_ms;
    context->state_entered_at_ms = now_ms;
    context->active = 1U;
    context->load_present_seen = 0U;
    context->result_pending = 0U;
    return 1U;
}

void UnloadLogic_Abort(unload_logic_context_t* context, uint32_t now_ms, uart_error_t error_code) {
    if (context == NULL || context->active == 0U) {
        return;
    }

    context->active = 0U;
    context->state = UNLOAD_LOGIC_IDLE;
    context->started_at_ms = now_ms;
    context->state_entered_at_ms = now_ms;
    context->load_present_seen = 0U;
    UnloadLogic_PublishResult(context, APP_UNLOAD_RESULT_ABORTED, now_ms, error_code);
}

void UnloadLogic_Fail(unload_logic_context_t* context, uint32_t now_ms, uart_error_t error_code) {
    if (context == NULL || context->active == 0U) {
        return;
    }

    context->active = 0U;
    context->state = UNLOAD_LOGIC_FAILED;
    context->started_at_ms = now_ms;
    context->state_entered_at_ms = now_ms;
    context->load_present_seen = 0U;
    UnloadLogic_PublishResult(context, APP_UNLOAD_RESULT_FAILED, now_ms, error_code);
}

void UnloadLogic_Reset(unload_logic_context_t* context, uint32_t now_ms) {
    if (context == NULL) {
        return;
    }

    context->active = 0U;
    context->state = UNLOAD_LOGIC_IDLE;
    context->started_at_ms = now_ms;
    context->state_entered_at_ms = now_ms;
    context->load_present_seen = 0U;
    context->result_pending = 0U;
    context->active_command = (app_unload_command_t){ 0 };
    context->active_command.route_id = UART_LINETRACER_ROUTE_NONE;
}

void UnloadLogic_Update(unload_logic_context_t* context, uart_linetracer_load_state_t load_state, uint32_t now_ms) {
    if (context == NULL || context->active == 0U) {
        return;
    }

    if (load_state == UART_LINETRACER_LOAD_PRESENT) {
        context->load_present_seen = 1U;
    }

    if (UnloadLogic_TimeReached(now_ms, context->started_at_ms + UNLOAD_OPERATION_TIMEOUT_MS) != 0U) {
        context->active = 0U;
        context->state = UNLOAD_LOGIC_FAILED;
        context->state_entered_at_ms = now_ms;
        UnloadLogic_PublishResult(context, APP_UNLOAD_RESULT_TIMEOUT, now_ms, UART_ERROR_TIMEOUT);
        return;
    }

    switch (context->state) {
        case UNLOAD_LOGIC_MOVING_TO_RELEASE:
            if (UnloadLogic_TimeReached(now_ms, context->state_entered_at_ms + UNLOAD_SERVO_DEPLOY_MS) != 0U) {
                context->state = UNLOAD_LOGIC_WAITING_LOAD_OFF;
                context->state_entered_at_ms = now_ms;
            }
            break;

        case UNLOAD_LOGIC_WAITING_LOAD_OFF:
            if (context->load_present_seen == 0U || load_state != UART_LINETRACER_LOAD_EMPTY) {
                break;
            }

            /* SensorTask publishes load_state only after its FSR stability filter has settled. */
            context->state = UNLOAD_LOGIC_MOVING_HOME;
            context->state_entered_at_ms = now_ms;
            break;

        case UNLOAD_LOGIC_MOVING_HOME:
            if (UnloadLogic_TimeReached(now_ms, context->state_entered_at_ms + UNLOAD_SERVO_HOME_MS) != 0U) {
                context->active = 0U;
                context->state = UNLOAD_LOGIC_IDLE;
                context->state_entered_at_ms = now_ms;
                UnloadLogic_PublishResult(context, APP_UNLOAD_RESULT_COMPLETE, now_ms, UART_ERROR_NONE);
            }
            break;

        case UNLOAD_LOGIC_IDLE:
        case UNLOAD_LOGIC_FAILED:
        default:
            break;
    }
}

unload_servo_output_t UnloadLogic_GetServoOutput(const unload_logic_context_t* context) {
    if (context == NULL) {
        return UNLOAD_SERVO_OUTPUT_DISABLE;
    }

    switch (context->state) {
        case UNLOAD_LOGIC_MOVING_TO_RELEASE:
        case UNLOAD_LOGIC_WAITING_LOAD_OFF:
            return UNLOAD_SERVO_OUTPUT_RELEASE;

        case UNLOAD_LOGIC_MOVING_HOME:
            return UNLOAD_SERVO_OUTPUT_HOME;

        case UNLOAD_LOGIC_IDLE:
        case UNLOAD_LOGIC_FAILED:
        default:
            return UNLOAD_SERVO_OUTPUT_DISABLE;
    }
}

uint8_t UnloadLogic_GetPendingResult(const unload_logic_context_t* context, app_unload_result_t* result) {
    if (context == NULL || result == NULL || context->result_pending == 0U) {
        return 0U;
    }

    *result = context->pending_result;
    return 1U;
}

void UnloadLogic_AcknowledgeResult(unload_logic_context_t* context) {
    if (context != NULL) {
        context->result_pending = 0U;
    }
}
