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

static uint8_t ControlLogic_TimeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1U : 0U;
}

static void ControlLogic_SetJunctionPhase(control_context_t* context, control_junction_phase_t phase, uint32_t now_ms) {
    context->junction_phase = phase;
    context->junction_phase_started_at_ms = now_ms;
    context->junction_condition_since_ms = now_ms;
    context->junction_condition_active = 0U;
    if (phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET) {
        context->junction_target_edge_seen = 0U;
    }
}

static void ControlLogic_ResetJunctionManeuver(control_context_t* context) {
    context->junction_phase = CONTROL_JUNCTION_IDLE;
    context->junction_action = ROUTE_ACTION_NONE;
    context->junction_phase_started_at_ms = 0U;
    context->junction_turn_started_at_ms = 0U;
    context->junction_condition_since_ms = 0U;
    context->junction_condition_active = 0U;
    context->junction_candidate_since_ms = 0U;
    context->junction_candidate_active = 0U;
    context->junction_target_edge_seen = 0U;
}

static void ControlLogic_StartJunctionGuard(control_context_t* context, uint32_t now_ms) {
    context->junction_guard_until_ms = now_ms + CONTROL_JUNCTION_EXIT_GUARD_MS;
    context->junction_guard_active = 1U;
    /* Reject stripe events detected before this maneuver completed, even if they are delivered later. */
    context->delayed_marker_ignore_before_ms = now_ms;
    context->delayed_marker_ignore_valid = 1U;
}

static uint8_t ControlLogic_JunctionGuardActive(control_context_t* context, uint32_t now_ms) {
    if (context->junction_guard_active == 0U) {
        return 0U;
    }

    if (ControlLogic_TimeReached(now_ms, context->junction_guard_until_ms) != 0U) {
        context->junction_guard_active = 0U;
        return 0U;
    }

    return 1U;
}

static uint8_t ControlLogic_ConditionStable(control_context_t* context, uint8_t condition, uint32_t now_ms,
                                            uint32_t stable_ms) {
    if (condition == 0U) {
        context->junction_condition_active = 0U;
        context->junction_condition_since_ms = now_ms;
        return 0U;
    }

    if (context->junction_condition_active == 0U) {
        context->junction_condition_active = 1U;
        context->junction_condition_since_ms = now_ms;
    }

    return ((uint32_t)(now_ms - context->junction_condition_since_ms) >= stable_ms) ? 1U : 0U;
}

static uint8_t ControlLogic_StartJunctionManeuver(control_context_t* context, route_action_t action,
                                                  uint8_t from_junction, uint32_t now_ms) {
    if (context == NULL || context->junction_phase != CONTROL_JUNCTION_IDLE) {
        return 0U;
    }

    switch (action) {
        case ROUTE_ACTION_GO_STRAIGHT:
            ControlLogic_SetJunctionPhase(context, CONTROL_JUNCTION_CROSS_STRAIGHT, now_ms);
            break;

        case ROUTE_ACTION_TURN_LEFT:
        case ROUTE_ACTION_TURN_RIGHT:
            ControlLogic_SetJunctionPhase(
                context, (from_junction != 0U) ? CONTROL_JUNCTION_APPROACH_CENTER : CONTROL_JUNCTION_TURN_CLEAR_SOURCE,
                now_ms);
            if (from_junction == 0U) {
                context->junction_turn_started_at_ms = now_ms;
            }
            break;

        case ROUTE_ACTION_TURN_AROUND:
            ControlLogic_SetJunctionPhase(context, CONTROL_JUNCTION_TURN_CLEAR_SOURCE, now_ms);
            context->junction_turn_started_at_ms = now_ms;
            break;

        default:
            return 0U;
    }

    context->junction_action = action;
    context->junction_candidate_active = 0U;
    return 1U;
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

/* A same-zone job reaches its pickup directly; every other job first reaches the common junction. */
static linetracer_control_state_t ControlLogic_DepartureState(const control_context_t* context) {
    return (context != NULL && context->route_plan.phase == ROUTE_PHASE_TO_PICKUP)
               ? LINETRACER_CONTROL_MOVING_TO_PICKUP
               : LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION;
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

uint8_t ControlLogic_IsTurning(const control_context_t* context) {
    if (context == NULL) {
        return 0U;
    }

    return (context->state == LINETRACER_CONTROL_TURNING_FROM_DEST ||
            context->state == LINETRACER_CONTROL_TURNING_TO_PICKUP ||
            context->state == LINETRACER_CONTROL_TURNING_AT_PICKUP ||
            (context->state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE &&
             (context->pending_route_action == ROUTE_ACTION_TURN_LEFT ||
              context->pending_route_action == ROUTE_ACTION_TURN_RIGHT)) ||
            (context->state == LINETRACER_CONTROL_MOVING_TO_DEST &&
             (context->pending_route_action == ROUTE_ACTION_TURN_LEFT ||
              context->pending_route_action == ROUTE_ACTION_TURN_RIGHT)))
               ? 1U
               : 0U;
}

uint8_t ControlLogic_IsTurnAroundActive(const control_context_t* context) {
    if (context == NULL) {
        return 0U;
    }

    if (context->junction_phase != CONTROL_JUNCTION_IDLE && context->junction_action == ROUTE_ACTION_TURN_AROUND) {
        return 1U;
    }

    if (context->pending_route_action != ROUTE_ACTION_TURN_AROUND) {
        return 0U;
    }

    if (ControlLogic_IsTurning(context) != 0U) {
        return 1U;
    }

    /* Cover an in-flight obstacle event received just as the U-turn starts. */
    if (context->state == LINETRACER_CONTROL_OBSTACLE_STOP && context->resume_valid != 0U) {
        switch (context->resume_state) {
            case LINETRACER_CONTROL_TURNING_FROM_DEST:
            case LINETRACER_CONTROL_TURNING_TO_PICKUP:
            case LINETRACER_CONTROL_TURNING_AT_PICKUP:
                return 1U;

            default:
                break;
        }
    }

    return 0U;
}

uint8_t ControlLogic_JunctionManeuverActive(const control_context_t* context) {
    return (context != NULL && context->junction_phase != CONTROL_JUNCTION_IDLE) ? 1U : 0U;
}

uint8_t ControlLogic_StartPendingManeuver(control_context_t* context, uint32_t now_ms) {
    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U ||
        ControlLogic_JunctionManeuverActive(context) != 0U || ControlLogic_IsTurning(context) == 0U) {
        return 0U;
    }

    if (context->pending_route_action != ROUTE_ACTION_TURN_LEFT &&
        context->pending_route_action != ROUTE_ACTION_TURN_RIGHT &&
        context->pending_route_action != ROUTE_ACTION_TURN_AROUND) {
        return 0U;
    }

    return ControlLogic_StartJunctionManeuver(context, context->pending_route_action, 0U, now_ms);
}

route_action_t ControlLogic_JunctionMotorAction(const control_context_t* context) {
    if (context == NULL) {
        return ROUTE_ACTION_NONE;
    }

    switch (context->junction_phase) {
        case CONTROL_JUNCTION_APPROACH_CENTER:
        case CONTROL_JUNCTION_CROSS_STRAIGHT:
            return ROUTE_ACTION_GO_STRAIGHT;

        case CONTROL_JUNCTION_TURN_CLEAR_SOURCE:
        case CONTROL_JUNCTION_TURN_SEARCH_TARGET:
            return context->junction_action;

        case CONTROL_JUNCTION_IDLE:
        default:
            return ROUTE_ACTION_NONE;
    }
}

uint8_t ControlLogic_ShouldIgnoreMarker(const control_context_t* context, app_marker_code_t marker_code,
                                        uint32_t marker_detected_at_ms, uint32_t now_ms) {
    if (context == NULL) {
        return 1U;
    }

    if (ControlLogic_JunctionManeuverActive(context) != 0U) {
        return 1U;
    }

    if (context->junction_guard_active != 0U &&
        ControlLogic_TimeReached(now_ms, context->junction_guard_until_ms) == 0U) {
        return 1U;
    }

    if (context->delayed_marker_ignore_valid != 0U && marker_detected_at_ms != 0U &&
        (int32_t)(marker_detected_at_ms - context->delayed_marker_ignore_before_ms) <= 0) {
        return 1U;
    }

    return 0U;
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

static uint8_t ControlLogic_StateCanPauseForObstacle(linetracer_control_state_t state) {
    if (state == LINETRACER_CONTROL_INITIALIZING || state == LINETRACER_CONTROL_WAITING_AT_DEST ||
        state == LINETRACER_CONTROL_UNLOADING) {
        return 1U;
    }

    return ControlLogic_StateCanResume(state);
}

static uint8_t ControlLogic_CanResumeAfterEmergency(const control_context_t* context) {
    if (ControlLogic_HasActiveJob(context) == 0U) {
        return 0U;
    }

    if (ControlLogic_StateCanResume(context->state) != 0U) {
        return 1U;
    }

    return (context->safety_latched != 0U && context->resume_valid != 0U &&
            ControlLogic_StateCanResume(context->resume_state) != 0U)
               ? 1U
               : 0U;
}

static void ControlLogic_RestartResumeTimers(control_context_t* context, uint32_t now_ms) {
    context->state_entered_at_ms = now_ms;
    context->junction_condition_since_ms = now_ms;
    context->junction_condition_active = 0U;
    context->junction_candidate_since_ms = now_ms;
    context->junction_candidate_active = 0U;

    if (ControlLogic_StateExpectsMarker(context->state) != 0U) {
        context->marker_wait_started_at_ms = now_ms;
    }

    if (ControlLogic_IsTurning(context) != 0U) {
        context->junction_phase_started_at_ms = now_ms;
        context->junction_turn_started_at_ms = now_ms;
    }

    if (context->junction_guard_active != 0U) {
        context->junction_guard_until_ms = now_ms + CONTROL_JUNCTION_EXIT_GUARD_MS;
    }

    /* Drop marker events captured before recovery even if SensorTask delivers them late. */
    context->delayed_marker_ignore_before_ms = now_ms;
    context->delayed_marker_ignore_valid = 1U;
}

static uint8_t ControlLogic_NormalTransitionIsAllowed(const control_context_t* context,
                                                      linetracer_control_state_t next_state) {
    switch (context->state) {
        case LINETRACER_CONTROL_INITIALIZING:
            return (next_state == LINETRACER_CONTROL_WAITING_AT_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_WAITING_AT_DEST:
            return (next_state == LINETRACER_CONTROL_TURNING_FROM_DEST ||
                    next_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION ||
                    next_state == LINETRACER_CONTROL_MOVING_TO_PICKUP || next_state == LINETRACER_CONTROL_UNLOADING)
                       ? 1U
                       : 0U;

        case LINETRACER_CONTROL_TURNING_FROM_DEST:
            return (next_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION ||
                    next_state == LINETRACER_CONTROL_MOVING_TO_PICKUP)
                       ? 1U
                       : 0U;

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
            return (next_state == LINETRACER_CONTROL_TURNING_AT_PICKUP ||
                    next_state == LINETRACER_CONTROL_MOVING_TO_DEST)
                       ? 1U
                       : 0U;

        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
            return (next_state == LINETRACER_CONTROL_MOVING_TO_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return (next_state == LINETRACER_CONTROL_UNLOADING) ? 1U : 0U;

        case LINETRACER_CONTROL_UNLOADING:
            return (next_state == LINETRACER_CONTROL_WAITING_AT_DEST) ? 1U : 0U;

        case LINETRACER_CONTROL_STOPPED:
            return ControlLogic_StateCanResume(next_state);

        case LINETRACER_CONTROL_OBSTACLE_STOP:
            return ControlLogic_StateCanPauseForObstacle(next_state);

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

    if (next_state == LINETRACER_CONTROL_STOPPED &&
        (ControlLogic_StateCanResume(context->state) != 0U || context->state == LINETRACER_CONTROL_UNLOADING)) {
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
    context->junction_guard_until_ms = 0U;
    context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
    context->last_marker_code = APP_MARKER_NONE;
    context->route_active = 0U;
    context->resume_valid = 0U;
    context->safety_latched = 0U;
    context->safety_error_code = UART_ERROR_NONE;
    context->last_marker_valid = 0U;
    context->junction_guard_active = 0U;
    context->departure_turn_required = 0U;
    context->load_wait_armed = 0U;
    RoutePlanner_Reset(&context->route_plan);
    context->pending_route_action = ROUTE_ACTION_NONE;
    ControlLogic_ResetJunctionManeuver(context);
}

uint8_t ControlLogic_ApplySafetyEvent(control_context_t* context, const app_control_safety_event_t* event,
                                      uint32_t now_ms) {
    linetracer_stop_reason_t reason;

    if (context == NULL || event == NULL) {
        return 0U;
    }

    switch (event->type) {
        case APP_CONTROL_SAFETY_OBSTACLE_ACTIVE:
            if (context->safety_latched != 0U || ControlLogic_StateCanPauseForObstacle(context->state) == 0U) {
                return 0U;
            }

            context->resume_state = context->state;
            context->resume_valid = 1U;
            context->stop_reason = LINETRACER_STOP_REASON_OBSTACLE;
            context->safety_error_code = UART_ERROR_SENSOR;
            context->junction_condition_active = 0U;
            context->junction_candidate_active = 0U;
            return ControlLogic_Transition(context, LINETRACER_CONTROL_OBSTACLE_STOP, now_ms);

        case APP_CONTROL_SAFETY_OBSTACLE_CLEARED:
            if (context->safety_latched != 0U || context->state != LINETRACER_CONTROL_OBSTACLE_STOP ||
                context->resume_valid == 0U) {
                return 0U;
            }

            context->stop_reason = LINETRACER_STOP_REASON_NONE;
            context->safety_error_code = UART_ERROR_NONE;
            if (ControlLogic_Transition(context, context->resume_state, now_ms) == 0U) {
                context->stop_reason = LINETRACER_STOP_REASON_OBSTACLE;
                context->safety_error_code = UART_ERROR_SENSOR;
                return 0U;
            }
            context->resume_valid = 0U;
            if (ControlLogic_IsTurning(context) != 0U) {
                context->junction_phase_started_at_ms = now_ms;
                context->junction_turn_started_at_ms = now_ms;
            }
            return 1U;

        case APP_CONTROL_SAFETY_LATCHED:
            reason = event->reason;
            if (reason == LINETRACER_STOP_REASON_NONE) {
                reason = context->stop_reason;
            }

            if (reason == LINETRACER_STOP_REASON_EMERGENCY && ControlLogic_CanResumeAfterEmergency(context) != 0U) {
                if (ControlLogic_StateCanResume(context->state) != 0U) {
                    context->resume_state = context->state;
                    context->resume_valid = 1U;
                }
                context->junction_condition_active = 0U;
                context->junction_candidate_active = 0U;
            } else {
                context->resume_valid = 0U;
                ControlLogic_ResetJunctionManeuver(context);
            }

            context->safety_latched = 1U;
            context->stop_reason = reason;
            if (event->error_code != UART_ERROR_NONE) {
                context->safety_error_code = event->error_code;
            } else if (reason == LINETRACER_STOP_REASON_EMERGENCY) {
                context->safety_error_code = UART_ERROR_EMERGENCY_STOP;
            } else {
                context->safety_error_code = UART_ERROR_BUSY;
            }

            return ControlLogic_Transition(context, ControlLogic_SafetyState(reason), now_ms);

        case APP_CONTROL_SAFETY_RESET_REJECTED:
            reason = event->reason;
            if (reason == LINETRACER_STOP_REASON_NONE) {
                reason = context->stop_reason;
            }

            context->safety_latched = 1U;
            context->stop_reason = reason;
            if (event->error_code != UART_ERROR_NONE) {
                context->safety_error_code = event->error_code;
            } else if (reason == LINETRACER_STOP_REASON_EMERGENCY) {
                context->safety_error_code = UART_ERROR_EMERGENCY_STOP;
            } else {
                context->safety_error_code = UART_ERROR_BUSY;
            }
            return ControlLogic_Transition(context, ControlLogic_SafetyState(reason), now_ms);

        case APP_CONTROL_SAFETY_RESET_APPROVED:
            if (context->safety_latched != 0U && context->stop_reason == LINETRACER_STOP_REASON_EMERGENCY &&
                context->resume_valid != 0U && ControlLogic_HasActiveJob(context) != 0U &&
                ControlLogic_StateCanResume(context->resume_state) != 0U) {
                context->state = context->resume_state;
                context->resume_valid = 0U;
                context->safety_latched = 0U;
                context->stop_reason = LINETRACER_STOP_REASON_NONE;
                context->safety_error_code = UART_ERROR_NONE;
                ControlLogic_RestartResumeTimers(context, now_ms);
                return 1U;
            }

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
    /* INITIALIZE/SET_CURRENT_POSITION stages the vehicle in its requested initial heading. */
    context->departure_turn_required = 0U;
    if (ControlLogic_Transition(context, LINETRACER_CONTROL_WAITING_AT_DEST, now_ms) == 0U) {
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    ControlLogic_Accept(result);
}

static void ControlLogic_HandleAssignRoute(control_context_t* context, const app_control_command_t* command,
                                           uint32_t now_ms, control_command_result_t* result) {
    uint8_t turn_before_departure;
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

    turn_before_departure = context->departure_turn_required;

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

    /*
     * The first route after INITIALIZE starts from the manually staged heading.
     * After an unload, the
     * vehicle still faces into the completed destination,
     * so turn around and reacquire the outgoing line before
     * line following.
     */
    context->pending_route_action = (turn_before_departure != 0U) ? ROUTE_ACTION_TURN_AROUND : ROUTE_ACTION_GO_STRAIGHT;

    if (ControlLogic_Transition(
            context,
            (turn_before_departure != 0U) ? LINETRACER_CONTROL_TURNING_FROM_DEST : ControlLogic_DepartureState(context),
            now_ms) == 0U) {
        context->active_job_id = UART_LINETRACER_JOB_ID_NONE;
        context->active_route = UART_LINETRACER_ROUTE_NONE;
        context->route_active = 0U;
        RoutePlanner_Reset(&context->route_plan);
        context->pending_route_action = ROUTE_ACTION_NONE;
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    context->departure_turn_required = 0U;

    ControlLogic_Accept(result);
}

static void ControlLogic_HandleStop(control_context_t* context, const app_control_command_t* command, uint32_t now_ms,
                                    control_command_result_t* result) {
    linetracer_control_state_t previous_state;

    if (uart_linetracer_job_id_is_valid(command->job_id) == 0U || ControlLogic_HasActiveJob(context) == 0U ||
        command->job_id != context->active_job_id) {
        ControlLogic_Reject(result, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        return;
    }

    if (context->state == LINETRACER_CONTROL_STOPPED && context->stop_reason == LINETRACER_STOP_REASON_COMMAND) {
        ControlLogic_Accept(result);
        return;
    }

    if (ControlLogic_StateCanResume(context->state) == 0U && context->state != LINETRACER_CONTROL_UNLOADING) {
        ControlLogic_Reject(result, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return;
    }

    previous_state = context->state;
    if (previous_state == LINETRACER_CONTROL_UNLOADING) {
        /*
         * An interrupted unload must be restarted as a new operation after
         * explicit reset. Never resume from an unknown servo/load position.
         */
        context->resume_state = LINETRACER_CONTROL_INITIALIZING;
        context->resume_valid = 0U;
    } else {
        context->resume_state = context->state;
        context->resume_valid = 1U;
    }
    context->stop_reason = LINETRACER_STOP_REASON_COMMAND;
    ControlLogic_ResetJunctionManeuver(context);

    if (ControlLogic_Transition(context, LINETRACER_CONTROL_STOPPED, now_ms) == 0U) {
        context->resume_valid = 0U;
        context->stop_reason = LINETRACER_STOP_REASON_NONE;
        ControlLogic_Reject(result, UART_STATUS_ERROR, UART_ERROR_INTERNAL);
        return;
    }

    if (previous_state == LINETRACER_CONTROL_UNLOADING) {
        result->unload_command = APP_UNLOAD_COMMAND_ABORT;
        result->action_job_id = context->active_job_id;
        result->action_route_id = context->active_route;
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

    /*
     * RESET also clears any command-stop or safety inhibit retained by
     * UnloadTask, even if ControlTask has already left UNLOADING.
     */
    result->unload_command = APP_UNLOAD_COMMAND_RESET;
    result->action_job_id = previous_job_id;
    result->action_route_id = previous_route_id;

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

app_tx_event_type_t ControlLogic_CommandResponseEventType(const control_command_result_t* result) {
    if (result == NULL) {
        return APP_TX_EVENT_NONE;
    }

    if (result->accepted != 0U && result->status_requested != 0U) {
        return APP_TX_EVENT_STATUS;
    }

    return APP_TX_EVENT_COMMAND_ACK;
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

        case LINETRACER_CONTROL_MOVING_TO_DEST:
            if (context->pending_route_action != ROUTE_ACTION_TURN_LEFT &&
                context->pending_route_action != ROUTE_ACTION_TURN_RIGHT) {
                return 0U;
            }
            context->pending_route_action = ROUTE_ACTION_GO_STRAIGHT;
            context->marker_wait_started_at_ms = now_ms;
            return 1U;

        case LINETRACER_CONTROL_TURNING_FROM_DEST:
            next_state = ControlLogic_DepartureState(context);
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
    ControlLogic_ResetJunctionManeuver(context);
    (void)ControlLogic_Transition(context, LINETRACER_CONTROL_ERROR, now_ms);
    return action;
}

static app_marker_code_t ControlLogic_MarkerCodeForIndex(uint8_t index) {
    /* Destination identity comes from the active route, not stripe count. */
    (void)index;
    return APP_MARKER_JUNCTION;
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
        case ROUTE_MARKER_TARGET_JUNCTION:
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

    /* Destination marker activity can continue while the vehicle is stationary for unloading. */
    if (context->state == LINETRACER_CONTROL_UNLOADING) {
        return ROUTE_ACTION_NONE;
    }

    expected_marker = ControlLogic_ExpectedMarkerCode(context);
    event_time = (marker_detected_at_ms != 0U) ? marker_detected_at_ms : now_ms;

    if (ControlLogic_MarkerIsDuplicate(context, marker_code, event_time) != 0U) {
        return ROUTE_ACTION_NONE;
    }

    /*
     * The pickup marker may still be reported after the controller has
     * stopped at the pickup. Consume
     * that delayed event so it cannot be
     * interpreted as an unexpected next-route marker while waiting for load.

     */
    if (context->state == LINETRACER_CONTROL_PICKUP_READY || context->state == LINETRACER_CONTROL_WAITING_LOAD ||
        context->state == LINETRACER_CONTROL_UNLOADING) {
        context->last_marker_code = marker_code;
        context->last_marker_detected_at_ms = event_time;
        context->last_marker_valid = 1U;
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
            } else {
                /* Ignore any load level that existed before arrival at the pickup. */
                context->load_wait_armed = 0U;
            }
            break;

        case LINETRACER_CONTROL_MOVING_TO_DEST:
            if (action == ROUTE_ACTION_STOP_AT_DEST) {
                transition_ok = ControlLogic_Transition(context, LINETRACER_CONTROL_UNLOADING, now_ms);
            } else if (action != ROUTE_ACTION_GO_STRAIGHT && action != ROUTE_ACTION_TURN_LEFT &&
                       action != ROUTE_ACTION_TURN_RIGHT) {
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

static uint8_t ControlLogic_TargetEdgeDetected(const control_context_t* context, uint8_t line_left, uint8_t line_center,
                                               uint8_t line_right) {
    if (context == NULL ||
        (context->junction_action != ROUTE_ACTION_TURN_LEFT && context->junction_action != ROUTE_ACTION_TURN_RIGHT &&
         context->junction_action != ROUTE_ACTION_TURN_AROUND)) {
        return 0U;
    }

    if (context->junction_action == ROUTE_ACTION_TURN_LEFT) {
        /* Accept 100, 110, or 010 so a fast left turn cannot skip a narrow target edge. */
        return (line_right == 0U && (line_left != 0U || line_center != 0U)) ? 1U : 0U;
    }

    /* Accept 001, 011, or 010 so a fast right turn/U-turn cannot skip a narrow target edge. */
    return (line_left == 0U && (line_center != 0U || line_right != 0U)) ? 1U : 0U;
}

static uint8_t ControlLogic_TargetAligned(const control_context_t* context, uint8_t line_left, uint8_t line_center,
                                          uint8_t line_right) {
    if (context == NULL || context->junction_target_edge_seen == 0U || line_center == 0U) {
        return 0U;
    }

    if (context->junction_action == ROUTE_ACTION_TURN_LEFT) {
        /* 100 only arms target detection; finish on 110 or 010, then let PID centre. */
        return (line_right == 0U) ? 1U : 0U;
    }

    /* 001 only arms target detection; finish on 011 or 010 for right turns/U-turns. */
    return (line_left == 0U) ? 1U : 0U;
}

static uint8_t ControlLogic_CompleteDetectedTurn(control_context_t* context, uint32_t now_ms) {
    if (ControlLogic_CompleteTurn(context, now_ms) == 0U) {
        return 0U;
    }

    /*
     * A completed turn has already left its source branch. Do not consume a
     * synthetic endpoint-exit
     * marker here: the next required observation is
     * the next both-black junction or the destination stripe
     * group.
     */
    (void)now_ms;
    return 1U;
}

control_line_result_t ControlLogic_ProcessLineSampleWithCenter(control_context_t* context, uint8_t line_left,
                                                               uint8_t line_center, uint8_t line_right,
                                                               uint32_t now_ms) {
    control_line_result_t result = { 0 };
    uint8_t all_white;
    uint8_t both_black;
    uint8_t both_white;
    uint8_t junction_guard_active;
    uint8_t target_aligned;
    linetracer_control_state_t previous_state;

    result.action = ROUTE_ACTION_NONE;
    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U || context->safety_latched != 0U) {
        return result;
    }

    line_left = (line_left != 0U) ? 1U : 0U;
    line_center = (line_center != 0U) ? 1U : 0U;
    line_right = (line_right != 0U) ? 1U : 0U;
    both_black = (line_left != 0U && line_right != 0U) ? 1U : 0U;
    both_white = (line_left == 0U && line_right == 0U) ? 1U : 0U;
    all_white = (both_white != 0U && line_center == 0U) ? 1U : 0U;

    if (context->junction_phase == CONTROL_JUNCTION_IDLE) {
        junction_guard_active = ControlLogic_JunctionGuardActive(context, now_ms);
        if (junction_guard_active != 0U || ControlLogic_ExpectedMarkerCode(context) != APP_MARKER_JUNCTION) {
            context->junction_candidate_active = 0U;
            return result;
        }

        if (both_black == 0U) {
            context->junction_candidate_active = 0U;
            return result;
        }

        if (context->junction_candidate_active == 0U) {
            context->junction_candidate_active = 1U;
            context->junction_candidate_since_ms = now_ms;
            if (CONTROL_JUNCTION_BLACK_STABLE_MS != 0U) {
                return result;
            }
        }

        if ((uint32_t)(now_ms - context->junction_candidate_since_ms) < CONTROL_JUNCTION_BLACK_STABLE_MS) {
            return result;
        }

        previous_state = context->state;
        result.action =
            ControlLogic_HandleMarker(context, APP_MARKER_JUNCTION, context->junction_candidate_since_ms, now_ms);
        result.action_valid = 1U;
        result.state_changed = (previous_state != context->state) ? 1U : 0U;
        context->junction_candidate_active = 0U;

        if (result.action == ROUTE_ACTION_GO_STRAIGHT || result.action == ROUTE_ACTION_TURN_LEFT ||
            result.action == ROUTE_ACTION_TURN_RIGHT) {
            if (ControlLogic_StartJunctionManeuver(context, result.action, 1U, now_ms) == 0U) {
                result.action = ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE,
                                                        ROUTE_ACTION_ERROR, now_ms);
            }
        }
        return result;
    }

    switch (context->junction_phase) {
        case CONTROL_JUNCTION_APPROACH_CENTER:
            if ((uint32_t)(now_ms - context->junction_phase_started_at_ms) >= CONTROL_JUNCTION_CENTER_ADVANCE_MS) {
                context->junction_turn_started_at_ms = now_ms;
                ControlLogic_SetJunctionPhase(context, CONTROL_JUNCTION_TURN_CLEAR_SOURCE, now_ms);
            }
            break;

        case CONTROL_JUNCTION_CROSS_STRAIGHT:
            /*
             * A transverse marker is clear as soon as it no longer covers both
             * outer
             * sensors. Requiring 000 here can wedge the maneuver forever
             * when normal PID correction
             * resumes as 100/110 or 001/011.
             */
            if (ControlLogic_ConditionStable(context, (both_black == 0U) ? 1U : 0U, now_ms,
                                             CONTROL_JUNCTION_CROSS_CLEAR_MS) != 0U) {
                ControlLogic_ResetJunctionManeuver(context);
                ControlLogic_StartJunctionGuard(context, now_ms);
                result.maneuver_completed = 1U;
            }
            break;

        case CONTROL_JUNCTION_TURN_CLEAR_SOURCE:
            /*
             * A clockwise U-turn does not wait for a stable 000 interval. Once the
             * centre
             * and right sensors have left the source line (000 or 100), search
             * continuously. Regular
             * 90-degree turns retain the stronger all-white guard.
             */
            if (context->junction_action == ROUTE_ACTION_TURN_AROUND && line_center == 0U && line_right == 0U) {
                ControlLogic_SetJunctionPhase(context, CONTROL_JUNCTION_TURN_SEARCH_TARGET, now_ms);
            } else if (context->junction_action != ROUTE_ACTION_TURN_AROUND &&
                       ControlLogic_ConditionStable(context, all_white, now_ms, CONTROL_TURN_SOURCE_CLEAR_MS) != 0U) {
                ControlLogic_SetJunctionPhase(context, CONTROL_JUNCTION_TURN_SEARCH_TARGET, now_ms);
            }
            break;

        case CONTROL_JUNCTION_TURN_SEARCH_TARGET:
            if (context->junction_target_edge_seen == 0U) {
                if (ControlLogic_TargetEdgeDetected(context, line_left, line_center, line_right) != 0U) {
                    context->junction_target_edge_seen = 1U;
                    context->junction_condition_active = 0U;
                } else {
                    break;
                }
            }

            target_aligned = ControlLogic_TargetAligned(context, line_left, line_center, line_right);
            if (ControlLogic_ConditionStable(context, target_aligned, now_ms, CONTROL_TURN_TARGET_CENTERED_MS) != 0U) {
                previous_state = context->state;
                if (ControlLogic_CompleteDetectedTurn(context, now_ms) == 0U) {
                    result.action = ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE,
                                                            ROUTE_ACTION_ERROR, now_ms);
                    result.action_valid = 1U;
                    break;
                }

                ControlLogic_ResetJunctionManeuver(context);
                ControlLogic_StartJunctionGuard(context, now_ms);
                result.maneuver_completed = 1U;
                result.state_changed = (previous_state != context->state) ? 1U : 0U;
            }
            break;

        case CONTROL_JUNCTION_IDLE:
        default:
            break;
    }

    return result;
}

control_line_result_t ControlLogic_ProcessLineSample(control_context_t* context, uint8_t line_left, uint8_t line_right,
                                                     uint32_t now_ms) {
    return ControlLogic_ProcessLineSampleWithCenter(context, line_left, 1U, line_right, now_ms);
}

linetracer_stop_reason_t ControlLogic_CheckRouteTimeout(control_context_t* context, uint32_t now_ms) {
    linetracer_stop_reason_t reason;
    uint32_t started_at_ms;
    uint32_t timeout_ms;

    if (context == NULL || ControlLogic_HasActiveJob(context) == 0U || context->safety_latched != 0U) {
        return LINETRACER_STOP_REASON_NONE;
    }

    /*
     * Demo tracks can have different branch lengths and turning radii. When
     * route timeouts are disabled,
     * progress is driven only by confirmed line,
     * marker, and load events instead of wall-clock deadlines.
     */
    if (CONTROL_ROUTE_TIMEOUTS_ENABLED == 0U) {
        (void)now_ms;
        return LINETRACER_STOP_REASON_NONE;
    }

    if (context->junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT) {
        reason = LINETRACER_STOP_REASON_MARKER_SEQUENCE;
        started_at_ms = context->junction_phase_started_at_ms;
        timeout_ms = CONTROL_JUNCTION_CROSS_TIMEOUT_MS;
    } else if (ControlLogic_JunctionManeuverActive(context) != 0U &&
               (context->junction_action == ROUTE_ACTION_TURN_LEFT ||
                context->junction_action == ROUTE_ACTION_TURN_RIGHT ||
                context->junction_action == ROUTE_ACTION_TURN_AROUND)) {
        reason = LINETRACER_STOP_REASON_TURN_TIMEOUT;
        started_at_ms = (context->junction_turn_started_at_ms != 0U) ? context->junction_turn_started_at_ms
                                                                     : context->junction_phase_started_at_ms;
        timeout_ms =
            (context->junction_action == ROUTE_ACTION_TURN_AROUND) ? CONTROL_UTURN_TIMEOUT_MS : CONTROL_TURN_TIMEOUT_MS;
    } else if (ControlLogic_IsTurning(context) != 0U) {
        reason = LINETRACER_STOP_REASON_TURN_TIMEOUT;
        started_at_ms = context->state_entered_at_ms;
        timeout_ms = (context->pending_route_action == ROUTE_ACTION_TURN_AROUND) ? CONTROL_UTURN_TIMEOUT_MS
                                                                                 : CONTROL_TURN_TIMEOUT_MS;
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
    if (action == ROUTE_ACTION_GO_STRAIGHT) {
        if (context->state != LINETRACER_CONTROL_WAITING_LOAD ||
            ControlLogic_Transition(context, LINETRACER_CONTROL_MOVING_TO_DEST, now_ms) == 0U) {
            return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
        }
        context->pending_route_action = action;
        context->load_wait_armed = 0U;
    } else if (action == ROUTE_ACTION_TURN_AROUND) {
        if (context->state != LINETRACER_CONTROL_WAITING_LOAD ||
            ControlLogic_Transition(context, LINETRACER_CONTROL_TURNING_AT_PICKUP, now_ms) == 0U) {
            return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_MARKER_SEQUENCE, ROUTE_ACTION_ERROR, now_ms);
        }
        context->pending_route_action = action;
        context->load_wait_armed = 0U;
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

    /*
     * During unloading, the filtered LOAD_EMPTY transition only tells
     * UnloadTask to return the servo
     * home. Job completion is intentionally
     * deferred until ControlTask receives APP_UNLOAD_RESULT_COMPLETE.
     */
    if (context->state == LINETRACER_CONTROL_UNLOADING) {
        return ROUTE_ACTION_NONE;
    }

    action = RoutePlanner_OnLoadOff(&context->route_plan);
    if (action == ROUTE_ACTION_LOAD_LOST) {
        return ControlLogic_RouteError(context, LINETRACER_STOP_REASON_LOAD_LOST, action, now_ms);
    }

    return action;
}

static control_job_completion_t ControlLogic_CompleteJob(control_context_t* context, uint32_t now_ms) {
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
    context->departure_turn_required = 1U;
    RoutePlanner_Reset(&context->route_plan);
    context->pending_route_action = ROUTE_ACTION_NONE;

    return completion;
}

control_job_completion_t ControlLogic_HandleUnloadResult(control_context_t* context,
                                                         const app_unload_result_t* unload_result, uint32_t now_ms) {
    control_job_completion_t completion = { 0 };

    completion.route_id = UART_LINETRACER_ROUTE_NONE;
    completion.destination = UART_LINETRACER_POSITION_NONE;

    if (context == NULL || unload_result == NULL || unload_result->type != APP_UNLOAD_RESULT_COMPLETE ||
        context->state != LINETRACER_CONTROL_UNLOADING || context->active_job_id != unload_result->job_id ||
        context->active_route != unload_result->route_id) {
        return completion;
    }

    return ControlLogic_CompleteJob(context, now_ms);
}
