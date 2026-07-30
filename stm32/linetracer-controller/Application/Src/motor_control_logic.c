#include "motor_control_logic.h"

#include <stddef.h>

#include "motor_control_config.h"

static void MotorControlLogic_MakeForward(int32_t left_pwm, int32_t right_pwm, motor_output_t* output) {
    output->left_pwm = MotorControlLogic_ClampPwm(left_pwm + MOTOR_CONTROL_LEFT_TRIM);
    output->right_pwm = MotorControlLogic_ClampPwm(right_pwm + MOTOR_CONTROL_RIGHT_TRIM);
    output->left_direction = MOTOR_DIRECTION_FORWARD;
    output->right_direction = MOTOR_DIRECTION_FORWARD;
    output->standby = 1U;
}

static void MotorControlLogic_MakePivot(motor_direction_t left_direction, motor_direction_t right_direction,
                                        motor_output_t* output) {
    output->left_pwm = MotorControlLogic_ClampPwm(MOTOR_CONTROL_PIVOT_PWM + MOTOR_CONTROL_LEFT_TRIM);
    output->right_pwm = MotorControlLogic_ClampPwm(MOTOR_CONTROL_PIVOT_PWM + MOTOR_CONTROL_RIGHT_TRIM);
    output->left_direction = left_direction;
    output->right_direction = right_direction;
    output->standby = 1U;
}

uint16_t MotorControlLogic_ClampPwm(int32_t pwm) {
    if (pwm <= 0) {
        return 0U;
    }

    if (pwm >= (int32_t)MOTOR_CONTROL_PWM_MAX) {
        return MOTOR_CONTROL_PWM_MAX;
    }

    return (uint16_t)pwm;
}

void MotorControlLogic_MakeSafeStop(motor_output_t* output) {
    if (output == NULL) {
        return;
    }

    output->left_pwm = 0U;
    output->right_pwm = 0U;
    output->left_direction = MOTOR_DIRECTION_COAST;
    output->right_direction = MOTOR_DIRECTION_COAST;
    output->standby = 0U;
}

uint8_t MotorControlLogic_ComputeDifferentialForward(uint16_t base_pwm, int16_t correction,
                                                     motor_output_t* output) {
    if (output == NULL) {
        return 0U;
    }

    MotorControlLogic_MakeForward((int32_t)base_pwm - (int32_t)correction,
                                  (int32_t)base_pwm + (int32_t)correction, output);
    return 1U;
}

uint8_t MotorControlLogic_ComputeLineFollow(linetracer_line_state_t line_state, motor_output_t* output) {
    if (output == NULL) {
        return 0U;
    }

    switch (line_state) {
        case LINETRACER_LINE_CENTERED:
            MotorControlLogic_MakeForward(MOTOR_CONTROL_BASE_PWM, MOTOR_CONTROL_BASE_PWM, output);
            return 1U;

        case LINETRACER_LINE_LEFT_ONLY:
            MotorControlLogic_MakeForward(MOTOR_CONTROL_BASE_PWM - MOTOR_CONTROL_CORRECTION_PWM,
                                          MOTOR_CONTROL_BASE_PWM + MOTOR_CONTROL_CORRECTION_PWM, output);
            return 1U;

        case LINETRACER_LINE_RIGHT_ONLY:
            MotorControlLogic_MakeForward(MOTOR_CONTROL_BASE_PWM + MOTOR_CONTROL_CORRECTION_PWM,
                                          MOTOR_CONTROL_BASE_PWM - MOTOR_CONTROL_CORRECTION_PWM, output);
            return 1U;

        case LINETRACER_LINE_UNKNOWN:
        case LINETRACER_LINE_WHITE_GAP:
        default:
            /* Keep the previous output while crossing a marker. SafetyTask owns a confirmed line-loss stop. */
            return 0U;
    }
}

uint8_t MotorControlLogic_ComputeRouteAction(route_action_t action, motor_output_t* output) {
    if (output == NULL) {
        return 0U;
    }

    switch (action) {
        case ROUTE_ACTION_GO_STRAIGHT:
            MotorControlLogic_MakeForward(MOTOR_CONTROL_BASE_PWM, MOTOR_CONTROL_BASE_PWM, output);
            return 1U;

        case ROUTE_ACTION_TURN_LEFT:
            MotorControlLogic_MakePivot(MOTOR_DIRECTION_REVERSE, MOTOR_DIRECTION_FORWARD, output);
            return 1U;

        case ROUTE_ACTION_TURN_RIGHT:
            MotorControlLogic_MakePivot(MOTOR_DIRECTION_FORWARD, MOTOR_DIRECTION_REVERSE, output);
            return 1U;

        case ROUTE_ACTION_TURN_AROUND:
            MotorControlLogic_MakePivot(MOTOR_DIRECTION_FORWARD, MOTOR_DIRECTION_REVERSE, output);
            return 1U;

        case ROUTE_ACTION_STOP_AT_PICKUP:
        case ROUTE_ACTION_STOP_AT_DEST:
        case ROUTE_ACTION_JOB_COMPLETE:
        case ROUTE_ACTION_LOAD_LOST:
        case ROUTE_ACTION_ERROR:
            MotorControlLogic_MakeSafeStop(output);
            return 1U;

        case ROUTE_ACTION_NONE:
        default:
            return 0U;
    }
}

uint8_t MotorControlLogic_ComputeControlOutput(linetracer_control_state_t state, route_action_t pending_action,
                                               linetracer_line_state_t line_state, uint8_t route_active,
                                               uint8_t safety_latched, motor_output_t* output) {
    if (output == NULL) {
        return 0U;
    }

    if (safety_latched != 0U || route_active == 0U) {
        MotorControlLogic_MakeSafeStop(output);
        return 1U;
    }

    switch (state) {
        case LINETRACER_CONTROL_TURNING_FROM_DEST:
        case LINETRACER_CONTROL_TURNING_TO_PICKUP:
        case LINETRACER_CONTROL_TURNING_AT_PICKUP:
            if (MotorControlLogic_ComputeRouteAction(pending_action, output) == 0U) {
                MotorControlLogic_MakeSafeStop(output);
            }
            return 1U;

        case LINETRACER_CONTROL_MOVING_ON_COMMON_LINE:
            if (pending_action == ROUTE_ACTION_TURN_LEFT || pending_action == ROUTE_ACTION_TURN_RIGHT) {
                return MotorControlLogic_ComputeRouteAction(pending_action, output);
            }
            return MotorControlLogic_ComputeLineFollow(line_state, output);

        case LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION:
        case LINETRACER_CONTROL_MOVING_TO_PICKUP:
        case LINETRACER_CONTROL_MOVING_TO_DEST:
            return MotorControlLogic_ComputeLineFollow(line_state, output);

        case LINETRACER_CONTROL_INITIALIZING:
        case LINETRACER_CONTROL_WAITING_AT_DEST:
        case LINETRACER_CONTROL_PICKUP_READY:
        case LINETRACER_CONTROL_WAITING_LOAD:
        case LINETRACER_CONTROL_UNLOADING:
        case LINETRACER_CONTROL_STOPPED:
        case LINETRACER_CONTROL_OBSTACLE_STOP:
        case LINETRACER_CONTROL_ERROR:
        case LINETRACER_CONTROL_EMERGENCY_STOPPED:
        default:
            MotorControlLogic_MakeSafeStop(output);
            return 1U;
    }
}
