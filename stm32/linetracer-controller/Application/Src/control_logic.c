#include "control_logic.h"

#include <string.h>

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

static uart_error_t ControlLogic_SafetyError(const control_context_t* context) {
    if (context->safety_error_code != UART_ERROR_NONE) {
        return (uart_error_t)context->safety_error_code;
    }

    if (context->stop_reason == LINETRACER_STOP_REASON_EMERGENCY) {
        return UART_ERROR_EMERGENCY_STOP;
    }

    return UART_ERROR_BUSY;
}

static uart_error_t ControlLogic_CurrentError(const control_context_t* context) {
    if (context == NULL) {
        return UART_ERROR_INTERNAL;
    }

    if (context->safety_error_code != UART_ERROR_NONE) {
        return (uart_linetracer_fault_error_is_valid(context->safety_error_code) != 0U)
                   ? (uart_error_t)context->safety_error_code
                   : UART_ERROR_INTERNAL;
    }

    switch (context->stop_reason) {
        case LINETRACER_STOP_REASON_EMERGENCY:
            return UART_ERROR_EMERGENCY_STOP;

        case LINETRACER_STOP_REASON_TURN_TIMEOUT:
        case LINETRACER_STOP_REASON_COMM_TIMEOUT:
            return UART_ERROR_TIMEOUT;

        case LINETRACER_STOP_REASON_OBSTACLE:
        case LINETRACER_STOP_REASON_LINE_LOST:
        case LINETRACER_STOP_REASON_LOAD_LOST:
        case LINETRACER_STOP_REASON_OVERLOAD:
        case LINETRACER_STOP_REASON_MARKER_SEQUENCE:
        case LINETRACER_STOP_REASON_SENSOR_FAULT:
            return UART_ERROR_SENSOR;

        case LINETRACER_STOP_REASON_NONE:
        case LINETRACER_STOP_REASON_COMMAND:
        default:
            return (context->state == LINETRACER_CONTROL_ERROR) ? UART_ERROR_INTERNAL : UART_ERROR_NONE;
    }
}

static linetracer_control_state_t ControlLogic_SafetyState(linetracer_stop_reason_t reason) {
    switch (reason) {
        case LINETRACER_STOP_REASON_EMERGENCY:
            return LINETRACER_CONTROL_EMERGENCY_STOPPED;

        case LINETRACER_STOP_REASON_OBSTACLE:
            return LINETRACER_CONTROL_OBSTACLE_STOP;

        default:
            return LINETRACER_CONTROL_ERROR;
    }
}

static uint8_t ControlLogic_ContextIsTurning(const control_context_t* context) {
    if (context == NULL) {
        return 0U;
    }

    return (context->state == LINETRACER_CONTROL_TURNING_FROM_DEST ||
            context->state == LINETRACER_CONTROL_TURNING_TO_PICKUP ||
            context->state == LINETRACER_CONTROL_TURNING_AT_PICKUP ||
            (context->state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE &&
             (context->pending_route_action == ROUTE_ACTION_TURN_LEFT ||
              context->pending_route_action == ROUTE_ACTION_TURN_RIGHT)))
               ? 1U
               : 0U;
}

static uint8_t ControlLogic_StateExpectsMarker(linetracer_control_state_t state) {
    return (state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION ||
            state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE || state == LINETRACER_CONTROL_MOVING_TO_PICKUP ||
            state == LINETRACER_CONTROL_MOVING_TO_DEST)
               ? 1U
               : 0U;
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
            return (next_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
            return (next_state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE ||
                    next_state == LINETRACER_CONTROL_MOVING_TO_PICKUP)
                       ? 1U
                       : 0U;

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
    if (ControlLogic_StateExpectsMarker(next_state) != 0U) {
        context->marker_wait_started_at_ms = now_ms;
    }
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
    context->marker_wait_started_at_ms = now_ms;
    context->last_marker_detected_at_ms = 0U;
    context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
    context->last_marker_code = APP_MARKER_NONE;
    context->route_active = 0U;
    context->resume_valid = 0U;
    context->safety_latched = 0U;
    context->safety_error_code = UART_ERROR_NONE;
    context->last_marker_valid = 0U;
    RoutePlanner_Reset(&context->route_plan);
    context->pending_route_action = ROUTE_ACTION_NONE;
}

uint8_t ControlLogic_ApplySafetyEvent(control_context_t* context, const app_control_safety_event_t* event,
                                      uint32_t now_ms) {
    linetracer_stop_reason_t reason;

    if (context == NULL || event == NULL) {
        return 0U;
    }

    switch (event->type) {
        case APP_CONTROL_SAFETY_LATCHED:
        case APP_CONTROL_SAFETY_RESET_REJECTED:
            reason = event->reason;
            if (reason == LINETRACER_STOP_REASON_NONE) {
                reason = context->stop_reason;
            }

            context->safety_latched = 1U;
            context->stop_reason = reason;
            context->resume_valid = 0U;
            if (event->error_code != UART_ERROR_NONE) {
                context->safety_error_code = event->error_code;
            } else if (reason == LINETRACER_STOP_REASON_EMERGENCY) {
                context->safety_error_code = UART_ERROR_EMERGENCY_STOP;
            } else {
                context->safety_error_code = UART_ERROR_BUSY;
            }

            return ControlLogic_Transition(context, ControlLogic_SafetyState(reason), now_ms);

        case APP_CONTROL_SAFETY_RESET_APPROVED:
            ControlLogic_Init(context, now_ms);
            return 1U;

        case APP_CONTROL_SAFETY_NONE:
        default:
            return 0U;
    }
}

void ControlLogic_MakeSnapshot(const control_context_t* context, uart_linetracer_load_state_t load_state,
                               uint32_t now_ms, app_control_snapshot_t* snapshot) {
    if (context == NULL || snapshot == NULL) {
        return;
    }

    (void)memset(snapshot, 0, sizeof(*snapshot));
    snapshot->updated_at_ms = now_ms;
    snapshot->job_id = context->active_job_id;
    snapshot->route_id = context->active_route;
    snapshot->state = linetracer_control_state_to_uart_state(context->state);
    snapshot->load_state = load_state;
    snapshot->error_code = (uint8_t)ControlLogic_CurrentError(context);
    snapshot->safety_latched = context->safety_latched;
}

uint8_t ControlLogic_BuildStartedEvent(const control_context_t* context, const app_control_command_t* command,
                                       const control_command_result_t* result, uart_linetracer_load_state_t load_state,
                                       uint32_t now_ms, app_tx_event_t* event) {
    if (context == NULL || command == NULL || result == NULL || event == NULL ||
        command->type != APP_CONTROL_COMMAND_ASSIGN_ROUTE || result->accepted == 0U ||
        result->status != UART_STATUS_ACK || result->state_changed == 0U || context->route_active == 0U ||
        uart_linetracer_job_id_is_valid(context->active_job_id) == 0U ||
        uart_linetracer_route_is_valid(context->active_route) == 0U) {
        return 0U;
    }

    (void)memset(event, 0, sizeof(*event));
    event->type = APP_TX_EVENT_STARTED;
    event->created_at_ms = now_ms;
    event->job_id = context->active_job_id;
    event->route_id = context->active_route;
    event->state = linetracer_control_state_to_uart_state(context->state);
    event->load_state = load_state;
    event->status = UART_STATUS_SUCCESS;
    event->error_code = UART_ERROR_NONE;
    return 1U;
}

uint8_t ControlLogic_BuildSafetyFaultEvent(const control_context_t* context,
                                           const app_control_safety_event_t* safety_event,
                                           uart_linetracer_load_state_t load_state, uint32_t now_ms,
                                           app_tx_event_t* event) {
    uart_error_t error_code;

    if (context == NULL || safety_event == NULL || event == NULL || safety_event->type != APP_CONTROL_SAFETY_LATCHED) {
        return 0U;
    }

    (void)memset(event, 0, sizeof(*event));
    event->type = APP_TX_EVENT_FAULT;
    event->created_at_ms = now_ms;
    if (uart_linetracer_job_id_is_valid(context->active_job_id) != 0U &&
        uart_linetracer_route_is_valid(context->active_route) != 0U) {
        event->job_id = context->active_job_id;
        event->route_id = context->active_route;
    } else {
        event->job_id = UART_LINETRACER_JOB_ID_NONE;
        event->route_id = UART_LINETRACER_ROUTE_NONE;
    }
    event->state = linetracer_control_state_to_uart_state(context->state);
    event->load_state = load_state;
    event->status = UART_STATUS_ERROR;
    error_code = ControlLogic_CurrentError(context);
    if (error_code == UART_ERROR_NONE && uart_linetracer_fault_error_is_valid(safety_event->error_code) != 0U) {
        error_code = (uart_error_t)safety_event->error_code;
    }
    event->error_code = (error_code != UART_ERROR_NONE) ? (uint8_t)error_code : (uint8_t)UART_ERROR_INTERNAL;
    return 1U;
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
    context->last_marker_code = APP_MARKER_NONE;
    context->last_marker_detected_at_ms = 0U;
    context->last_marker_valid = 0U;

    if (RoutePlanner_Create(context->current_position, context->active_route, &context->route_plan) == 0U) {
        context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
        context->active_route = UART_LINETRACER_ROUTE_NONE;
        context->route_active = 0U;
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    context->pending_route_action = ROUTE_ACTION_TURN_AROUND;

    if (ControlLogic_Transition(context, LINETRACER_CONTROL_TURNING_FROM_DEST, now_ms) == 0U) {
        context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
        context->active_route = UART_LINETRACER_ROUTE_NONE;
        context->route_active = 0U;
        RoutePlanner_Reset(&context->route_plan);
        context->pending_route_action = ROUTE_ACTION_NONE;
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

    if (context->safety_latched != 0U) {
        ControlLogic_Reject(result, UART_STATUS_NACK, ControlLogic_SafetyError(context));
        return;
    }

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

    if (context->safety_latched != 0U &&
        (command->type == APP_CONTROL_COMMAND_ASSIGN_ROUTE || command->type == APP_CONTROL_COMMAND_RESUME_DRIVE)) {
        ControlLogic_Reject(&result, UART_STATUS_NACK, ControlLogic_SafetyError(context));
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

uint8_t ControlLogic_CompleteTurn(control_context_t* context, uint32_t now_ms) {
    linetracer_control_state_t next_state;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U) {
        return 0U;
    }

    switch (context->state) {
        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
            if (context->pending_route_action != ROUTE_ACTION_TURN_LEFT &&
                context->pending_route_action != ROUTE_ACTION_TURN_RIGHT) {
                return 0U;
            }
            context->pending_route_action = ROUTE_ACTION_GO_STRAIGHT;
            context->marker_wait_started_at_ms = now_ms;
            return 1U;

        case LINETRACER_CONTROL_TURNING_FROM_DEST:
            next_state = LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION;
            break;

        case LINETRACER_CONTROL_TURNING_TO_PICKUP:
            next_state = LINETRACER_CONTROL_MOVING_TO_PICKUP;
            break;

        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
            next_state = LINETRACER_CONTROL_MOVING_TO_DEST;
            break;

        default:
            return 0U;
    }

    if (ControlLogic_Transition(context, next_state, now_ms) == 0U) {
        return 0U;
    }

    context->pending_route_action = ROUTE_ACTION_GO_STRAIGHT;
    return 1U;
}

static route_action_t ControlLogic_RouteError(control_context_t* context, linetracer_stop_reason_t reason,
                                              route_action_t action, uint32_t now_ms) {
    context->stop_reason = reason;
    context->resume_valid = 0U;
    context->pending_route_action = action;
    (void)ControlLogic_Transition(context, LINETRACER_CONTROL_ERROR, now_ms);
    return action;
}

static app_marker_code_t ControlLogic_MarkerCodeForIndex(uint8_t index) {
    switch (index) {
        case 0U:
            return APP_MARKER_DEST_A;

        case 1U:
            return APP_MARKER_DEST_B;

        case 2U:
            return APP_MARKER_DEST_C;

        default:
            return APP_MARKER_INVALID;
    }
}

app_marker_code_t ControlLogic_ExpectedMarkerCode(const control_context_t* context) {
    if (context == NULL || context->route_plan.valid == 0U) {
        return APP_MARKER_NONE;
    }

    switch (context->route_plan.expected_marker) {
        case ROUTE_MARKER_DEST_EXIT:
            return ControlLogic_MarkerCodeForIndex(context->route_plan.origin_index);

        case ROUTE_MARKER_SOURCE_JUNCTION:
        case ROUTE_MARKER_COMMON_JUNCTION:
        case ROUTE_MARKER_RETURN_JUNCTION:
            return APP_MARKER_JUNCTION;

        case ROUTE_MARKER_PICKUP:
        case ROUTE_MARKER_PICKUP_EXIT:
        case ROUTE_MARKER_DEST:
            return ControlLogic_MarkerCodeForIndex(context->route_plan.target_index);

        case ROUTE_MARKER_NONE:
        default:
            return APP_MARKER_NONE;
    }
}

static uint8_t ControlLogic_MarkerIsDuplicate(const control_context_t* context, app_marker_code_t marker_code,
                                              uint32_t marker_detected_at_ms) {
    if (context->last_marker_valid == 0U || marker_code != context->last_marker_code) {
        return 0U;
    }

    return (marker_detected_at_ms == context->last_marker_detected_at_ms) ? 1U : 0U;
}

route_action_t ControlLogic_HandleMarker(control_context_t* context, app_marker_code_t marker_code,
                                         uint32_t marker_detected_at_ms, uint32_t now_ms) {
    route_action_t action;
    app_marker_code_t expected_marker;
    uint32_t event_time;
    uint8_t transition_ok = 1U;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    expected_marker = ControlLogic_ExpectedMarkerCode(context);
    event_time = (marker_detected_at_ms != 0U) ? marker_detected_at_ms : now_ms;

    if (ControlLogic_MarkerIsDuplicate(context, marker_code, event_time) != 0U) {
        return ROUTE_ACTION_NONE;
    }

    if (expected_marker == APP_MARKER_NONE || marker_code == APP_MARKER_INVALID || marker_code != expected_marker) {
        return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
    }

    action = RoutePlanner_OnMarker(&context->route_plan);
    context->pending_route_action = action;
    if (action == ROUTE_ACTION_ERROR) {
        return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, action, now_ms);
    }

    context->last_marker_code = marker_code;
    context->last_marker_detected_at_ms = event_time;
    context->last_marker_valid = 1U;

    switch (context->state) {
        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
            if (action == ROUTE_ACTION_GO_STRAIGHT) {
                if (context->route_plan.phase == ROUTE_PHASE_TO_PICKUP) {
                    transition_ok = ControlLogic_Transition(context, LINETRACER_CONTROL_MOVING_TO_PICKUP, now_ms);
                } else if (context->route_plan.phase != ROUTE_PHASE_TO_SOURCE_JUNCTION) {
                    transition_ok = 0U;
                }
            } else if (action == ROUTE_ACTION_TURN_LEFT || action == ROUTE_ACTION_TURN_RIGHT) {
                transition_ok = ControlLogic_Transition(context, LINETRACER_CONTROL_MOVING_ON_COMMON_LINE, now_ms);
            } else {
                transition_ok = 0U;
            }
            break;

        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
            if (action == ROUTE_ACTION_TURN_LEFT || action == ROUTE_ACTION_TURN_RIGHT) {
                transition_ok = ControlLogic_Transition(context, LINETRACER_CONTROL_TURNING_TO_PICKUP, now_ms);
            } else if (action != ROUTE_ACTION_GO_STRAIGHT) {
                transition_ok = 0U;
            }
            break;

        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
            if (action != ROUTE_ACTION_STOP_AT_PICKUP ||
                ControlLogic_Transition(context, LINETRACER_CONTROL_PICKUP_READY, now_ms) == 0U ||
                ControlLogic_Transition(context, LINETRACER_CONTROL_WAITING_LOAD, now_ms) == 0U) {
                transition_ok = 0U;
            }
            break;

        case LINETRACER_CONTROL_MOVING_TO_DEST:
            if (action == ROUTE_ACTION_STOP_AT_DEST) {
                transition_ok = ControlLogic_Transition(context, LINETRACER_CONTROL_UNLOADING, now_ms);
            } else if (action != ROUTE_ACTION_GO_STRAIGHT) {
                transition_ok = 0U;
            }
            break;

        default:
            transition_ok = 0U;
            break;
    }

    if (transition_ok == 0U) {
        return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
    }

    if (ControlLogic_StateExpectsMarker(context->state) != 0U) {
        context->marker_wait_started_at_ms = now_ms;
    }

    return action;
}

linetracer_stop_reason_t ControlLogic_CheckRouteTimeout(control_context_t* context, uint32_t now_ms) {
    linetracer_stop_reason_t reason;
    uint32_t started_at_ms;
    uint32_t timeout_ms;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U || context->safety_latched != 0U) {
        return LINETRACER_STOP_REASON_NONE;
    }

    if (ControlLogic_ContextIsTurning(context) != 0U) {
        reason = LINETRACER_STOP_REASON_TURN_TIMEOUT;
        started_at_ms = context->state_entered_at_ms;
        timeout_ms = CONTROL_TURN_TIMEOUT_MS;
    } else if (ControlLogic_StateExpectsMarker(context->state) != 0U &&
               ControlLogic_ExpectedMarkerCode(context) != APP_MARKER_NONE) {
        reason = LINETRACER_STOP_REASON_MARKER_SEQUENCE;
        started_at_ms = context->marker_wait_started_at_ms;
        timeout_ms = CONTROL_MARKER_TIMEOUT_MS;
    } else {
        return LINETRACER_STOP_REASON_NONE;
    }

    if ((uint32_t)(now_ms - started_at_ms) < timeout_ms) {
        return LINETRACER_STOP_REASON_NONE;
    }

    (void)ControlLogic_RouteError(context, reason, ROUTE_ACTION_ERROR, now_ms);
    return reason;
}

route_action_t ControlLogic_HandleLoadOn(control_context_t* context, uint32_t now_ms) {
    route_action_t action;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    action = RoutePlanner_OnLoadOn(&context->route_plan);
    if (action == ROUTE_ACTION_TURN_AROUND) {
        if (context->state != LINETRACER_CONTROL_WAITING_LOAD ||
            ControlLogic_Transition(context, LINETRACER_CONTROL_TURNING_AT_PICKUP, now_ms) == 0U) {
            return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
        }
        context->pending_route_action = action;
    }

    return action;
}

route_action_t ControlLogic_HandleLoadOff(control_context_t* context, uint32_t now_ms,
                                          control_job_completion_t* completion) {
    route_action_t action;

    if (completion != NULL) {
        *completion = (control_job_completion_t){ 0 };
        completion->route_id = UART_LINETRACER_ROUTE_NONE;
        completion->destination = UART_LINETRACER_POSITION_NONE;
    }

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    action = RoutePlanner_OnLoadOff(&context->route_plan);
    if (action == ROUTE_ACTION_LOAD_LOST) {
        return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_LOAD_LOST, action, now_ms);
    }

    if (action == ROUTE_ACTION_JOB_COMPLETE) {
        control_job_completion_t result = ControlLogic_CompleteJob(context, now_ms);

        if (result.completed == 0U) {
            return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
        }
        if (completion != NULL) {
            *completion = result;
        }
        context->pending_route_action = ROUTE_ACTION_NONE;
    }

    return action;
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
    context->last_marker_code = APP_MARKER_NONE;
    context->last_marker_detected_at_ms = 0U;
    context->last_marker_valid = 0U;
    RoutePlanner_Reset(&context->route_plan);
    context->pending_route_action = ROUTE_ACTION_NONE;

    return completion;
}
