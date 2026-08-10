#include "control_task.h"

#include <stdint.h>

#include "app_queues.h"
#include "cmsis_os2.h"
#include "control_config.h"
#include "control_logic.h"
#include "line_follow_pid.h"
#include "main.h"
#include "motor_control.h"
#include "sensor_config.h"
#include "sensor_task.h"
#include "unload_hw.h"
#include "unload_task.h"

static control_context_t controlTaskContext;
static line_follow_pid_t controlTaskLinePid;
static uart_linetracer_load_state_t controlTaskLoadState = UART_LINETRACER_LOAD_EMPTY;
static linetracer_line_state_t controlTaskLineState = LINETRACER_LINE_UNKNOWN;
static int16_t controlTaskLineError;
static uint8_t controlTaskMotorReady;
static uint32_t controlTaskStartBoostUntilMs;
static volatile uint8_t controlTaskInitialized;
static app_control_safety_event_t controlTaskPendingUnloadReset;
static uint8_t controlTaskPendingUnloadResetActive;
static uint32_t controlTaskPendingUnloadResetDeadlineMs;
static uint32_t controlTaskPendingUnloadResetRequestId;
static uint32_t controlTaskNextUnloadRequestId;

static void ControlTask_PublishStateChanged(uint32_t now_ms);

typedef struct {
    app_tx_event_t event;
    uint32_t retry_at_ms;
    uint8_t active;
} control_pending_response_t;

static control_pending_response_t controlPendingResponses[CONTROL_TASK_PENDING_RESPONSE_CAPACITY];
static uint8_t ControlTask_RequestUnloadReset(uint32_t now_ms, uint32_t inhibit_generation, uint32_t* request_id);

static uint32_t ControlTask_EnterShortCriticalSection(void) {
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void ControlTask_ExitShortCriticalSection(uint32_t primask) {
    if (primask == 0U) {
        __enable_irq();
    }
}

bool ControlTask_GetLatest(app_control_snapshot_t* snapshot) {
    uint32_t primask;
    uint32_t now_ms;

    if (snapshot == NULL) {
        return false;
    }

    now_ms = osKernelGetTickCount();
    primask = ControlTask_EnterShortCriticalSection();
    if (controlTaskInitialized == 0U) {
        ControlTask_ExitShortCriticalSection(primask);
        return false;
    }

    ControlLogic_MakeSnapshot(&controlTaskContext, controlTaskLoadState, now_ms, snapshot);
    ControlTask_ExitShortCriticalSection(primask);
    return true;
}

bool ControlTask_IsTurning(void) {
    uint32_t primask;
    bool turning;

    primask = ControlTask_EnterShortCriticalSection();
    turning = (controlTaskInitialized != 0U) && (ControlLogic_IsTurning(&controlTaskContext) != 0U);
    ControlTask_ExitShortCriticalSection(primask);
    return turning;
}

static bool ControlTask_StateRequiresUltrasonic(linetracer_control_state_t state) {
    switch (state) {
        case LINETRACER_CONTROL_TURNING_FROM_DEST:
        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
        case LINETRACER_CONTROL_TURNING_TO_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return true;

        default:
            return false;
    }
}

bool ControlTask_ShouldMonitorUltrasonic(void) {
    bool enabled = false;
    uint32_t primask = ControlTask_EnterShortCriticalSection();

    if ((controlTaskInitialized != 0U) && (controlTaskContext.safety_latched == 0U)) {
        enabled = ControlTask_StateRequiresUltrasonic(controlTaskContext.state);
        if (controlTaskContext.state == LINETRACER_CONTROL_OBSTACLE_STOP) {
            enabled = (controlTaskContext.resume_valid != 0U) &&
                      ControlTask_StateRequiresUltrasonic(controlTaskContext.resume_state);
        }
    }

    ControlTask_ExitShortCriticalSection(primask);
    return enabled;
}

static void ControlTask_PublishHealthEvent(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_CONTROL;
    (void)AppQueues_TryPutHealth(&event);
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

        LineFollowPid_Reset(&controlTaskLinePid);
        controlTaskStartBoostUntilMs = 0U;
        MotorControl_ForceStop();
        if (event.type == APP_CONTROL_SAFETY_RESET_APPROVED) {
            uint32_t request_id = 0U;

            if (event.reason == LINETRACER_STOP_REASON_NONE) {
                event.reason = controlTaskContext.stop_reason;
            }
            if (controlTaskPendingUnloadResetActive != 0U ||
                ControlTask_RequestUnloadReset(now_ms, event.unload_inhibit_generation, &request_id) == 0U) {
                event.type = APP_CONTROL_SAFETY_RESET_REJECTED;
                event.error_code = UART_ERROR_BUSY;
            } else {
                /* Keep ControlTask latched until UnloadTask confirms the same request and generation. */
                controlTaskPendingUnloadReset = event;
                controlTaskPendingUnloadResetRequestId = request_id;
                controlTaskPendingUnloadResetDeadlineMs = now_ms + CONTROL_TASK_UNLOAD_RESET_TIMEOUT_MS;
                controlTaskPendingUnloadResetActive = 1U;
                ++processed;
                continue;
            }
        }

        if (ControlLogic_ApplySafetyEvent(&controlTaskContext, &event, now_ms) != 0U) {
            if (event.type == APP_CONTROL_SAFETY_LATCHED || event.type == APP_CONTROL_SAFETY_RESET_REJECTED) {
                app_tx_event_t fault_event;

                if (ControlLogic_BuildSafetyFaultEvent(&controlTaskContext, &event, controlTaskLoadState, now_ms,
                                                       &fault_event) != 0U) {
                    ControlTask_PublishTxEvent(&fault_event, now_ms);
                }
            } else if (event.type == APP_CONTROL_SAFETY_OBSTACLE_ACTIVE ||
                       event.type == APP_CONTROL_SAFETY_OBSTACLE_CLEARED) {
                ControlTask_PublishStateChanged(now_ms);
            }
            ControlTask_PublishSafetyResetResult(&event, now_ms);
        }
        ++processed;
    }
}

static void ControlTask_ApplyStartBoost(motor_output_t* output, uint32_t now_ms) {
    motor_output_t previous_output;
    uint16_t faster_pwm;
    uint16_t boost_delta;

    if (output == NULL) {
        return;
    }

    if (output->standby == 0U || output->left_direction != MOTOR_DIRECTION_FORWARD ||
        output->right_direction != MOTOR_DIRECTION_FORWARD || (output->left_pwm == 0U && output->right_pwm == 0U)) {
        controlTaskStartBoostUntilMs = 0U;
        return;
    }

    MotorControl_GetLastOutput(&previous_output);
    if (previous_output.standby == 0U || (previous_output.left_pwm == 0U && previous_output.right_pwm == 0U)) {
        controlTaskStartBoostUntilMs = now_ms + MOTOR_CONTROL_START_BOOST_MS;
    }

    if (controlTaskStartBoostUntilMs != 0U && ControlTask_TimeReached(now_ms, controlTaskStartBoostUntilMs) == 0U) {
        faster_pwm = (output->left_pwm > output->right_pwm) ? output->left_pwm : output->right_pwm;
        if (faster_pwm < MOTOR_CONTROL_START_BOOST_PWM) {
            /* Raise both wheels by the same amount so the PID steering difference survives startup. */
            boost_delta = MOTOR_CONTROL_START_BOOST_PWM - faster_pwm;
            output->left_pwm = MotorControlLogic_ClampPwm((int32_t)output->left_pwm + boost_delta);
            output->right_pwm = MotorControlLogic_ClampPwm((int32_t)output->right_pwm + boost_delta);
        }
    }
}

static uint8_t ControlTask_ApplyMotorOutput(const motor_output_t* output, uint32_t now_ms) {
    motor_output_t adjusted_output;

    if (controlTaskMotorReady == 0U) {
        MotorControl_ForceStop();
        return 0U;
    }

    if (output == NULL) {
        MotorControl_ForceStop();
        return 0U;
    }

    adjusted_output = *output;
    ControlTask_ApplyStartBoost(&adjusted_output, now_ms);
    if (MotorControl_Apply(&adjusted_output) == 0U) {
        controlTaskMotorReady = 0U;
        MotorControl_ForceStop();
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR, CONTROL_HEALTH_MOTOR_OUTPUT_FAILED, now_ms);
        return 0U;
    }

    return 1U;
}

static uint8_t ControlTask_ApplyRouteActionMotor(route_action_t action, uint32_t now_ms) {
    motor_output_t output;

    if (MotorControlLogic_ComputeRouteAction(action, &output) != 0U) {
        return ControlTask_ApplyMotorOutput(&output, now_ms);
    }

    return 0U;
}

static uint8_t ControlTask_PidLineFollowEnabled(void) {
    if (controlTaskContext.safety_latched != 0U || controlTaskContext.route_active == 0U) {
        return 0U;
    }

    switch (controlTaskContext.state) {
        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return 1U;

        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
            return (controlTaskContext.pending_route_action != ROUTE_ACTION_TURN_LEFT &&
                    controlTaskContext.pending_route_action != ROUTE_ACTION_TURN_RIGHT)
                       ? 1U
                       : 0U;

        default:
            return 0U;
    }
}

static uint8_t ControlTask_RouteMotionEnabled(void) {
    if (controlTaskContext.safety_latched != 0U || controlTaskContext.route_active == 0U ||
        controlTaskContext.state == LINETRACER_CONTROL_STOPPED ||
        controlTaskContext.state == LINETRACER_CONTROL_OBSTACLE_STOP ||
        controlTaskContext.state == LINETRACER_CONTROL_ERROR ||
        controlTaskContext.state == LINETRACER_CONTROL_EMERGENCY_STOPPED) {
        return 0U;
    }

    return 1U;
}

static void ControlTask_UpdateMotorOutput(uint32_t now_ms) {
    motor_output_t output;
    route_action_t maneuver_action;

    if (controlTaskMotorReady == 0U) {
        LineFollowPid_Reset(&controlTaskLinePid);
        MotorControl_ForceStop();
        return;
    }

    if (ControlTask_RouteMotionEnabled() != 0U) {
        (void)ControlLogic_StartPendingManeuver(&controlTaskContext, now_ms);
    }
    if (ControlTask_RouteMotionEnabled() != 0U && ControlLogic_JunctionManeuverActive(&controlTaskContext) != 0U) {
        LineFollowPid_Reset(&controlTaskLinePid);
        maneuver_action = ControlLogic_JunctionMotorAction(&controlTaskContext);
        if (MotorControlLogic_ComputeRouteAction(maneuver_action, &output) != 0U) {
            ControlTask_ApplyMotorOutput(&output, now_ms);
        }
        return;
    }

    if (ControlTask_PidLineFollowEnabled() != 0U &&
        (controlTaskLineState == LINETRACER_LINE_CENTERED || controlTaskLineState == LINETRACER_LINE_LEFT_ONLY ||
         controlTaskLineState == LINETRACER_LINE_RIGHT_ONLY)) {
#if SENSOR_LINE_USE_ANALOG_PID
        int16_t correction = LineFollowPid_Update(&controlTaskLinePid, controlTaskLineError, now_ms);

        if (MotorControlLogic_ComputeDifferentialForward(LINE_FOLLOW_PID_LEFT_BASE_PWM, LINE_FOLLOW_PID_RIGHT_BASE_PWM,
                                                         correction, &output) != 0U) {
            ControlTask_ApplyMotorOutput(&output, now_ms);
        }
#else
        /* Optional fallback for a digital-only calibration. */
        LineFollowPid_Reset(&controlTaskLinePid);
        if (MotorControlLogic_ComputeLineFollow(controlTaskLineState, &output) != 0U) {
            ControlTask_ApplyMotorOutput(&output, now_ms);
        }
#endif
        return;
    }

    LineFollowPid_Reset(&controlTaskLinePid);
    if (MotorControlLogic_ComputeControlOutput(controlTaskContext.state, controlTaskContext.pending_route_action,
                                               controlTaskLineState, controlTaskContext.route_active,
                                               controlTaskContext.safety_latched, &output) != 0U) {
        ControlTask_ApplyMotorOutput(&output, now_ms);
    }
}

static uint8_t ControlTask_RouteSensorEventsEnabled(void) {
    return ControlTask_RouteMotionEnabled();
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
    uart_error_t error_code;

    if (reason == LINETRACER_STOP_REASON_LOAD_LOST) {
        type = APP_SAFETY_EVENT_LOAD_LOST;
        error_code = UART_ERROR_SENSOR;
    } else if (reason == LINETRACER_STOP_REASON_TURN_TIMEOUT) {
        type = APP_SAFETY_EVENT_TURN_TIMEOUT;
        error_code = UART_ERROR_TIMEOUT;
    } else {
        type = APP_SAFETY_EVENT_MARKER_SEQUENCE;
        error_code = UART_ERROR_SENSOR;
    }

    safety_event.type = type;
    safety_event.occurred_at_ms = now_ms;
    safety_event.reason = reason;
    safety_event.source_task = APP_TASK_CONTROL;
    safety_event.error_code = error_code;
    safety_event.active = 1U;
    if (osMessageQueuePut(safetyEventQueue, &safety_event, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)type, now_ms);
    }

    ControlTask_PublishLifecycleEvent(APP_TX_EVENT_FAULT, controlTaskContext.active_job_id,
                                      controlTaskContext.active_route, UART_STATUS_ERROR, error_code, now_ms);
}

static void ControlTask_PublishUnloadFailure(const app_unload_result_t* result, uint32_t now_ms) {
    app_safety_event_t safety_event = { 0 };
    uart_error_t error_code;

    if (result == NULL) {
        return;
    }

    error_code = (uart_error_t)result->error_code;
    if (error_code != UART_ERROR_TIMEOUT && error_code != UART_ERROR_SERVO && error_code != UART_ERROR_BUSY &&
        error_code != UART_ERROR_INTERNAL) {
        error_code = UART_ERROR_INTERNAL;
    }

    /* Existing safety policy has no unload-specific reason; keep the precise error code. */
    safety_event.type = APP_SAFETY_EVENT_HEALTH_FAULT;
    safety_event.occurred_at_ms = now_ms;
    safety_event.reason = LINETRACER_STOP_REASON_HEALTH_FAULT;
    safety_event.source_task = APP_TASK_UNLOAD;
    safety_event.error_code = (uint8_t)error_code;
    safety_event.active = 1U;
    if (safetyEventQueue == NULL || osMessageQueuePut(safetyEventQueue, &safety_event, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)APP_SAFETY_EVENT_HEALTH_FAULT, now_ms);
    }

    ControlTask_PublishLifecycleEvent(APP_TX_EVENT_FAULT, result->job_id, result->route_id, UART_STATUS_ERROR,
                                      error_code, now_ms);
}

static void ControlTask_StartUnload(uint32_t now_ms) {
    app_unload_command_t command = { 0 };

    command.type = APP_UNLOAD_COMMAND_START;
    command.requested_at_ms = now_ms;
    command.inhibit_generation = UnloadHw_GetSafetyInhibitGeneration();
    command.job_id = controlTaskContext.active_job_id;
    command.route_id = controlTaskContext.active_route;
    if (unloadCommandQueue == NULL || osMessageQueuePut(unloadCommandQueue, &command, 0U, 0U) != osOK) {
        app_unload_result_t failure = { 0 };

        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)command.type, now_ms);
        failure.type = APP_UNLOAD_RESULT_FAILED;
        failure.job_id = command.job_id;
        failure.route_id = command.route_id;
        failure.error_code = (uint8_t)UART_ERROR_BUSY;
        ControlTask_PublishUnloadFailure(&failure, now_ms);
    }
}

static uint8_t ControlTask_RequestUnloadReset(uint32_t now_ms, uint32_t inhibit_generation, uint32_t* request_id) {
    app_unload_command_t command = { 0 };

    if (request_id == NULL) {
        return 0U;
    }

    ++controlTaskNextUnloadRequestId;
    if (controlTaskNextUnloadRequestId == 0U) {
        ++controlTaskNextUnloadRequestId;
    }
    command.type = APP_UNLOAD_COMMAND_RESET;
    command.requested_at_ms = now_ms;
    command.inhibit_generation = inhibit_generation;
    command.request_id = controlTaskNextUnloadRequestId;
    command.route_id = UART_LINETRACER_ROUTE_NONE;
    if (unloadCommandQueue == NULL || osMessageQueuePut(unloadCommandQueue, &command, 0U, 0U) != osOK) {
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)command.type, now_ms);
        return 0U;
    }

    *request_id = command.request_id;
    return 1U;
}

static void ControlTask_ClearPendingUnloadReset(void) {
    controlTaskPendingUnloadReset = (app_control_safety_event_t){ 0 };
    controlTaskPendingUnloadResetDeadlineMs = 0U;
    controlTaskPendingUnloadResetRequestId = 0U;
    controlTaskPendingUnloadResetActive = 0U;
}

static void ControlTask_RejectPendingUnloadReset(uint8_t error_code, uint32_t now_ms) {
    app_control_safety_event_t reset_event;

    if (controlTaskPendingUnloadResetActive == 0U) {
        return;
    }

    reset_event = controlTaskPendingUnloadReset;
    reset_event.type = APP_CONTROL_SAFETY_RESET_REJECTED;
    reset_event.error_code = (error_code != UART_ERROR_NONE) ? error_code : (uint8_t)UART_ERROR_BUSY;
    MotorControl_ForceStop();
    (void)ControlLogic_ApplySafetyEvent(&controlTaskContext, &reset_event, now_ms);
    ControlTask_PublishSafetyResetResult(&reset_event, now_ms);
    ControlTask_ClearPendingUnloadReset();
}

static void ControlTask_CheckPendingUnloadResetTimeout(uint32_t now_ms) {
    if (controlTaskPendingUnloadResetActive == 0U ||
        ControlTask_TimeReached(now_ms, controlTaskPendingUnloadResetDeadlineMs) == 0U) {
        return;
    }

    ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR, CONTROL_HEALTH_UNLOAD_RESET_TIMEOUT, now_ms);
    ControlTask_RejectPendingUnloadReset((uint8_t)UART_ERROR_TIMEOUT, now_ms);
}

static void ControlTask_ProcessUnloadResults(void) {
    app_unload_result_t result;
    uint32_t processed = 0U;

    if (unloadResultQueue == NULL) {
        return;
    }

    while (processed < APP_UNLOAD_RESULT_QUEUE_DEPTH &&
           osMessageQueueGet(unloadResultQueue, &result, NULL, 0U) == osOK) {
        uint32_t now_ms = osKernelGetTickCount();
        uint8_t matches_active_unload =
            (controlTaskContext.state == LINETRACER_CONTROL_UNLOADING &&
             controlTaskContext.active_job_id == result.job_id && controlTaskContext.active_route == result.route_id)
                ? 1U
                : 0U;

        if (result.type == APP_UNLOAD_RESULT_RESET_COMPLETE || result.type == APP_UNLOAD_RESULT_RESET_FAILED) {
            if (controlTaskPendingUnloadResetActive != 0U &&
                result.request_id == controlTaskPendingUnloadResetRequestId &&
                result.inhibit_generation == controlTaskPendingUnloadReset.unload_inhibit_generation) {
                app_control_safety_event_t reset_event = controlTaskPendingUnloadReset;

                if (result.type == APP_UNLOAD_RESULT_RESET_FAILED) {
                    ControlTask_RejectPendingUnloadReset(result.error_code, now_ms);
                } else {
                    (void)ControlLogic_ApplySafetyEvent(&controlTaskContext, &reset_event, now_ms);
                    ControlTask_PublishSafetyResetResult(&reset_event, now_ms);
                    ControlTask_ClearPendingUnloadReset();
                }
            } else {
                ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR,
                                               CONTROL_HEALTH_UNLOAD_RESET_RESULT_MISMATCH, now_ms);
            }
            ++processed;
            continue;
        }

        if (matches_active_unload != 0U) {
            if (result.type == APP_UNLOAD_RESULT_COMPLETE) {
                control_job_completion_t completion =
                    ControlLogic_HandleUnloadResult(&controlTaskContext, &result, now_ms);

                if (completion.completed != 0U) {
                    controlTaskLoadState = UART_LINETRACER_LOAD_EMPTY;
                    ControlTask_PublishLifecycleEvent(APP_TX_EVENT_UNLOAD_COMPLETE, completion.job_id,
                                                      completion.route_id, UART_STATUS_SUCCESS, UART_ERROR_NONE,
                                                      now_ms);
                }
            } else if (result.type == APP_UNLOAD_RESULT_FAILED || result.type == APP_UNLOAD_RESULT_TIMEOUT) {
                ControlTask_PublishUnloadFailure(&result, now_ms);
            }
        }
        ++processed;
    }
}

static void ControlTask_ProcessRouteAction(route_action_t action, linetracer_control_state_t previous_state,
                                           const control_job_completion_t* completion, uint32_t now_ms) {
    uint8_t motor_action_applied = 1U;

    if (action == ROUTE_ACTION_TURN_LEFT || action == ROUTE_ACTION_TURN_RIGHT || action == ROUTE_ACTION_TURN_AROUND) {
        (void)ControlLogic_StartPendingManeuver(&controlTaskContext, now_ms);
    }

    if (ControlLogic_JunctionManeuverActive(&controlTaskContext) == 0U || action == ROUTE_ACTION_STOP_AT_PICKUP ||
        action == ROUTE_ACTION_STOP_AT_DEST || action == ROUTE_ACTION_JOB_COMPLETE ||
        action == ROUTE_ACTION_LOAD_LOST || action == ROUTE_ACTION_ERROR) {
        motor_action_applied = ControlTask_ApplyRouteActionMotor(action, now_ms);
    }

    if (previous_state != controlTaskContext.state && action != ROUTE_ACTION_JOB_COMPLETE) {
        ControlTask_PublishStateChanged(now_ms);
    }

    switch (action) {
        case ROUTE_ACTION_STOP_AT_PICKUP:
            ControlTask_PublishLifecycleEvent(APP_TX_EVENT_ARRIVED, controlTaskContext.active_job_id,
                                              controlTaskContext.active_route, UART_STATUS_SUCCESS, UART_ERROR_NONE,
                                              now_ms);
            SensorTask_RequestFsrBaselineCapture(SENSOR_TASK_FSR_BASELINE_FOR_LOAD_ON);
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
            if (motor_action_applied == 0U) {
                app_unload_result_t failure = { 0 };

                failure.type = APP_UNLOAD_RESULT_FAILED;
                failure.job_id = controlTaskContext.active_job_id;
                failure.route_id = controlTaskContext.active_route;
                failure.error_code = (uint8_t)UART_ERROR_INTERNAL;
                ControlTask_PublishUnloadFailure(&failure, now_ms);
                break;
            }
            /* Destination-marker arrival starts the deterministic servo cycle immediately. */
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
            ControlTask_PublishSafetyFault((controlTaskContext.stop_reason != LINETRACER_STOP_REASON_NONE)
                                               ? controlTaskContext.stop_reason
                                               : LINETRACER_STOP_REASON_MARKER_SEQUENCE,
                                           now_ms);
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

        if (snapshot.line_state <= LINETRACER_LINE_WHITE_GAP) {
            controlTaskLineState = snapshot.line_state;
        }
        controlTaskLineError = snapshot.line_error;

        if (uart_linetracer_load_state_is_valid(snapshot.load_state) != 0U) {
            controlTaskLoadState = snapshot.load_state;
        }

        if (ControlTask_RouteSensorEventsEnabled() == 0U) {
            ++processed;
            continue;
        }

        {
            linetracer_control_state_t previous_state = controlTaskContext.state;
            control_line_result_t line_result = ControlLogic_ProcessLineSampleWithCenter(
                &controlTaskContext, snapshot.line_left, snapshot.line_center, snapshot.line_right, now_ms);

            if (line_result.maneuver_completed != 0U) {
                LineFollowPid_Reset(&controlTaskLinePid);
            }
            if (line_result.action_valid != 0U) {
                LineFollowPid_Reset(&controlTaskLinePid);
                ControlTask_ProcessRouteAction(line_result.action, previous_state, NULL, now_ms);
            } else if (line_result.state_changed != 0U) {
                ControlTask_PublishStateChanged(now_ms);
            }
        }

        if ((snapshot.event_flags & APP_SENSOR_EVENT_MARKER) != 0U && controlTaskContext.route_active != 0U &&
            ControlLogic_ShouldIgnoreMarker(&controlTaskContext, snapshot.marker_code, snapshot.marker_detected_at_ms,
                                            now_ms) == 0U) {
            linetracer_control_state_t previous_state = controlTaskContext.state;
            LineFollowPid_Reset(&controlTaskLinePid);
            route_action_t action = ControlLogic_HandleMarker(&controlTaskContext, snapshot.marker_code,
                                                              snapshot.marker_detected_at_ms, now_ms);

            ControlTask_ProcessRouteAction(action, previous_state, NULL, now_ms);
        }

        if (controlTaskContext.state == LINETRACER_CONTROL_WAITING_LOAD) {
            if (snapshot.load_state == UART_LINETRACER_LOAD_EMPTY) {
                /* Require an empty pickup tray after arrival before accepting a new load. */
                controlTaskContext.load_wait_armed = 1U;
            } else if (snapshot.load_state == UART_LINETRACER_LOAD_PRESENT &&
                       controlTaskContext.load_wait_armed != 0U) {
                linetracer_control_state_t previous_state = controlTaskContext.state;
                route_action_t action = ControlLogic_HandleLoadOn(&controlTaskContext, now_ms);

                ControlTask_ProcessRouteAction(action, previous_state, NULL, now_ms);
            }
        }

        if (controlTaskContext.state == LINETRACER_CONTROL_MOVING_TO_DEST &&
            snapshot.load_state == UART_LINETRACER_LOAD_EMPTY) {
            control_job_completion_t completion;
            linetracer_control_state_t previous_state = controlTaskContext.state;
            route_action_t action = ControlLogic_HandleLoadOff(&controlTaskContext, now_ms, &completion);

            ControlTask_ProcessRouteAction(action, previous_state, &completion, now_ms);
        }
        ++processed;
    }
}

static void ControlTask_CheckRouteTimeout(uint32_t now_ms) {
    linetracer_control_state_t previous_state = controlTaskContext.state;
    linetracer_stop_reason_t reason = ControlLogic_CheckRouteTimeout(&controlTaskContext, now_ms);

    if (reason != LINETRACER_STOP_REASON_NONE) {
        ControlTask_ProcessRouteAction(ROUTE_ACTION_ERROR, previous_state, NULL, now_ms);
    }
}

static void ControlTask_PublishCommandResult(const app_control_command_t* command,
                                             const control_command_result_t* result, uint32_t now_ms) {
    app_tx_event_type_t response_type = ControlLogic_CommandResponseEventType(result);
    app_tx_event_t event = ControlTask_MakeTxEvent(response_type, command, result, now_ms);
    app_tx_event_t started_event;

    ControlTask_PublishTxEvent(&event, now_ms);

    if (response_type == APP_TX_EVENT_STATUS) {
        return;
    }

    if (ControlLogic_BuildStartedEvent(&controlTaskContext, command, result, controlTaskLoadState, now_ms,
                                       &started_event) != 0U) {
        ControlTask_PublishTxEvent(&started_event, now_ms);
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
    command.inhibit_generation = UnloadHw_GetSafetyInhibitGeneration();
    command.job_id = result->action_job_id;
    command.route_id = result->action_route_id;

    if (unloadCommandQueue == NULL || osMessageQueuePut(unloadCommandQueue, &command, 0U, 0U) != osOK) {
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

        if (command.type == APP_CONTROL_COMMAND_STOP_DRIVE && result.accepted != 0U &&
            result.unload_command == APP_UNLOAD_COMMAND_ABORT) {
            MotorControl_ForceStop();
            (void)UnloadTask_RequestAbort();
        }

        ControlTask_PublishCommandResult(&command, &result, now_ms);
        ControlTask_PublishUnloadCommand(&result, now_ms);
        processed_commands++;
    }
}

void StartControlTask(void* argument) {
    uint32_t next_wake_tick;
    uint32_t last_alive_tick;

    (void)argument;
    controlTaskInitialized = 0U;
    controlTaskPendingUnloadReset = (app_control_safety_event_t){ 0 };
    controlTaskPendingUnloadResetActive = 0U;
    controlTaskPendingUnloadResetDeadlineMs = 0U;
    controlTaskPendingUnloadResetRequestId = 0U;
    controlTaskNextUnloadRequestId = 0U;
    next_wake_tick = osKernelGetTickCount();
    last_alive_tick = next_wake_tick;
    ControlLogic_Init(&controlTaskContext, next_wake_tick);
    LineFollowPid_Init(&controlTaskLinePid);
    controlTaskLoadState = UART_LINETRACER_LOAD_EMPTY;
    controlTaskLineState = LINETRACER_LINE_UNKNOWN;
    controlTaskLineError = 0;
    controlTaskMotorReady = MotorControl_Init();
    if (controlTaskMotorReady == 0U) {
        app_control_safety_event_t motor_fault = { 0 };

        motor_fault.type = APP_CONTROL_SAFETY_LATCHED;
        motor_fault.reason = LINETRACER_STOP_REASON_EMERGENCY;
        motor_fault.occurred_at_ms = next_wake_tick;
        motor_fault.error_code = UART_ERROR_INTERNAL;
        (void)ControlLogic_ApplySafetyEvent(&controlTaskContext, &motor_fault, next_wake_tick);
        ControlTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR, CONTROL_HEALTH_MOTOR_INIT_FAILED,
                                       next_wake_tick);
    }
    controlTaskInitialized = 1U;

    for (;;) {
        uint32_t now_ms;

        ControlTask_ProcessSafetyEvents();
        now_ms = osKernelGetTickCount();
        ControlTask_ProcessPendingResponses(now_ms);
        ControlTask_ProcessUnloadResults();
        ControlTask_CheckPendingUnloadResetTimeout(osKernelGetTickCount());
        ControlTask_ProcessSensorSnapshots();
        ControlTask_ProcessCommands();
        now_ms = osKernelGetTickCount();
        ControlTask_CheckRouteTimeout(now_ms);
        ControlTask_UpdateMotorOutput(now_ms);

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
