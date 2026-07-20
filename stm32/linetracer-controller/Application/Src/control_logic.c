#include "control_logic.h"

#include "logistics/contracts/uart/linetracer_commands.h"

static uint8_t ControlLogic_HasActiveJob(const control_context_t* context) {
    if (context == NULL) {
        return 0U;
    }

    return (context->route_active != 0U && uart_linetracer_job_id_is_valid(context->active_job_id) != 0U &&
            uart_linetracer_route_is_valid(context->active_route) != 0U)
               ? 1U
               : 0U;
}

static uart_linetracer_position_t ControlLogic_RouteDestination(uart_linetracer_route_t route_id) {
    switch (route_id) {
        case UART_LINETRACER_ROUTE_A:
            return UART_LINETRACER_POSITION_DEST_A;

        case UART_LINETRACER_ROUTE_B:
            return UART_LINETRACER_POSITION_DEST_B;

        case UART_LINETRACER_ROUTE_C:
            return UART_LINETRACER_POSITION_DEST_C;

        default:
            return UART_LINETRACER_POSITION_NONE;
    }
}

static uint8_t ControlLogic_CurrentPositionMatchesRoute(const control_context_t* context) {
    uart_linetracer_position_t destination;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U) {
        return 0U;
    }

    destination = ControlLogic_RouteDestination(context->active_route);
    return (destination != UART_LINETRACER_POSITION_NONE && context->current_position == destination) ? 1U : 0U;
}

static control_command_result_t ControlLogic_MakeResult(const control_context_t* context) {
    control_command_result_t result = { 0 };

    result.status = UART_STATUS_NACK;
    result.error_code = UART_ERROR_INTERNAL;
    result.unload_command = APP_UNLOAD_COMMAND_NONE;
    result.action_route_id = UART_LINETRACER_ROUTE_NONE;

    if (context != NULL) {
        result.previous_state = context->state;
        result.current_state = context->state;
    } else {
        result.previous_state = LINETRACER_CONTROL_ERROR;
        result.current_state = LINETRACER_CONTROL_ERROR;
    }

    return result;
}

static void ControlLogic_Accept(control_command_result_t* result) {
    result->accepted = 1U;
    result->status = UART_STATUS_ACK;
    result->error_code = UART_ERROR_NONE;
}

static void ControlLogic_Reject(control_command_result_t* result, uart_status_t status, uart_error_t error_code) {
    result->accepted = 0U;
    result->status = status;
    result->error_code = error_code;
}

static void ControlLogic_UpdateResultState(control_command_result_t* result, const control_context_t* context) {
    result->current_state = context->state;
    result->state_changed = (result->previous_state != result->current_state) ? 1U : 0U;
}

static uint8_t ControlLogic_CommandTimedOut(const app_control_command_t* command, uint32_t now_ms) {
    return ((uint32_t)(now_ms - command->received_at_ms) > UART_COMMAND_TIMEOUT_MS) ? 1U : 0U;
}

uint8_t ControlLogic_StateCanResume(linetracer_control_state_t state) {
    switch (state) {
        case LINETRACER_CONTROL_TURNING_FROM_DEST:
        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
        case LINETRACER_CONTROL_TURNING_TO_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
        case LINETRACER_CONTROL_PICKUP_READY:
        case LINETRACER_CONTROL_WAITING_LOAD:
        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return 1U;

        default:
            return 0U;
    }
}

static uint8_t ControlLogic_NormalTransitionIsAllowed(const control_context_t* context,
                                                      linetracer_control_state_t next_state) {
    switch (context->state) {
        case LINETRACER_CONTROL_INITIALIZING:
            return (next_state == LINETRACER_CONTROL_WAITING_AT_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_WAITING_AT_DEST:
            return (next_state == LINETRACER_CONTROL_TURNING_FROM_DEST || next_state == LINETRACER_CONTROL_UNLOADING)
                       ? 1U
                       : 0U;

        case LINETRACER_CONTROL_TURNING_FROM_DEST:
            if (ControlLogic_CurrentPositionMatchesRoute(context) != 0U) {
                return (next_state == LINETRACER_CONTROL_MOVING_TO_PICKUP) ? 1U : 0U;
            }
            return (next_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
            return (next_state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
            return (next_state == LINETRACER_CONTROL_TURNING_TO_PICKUP) ? 1U : 0U;

        case LINETRACER_CONTROL_TURNING_TO_PICKUP:
            return (next_state == LINETRACER_CONTROL_MOVING_TO_PICKUP) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
            return (next_state == LINETRACER_CONTROL_PICKUP_READY) ? 1U : 0U;

        case LINETRACER_CONTROL_PICKUP_READY:
            return (next_state == LINETRACER_CONTROL_WAITING_LOAD) ? 1U : 0U;

        case LINETRACER_CONTROL_WAITING_LOAD:
            return (next_state == LINETRACER_CONTROL_TURNING_AT_PICKUP) ? 1U : 0U;

        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
            return (next_state == LINETRACER_CONTROL_MOVING_TO_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return (next_state == LINETRACER_CONTROL_UNLOADING) ? 1U : 0U;

        case LINETRACER_CONTROL_UNLOADING:
            return (next_state == LINETRACER_CONTROL_WAITING_AT_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_STOPPED:
            return ControlLogic_StateCanResume(next_state);

        case LINETRACER_CONTROL_ERROR:
            return (next_state == LINETRACER_CONTROL_INITIALIZING) ? 1U : 0U;

        default:
            return 0U;
    }
}

uint8_t ControlLogic_Transition(control_context_t* context, linetracer_control_state_t next_state, uint32_t now_ms) {
    if (context == NULL) {
        return 0U;
    }

    if (context->state == next_state) {
        return 1U;
    }

    if (next_state == LINETRACER_CONTROL_EMERGENCY_STOPPED || next_state == LINETRACER_CONTROL_OBSTACLE_STOP ||
        next_state == LINETRACER_CONTROL_ERROR) {
        context->state = next_state;
        context->state_entered_at_ms = now_ms;
        return 1U;
    }

    if (next_state == LINETRACER_CONTROL_STOPPED && ControlLogic_StateCanResume(context->state) != 0U) {
        context->state = next_state;
        context->state_entered_at_ms = now_ms;
        return 1U;
    }

    if (ControlLogic_NormalTransitionIsAllowed(context, next_state) == 0U) {
        return 0U;
    }

    context->state = next_state;
    context->state_entered_at_ms = now_ms;
    return 1U;
}

void ControlLogic_Init(control_context_t* context, uint32_t now_ms) {
    if (context == NULL) {
        return;
    }

    context->state = LINETRACER_CONTROL_INITIALIZING;
    context->resume_state = LINETRACER_CONTROL_INITIALIZING;
    context->stop_reason = LINETRACER_STOP_REASON_NONE;
    context->current_position = UART_LINETRACER_POSITION_NONE;
    context->active_route = UART_LINETRACER_ROUTE_NONE;
    context->state_entered_at_ms = now_ms;
    context->last_command_at_ms = now_ms;
    context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
    context->route_active = 0U;
    context->resume_valid = 0U;
}

uint8_t ControlLogic_CommandToUartCommand(app_control_command_type_t command) {
    switch (command) {
        case APP_CONTROL_COMMAND_ASSIGN_ROUTE:
            return UART_CMD_LINETRACER_ASSIGN_ROUTE;

        case APP_CONTROL_COMMAND_SET_CURRENT_POSITION:
            return UART_CMD_LINETRACER_SET_CURRENT_POSITION;

        case APP_CONTROL_COMMAND_STOP_DRIVE:
            return UART_CMD_LINETRACER_STOP_DRIVE;

        case APP_CONTROL_COMMAND_RESUME_DRIVE:
            return UART_CMD_LINETRACER_RESUME_DRIVE;

        case APP_CONTROL_COMMAND_MANUAL_UNLOAD:
            return UART_CMD_LINETRACER_MANUAL_UNLOAD;

        case APP_CONTROL_COMMAND_RESET_SYSTEM:
            return UART_CMD_LINETRACER_RESET_SYSTEM;

        case APP_CONTROL_COMMAND_STATUS_REQUEST:
            return UART_CMD_LINETRACER_STATUS_REQUEST;

        default:
            return UART_CMD_NONE;
    }
}

static void ControlLogic_HandleSetPosition(control_context_t* context, const app_control_command_t* command,
                                           uint32_t now_ms, control_command_result_t* result) {
    if (uart_linetracer_position_is_valid(command->position) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        return;
    }

    if ((context->state != LINETRACER_CONTROL_INITIALIZING && context->state != LINETRACER_CONTROL_WAITING_AT_DEST) ||
        context->route_active != 0U) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return;
    }

    context->current_position = command->position;
    if (ControlLogic_Transition(context, LINETRACER_CONTROL_WAITING_AT_DEST, now_ms) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    ControlLogic_Accept(result);
}

static void ControlLogic_HandleAssignRoute(control_context_t* context, const app_control_command_t* command,
                                           uint32_t now_ms, control_command_result_t* result) {
    if (uart_linetracer_job_id_is_valid(command->job_id) == 0U ||
        uart_linetracer_route_is_valid(command->route_id) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        return;
    }

    if (context->state != LINETRACER_CONTROL_WAITING_AT_DEST || context->route_active != 0U ||
        uart_linetracer_position_is_valid(context->current_position) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return;
    }

    context->active_job_id = command->job_id;
    context->active_route = command->route_id;
    context->route_active = 1U;
    context->resume_valid = 0U;
    context->stop_reason = LINETRACER_STOP_REASON_NONE;

    if (ControlLogic_Transition(context, LINETRACER_CONTROL_TURNING_FROM_DEST, now_ms) == 0U) {
        context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
        context->active_route = UART_LINETRACER_ROUTE_NONE;
        context->route_active = 0U;
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    ControlLogic_Accept(result);
}

static void ControlLogic_HandleStop(control_context_t* context, const app_control_command_t* command, uint32_t now_ms,
                                    control_command_result_t* result) {
    if (uart_linetracer_job_id_is_valid(command->job_id) == 0U || ControlLogic_HasActiveJob(context) == 0U ||
        command->job_id != context->active_job_id) {
        ControlLogic_Reject(result, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        return;
    }

    if (context->state == LINETRACER_CONTROL_STOPPED && context->stop_reason == LINETRACER_STOP_REASON_COMMAND) {
        ControlLogic_Accept(result);
        return;
    }

    if (ControlLogic_StateCanResume(context->state) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return;
    }

    context->resume_state = context->state;
    context->resume_valid = 1U;
    context->stop_reason = LINETRACER_STOP_REASON_COMMAND;

    if (ControlLogic_Transition(context, LINETRACER_CONTROL_STOPPED, now_ms) == 0U) {
        context->resume_valid = 0U;
        context->stop_reason = LINETRACER_STOP_REASON_NONE;
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    ControlLogic_Accept(result);
}

static void ControlLogic_HandleResume(control_context_t* context, uint32_t now_ms, control_command_result_t* result) {
    if (context->state != LINETRACER_CONTROL_STOPPED || context->stop_reason != LINETRACER_STOP_REASON_COMMAND ||
        context->resume_valid == 0U || ControlLogic_HasActiveJob(context) == 0U ||
        ControlLogic_StateCanResume(context->resume_state) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return;
    }

    if (ControlLogic_Transition(context, context->resume_state, now_ms) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    context->resume_valid = 0U;
    context->stop_reason = LINETRACER_STOP_REASON_NONE;
    ControlLogic_Accept(result);
}

static void ControlLogic_HandleReset(control_context_t* context, uint32_t now_ms, control_command_result_t* result) {
    linetracer_control_state_t previous_state = context->state;
    uint16_t previous_job_id = context->active_job_id;
    uart_linetracer_route_t previous_route_id = context->active_route;

    if (context->state == LINETRACER_CONTROL_EMERGENCY_STOPPED) {
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_EMERGENCY_STOP);
        return;
    }

    if (context->state == LINETRACER_CONTROL_OBSTACLE_STOP) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_SENSOR);
        return;
    }

    if (context->state == LINETRACER_CONTROL_UNLOADING) {
        result->unload_command = APP_UNLOAD_COMMAND_RESET;
        result->action_job_id = previous_job_id;
        result->action_route_id = previous_route_id;
    }

    ControlLogic_Init(context, now_ms);
    result->previous_state = previous_state;
    ControlLogic_Accept(result);
}

control_command_result_t ControlLogic_HandleCommand(control_context_t* context, const app_control_command_t* command,
                                                    uint32_t now_ms) {
    control_command_result_t result = ControlLogic_MakeResult(context);

    if (context == NULL || command == NULL) {
        ControlLogic_Reject(&result, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        return result;
    }

    context->last_command_at_ms = now_ms;

    if (ControlLogic_CommandTimedOut(command, now_ms) != 0U) {
        ControlLogic_Reject(&result, UART_STATUS_ERROR, UART_ERROR_TIMEOUT);
        return result;
    }

    switch (command->type) {
        case APP_CONTROL_COMMAND_SET_CURRENT_POSITION:
            ControlLogic_HandleSetPosition(context, command, now_ms, &result);
            break;

        case APP_CONTROL_COMMAND_ASSIGN_ROUTE:
            ControlLogic_HandleAssignRoute(context, command, now_ms, &result);
            break;

        case APP_CONTROL_COMMAND_STOP_DRIVE:
            ControlLogic_HandleStop(context, command, now_ms, &result);
            break;

        case APP_CONTROL_COMMAND_RESUME_DRIVE:
            ControlLogic_HandleResume(context, now_ms, &result);
            break;

        case APP_CONTROL_COMMAND_RESET_SYSTEM:
            ControlLogic_HandleReset(context, now_ms, &result);
            break;

        case APP_CONTROL_COMMAND_STATUS_REQUEST:
            result.status_requested = 1U;
            ControlLogic_Accept(&result);
            break;

        case APP_CONTROL_COMMAND_MANUAL_UNLOAD:
            ControlLogic_Reject(&result, UART_STATUS_NACK, UART_ERROR_UNSUPPORTED_COMMAND);
            break;

        case APP_CONTROL_COMMAND_NONE:
        default:
            ControlLogic_Reject(&result, UART_STATUS_NACK, UART_ERROR_INVALID_COMMAND);
            break;
    }

    ControlLogic_UpdateResultState(&result, context);
    return result;
}

control_job_completion_t ControlLogic_CompleteJob(control_context_t* context, uint32_t now_ms) {
    control_job_completion_t completion = { 0 };
    uart_linetracer_position_t destination;

    completion.route_id = UART_LINETRACER_ROUTE_NONE;
    completion.destination = UART_LINETRACER_POSITION_NONE;

    if (context == NULL || context->state != LINETRACER_CONTROL_UNLOADING || ControlLogic_HasActiveJob(context) == 0U) {
        return completion;
    }

    destination = ControlLogic_RouteDestination(context->active_route);
    if (destination == UART_LINETRACER_POSITION_NONE ||
        ControlLogic_Transition(context, LINETRACER_CONTROL_WAITING_AT_DEST, now_ms) == 0U) {
        return completion;
    }

    completion.job_id = context->active_job_id;
    completion.route_id = context->active_route;
    completion.destination = destination;
    completion.completed = 1U;

    context->current_position = destination;
    context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
    context->active_route = UART_LINETRACER_ROUTE_NONE;
    context->route_active = 0U;
    context->resume_state = LINETRACER_CONTROL_WAITING_AT_DEST;
    context->resume_valid = 0U;
    context->stop_reason = LINETRACER_STOP_REASON_NONE;

    return completion;
}
