#include "gripper_control.h"

#include <stddef.h>
#include <string.h>

#include "gripper_calibration.h"

static uint8_t gripper_control_is_moving(const gripper_control_t* controller) {
    return (controller->state == UART_GRIPPER_STATE_MOVING_ARM ||
            controller->state == UART_GRIPPER_STATE_MOVING_GRIPPER ||
            controller->state == UART_GRIPPER_STATE_HOMING)
               ? 1U
               : 0U;
}

static uint8_t gripper_control_arm_target_is_safe(uint16_t base_angle, uint16_t shoulder_angle,
                                                  uint16_t elbow_angle) {
    return (base_angle >= GRIPPER_BASE_MIN_ANGLE_DECI_DEG && base_angle <= GRIPPER_BASE_MAX_ANGLE_DECI_DEG &&
            shoulder_angle >= GRIPPER_SHOULDER_MIN_ANGLE_DECI_DEG &&
            shoulder_angle <= GRIPPER_SHOULDER_MAX_ANGLE_DECI_DEG &&
            elbow_angle >= GRIPPER_ELBOW_MIN_ANGLE_DECI_DEG && elbow_angle <= GRIPPER_ELBOW_MAX_ANGLE_DECI_DEG)
               ? 1U
               : 0U;
}

static uint32_t gripper_control_absolute_difference(uint32_t first, uint32_t second) {
    return (first >= second) ? (first - second) : (second - first);
}

static uint32_t gripper_control_duration_for_delta(uint32_t delta, uint32_t units_per_second) {
    if (delta == 0U) {
        return 0U;
    }

    return ((delta * 1000U) + units_per_second - 1U) / units_per_second;
}

static uint32_t gripper_control_max_u32(uint32_t first, uint32_t second) {
    return (first >= second) ? first : second;
}

static uint32_t gripper_control_arm_minimum_duration(const gripper_control_t* controller, uint16_t base_angle,
                                                     uint16_t shoulder_angle, uint16_t elbow_angle) {
    uint32_t duration;

    duration = gripper_control_duration_for_delta(
        gripper_control_absolute_difference(controller->base_angle, base_angle),
        GRIPPER_BASE_MAX_SPEED_DECI_DEG_PER_SEC);
    duration = gripper_control_max_u32(
        duration,
        gripper_control_duration_for_delta(
            gripper_control_absolute_difference(controller->shoulder_angle, shoulder_angle),
            GRIPPER_SHOULDER_MAX_SPEED_DECI_DEG_PER_SEC));
    duration = gripper_control_max_u32(
        duration,
        gripper_control_duration_for_delta(
            gripper_control_absolute_difference(controller->elbow_angle, elbow_angle),
            GRIPPER_ELBOW_MAX_SPEED_DECI_DEG_PER_SEC));
    return duration;
}

static uint32_t gripper_control_claw_minimum_duration(const gripper_control_t* controller,
                                                      uint8_t position_percent) {
    return gripper_control_duration_for_delta(
        gripper_control_absolute_difference(controller->gripper_position, position_percent),
        GRIPPER_CLAW_MAX_SPEED_PERCENT_PER_SEC);
}

static int32_t gripper_control_enable_outputs(gripper_control_t* controller) {
    if (controller->outputs_enabled != 0U) {
        return 0;
    }
    if (controller->servo->enable(controller->servo->context) != 0) {
        return -1;
    }

    controller->outputs_enabled = 1U;
    return 0;
}

static void gripper_control_set_fault(gripper_control_t* controller, uart_error_t error) {
    controller->state = UART_GRIPPER_STATE_FAULT;
    controller->homed = 0U;
    controller->completion.valid = 1U;
    controller->completion.fault = 1U;
    controller->completion.motion_id = controller->active_motion_id;
    controller->completion.motion_type = controller->motion_type;
    controller->completion.error = error;
    controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
}

static gripper_control_result_t gripper_control_start_arm_motion(gripper_control_t* controller, uint16_t motion_id,
                                                                uart_gripper_motion_type_t motion_type,
                                                                uint16_t base_angle, uint16_t shoulder_angle,
                                                                uint16_t elbow_angle, uint32_t duration_ms,
                                                                uint32_t now_ms) {
    uint32_t minimum_duration;

    if (gripper_control_arm_target_is_safe(base_angle, shoulder_angle, elbow_angle) == 0U) {
        return GRIPPER_CONTROL_INVALID_PAYLOAD;
    }
    if (gripper_control_enable_outputs(controller) != 0) {
        controller->state = UART_GRIPPER_STATE_FAULT;
        return GRIPPER_CONTROL_SERVO_ERROR;
    }

    minimum_duration = gripper_control_arm_minimum_duration(controller, base_angle, shoulder_angle, elbow_angle);
    if (motion_type == UART_GRIPPER_MOTION_HOME) {
        minimum_duration = gripper_control_max_u32(
            minimum_duration,
            gripper_control_claw_minimum_duration(controller, controller->target_gripper_position));
    }

    controller->start_base_angle = controller->base_angle;
    controller->start_shoulder_angle = controller->shoulder_angle;
    controller->start_elbow_angle = controller->elbow_angle;
    controller->target_base_angle = base_angle;
    controller->target_shoulder_angle = shoulder_angle;
    controller->target_elbow_angle = elbow_angle;
    controller->motion_start_tick = now_ms;
    controller->motion_duration_ms = gripper_control_max_u32(duration_ms, minimum_duration);
    controller->active_motion_id = motion_id;
    controller->motion_type = motion_type;
    controller->completion.valid = 0U;
    controller->state =
        (motion_type == UART_GRIPPER_MOTION_HOME) ? UART_GRIPPER_STATE_HOMING : UART_GRIPPER_STATE_MOVING_ARM;
    return GRIPPER_CONTROL_OK;
}

static uint16_t gripper_control_interpolate_u16(uint16_t start, uint16_t target, uint32_t elapsed,
                                                uint32_t duration) {
    const int32_t difference = (int32_t)target - (int32_t)start;
    const int32_t value = (int32_t)start + (int32_t)((difference * (int32_t)elapsed) / (int32_t)duration);

    return (uint16_t)value;
}

static uint8_t gripper_control_interpolate_u8(uint8_t start, uint8_t target, uint32_t elapsed, uint32_t duration) {
    const int32_t difference = (int32_t)target - (int32_t)start;
    const int32_t value = (int32_t)start + (int32_t)((difference * (int32_t)elapsed) / (int32_t)duration);

    return (uint8_t)value;
}

gripper_control_result_t gripper_control_init(gripper_control_t* controller, const gripper_servo_port_t* servo) {
    if (controller == NULL || servo == NULL || servo->enable == NULL || servo->write_arm == NULL ||
        servo->write_gripper == NULL) {
        return GRIPPER_CONTROL_INVALID_ARGUMENT;
    }

    memset(controller, 0, sizeof(*controller));
    controller->servo = servo;
    controller->state = UART_GRIPPER_STATE_STOPPED;
    controller->base_angle = GRIPPER_BASE_HOME_ANGLE_DECI_DEG;
    controller->shoulder_angle = GRIPPER_SHOULDER_HOME_ANGLE_DECI_DEG;
    controller->elbow_angle = GRIPPER_ELBOW_HOME_ANGLE_DECI_DEG;
    controller->gripper_position = GRIPPER_INITIAL_POSITION_PERCENT;
    return GRIPPER_CONTROL_OK;
}

gripper_control_result_t gripper_control_process_command(gripper_control_t* controller, uint8_t command,
                                                         const uint8_t* payload, uint8_t length, uint32_t now_ms) {
    uint16_t motion_id;

    if (controller == NULL) {
        return GRIPPER_CONTROL_INVALID_ARGUMENT;
    }
    if (UART_IS_VALID_GRIPPER_PAYLOAD(command, payload, length) == 0U) {
        return GRIPPER_CONTROL_INVALID_PAYLOAD;
    }
    if (controller->safety_latched != 0U && command != UART_CMD_GRIPPER_GET_STATUS) {
        return GRIPPER_CONTROL_SAFETY_STOP;
    }

    switch (command) {
        case UART_CMD_GRIPPER_MOVE_ARM:
            if (controller->homed == 0U) {
                return GRIPPER_CONTROL_NOT_HOMED;
            }
            if (gripper_control_is_moving(controller) != 0U) {
                return GRIPPER_CONTROL_BUSY;
            }
            return gripper_control_start_arm_motion(
                controller, uart_gripper_move_motion_id(payload), UART_GRIPPER_MOTION_ARM,
                uart_gripper_move_base_angle(payload), uart_gripper_move_shoulder_angle(payload),
                uart_gripper_move_elbow_angle(payload), uart_gripper_move_duration_ms(payload), now_ms);

        case UART_CMD_GRIPPER_SET_GRIPPER:
            if (gripper_control_is_moving(controller) != 0U) {
                return GRIPPER_CONTROL_BUSY;
            }
            if (payload[UART_GRIPPER_SET_POSITION_INDEX] < GRIPPER_MIN_POSITION_PERCENT ||
                payload[UART_GRIPPER_SET_POSITION_INDEX] > GRIPPER_MAX_POSITION_PERCENT) {
                return GRIPPER_CONTROL_INVALID_PAYLOAD;
            }
            if (gripper_control_enable_outputs(controller) != 0) {
                controller->state = UART_GRIPPER_STATE_FAULT;
                return GRIPPER_CONTROL_SERVO_ERROR;
            }
            controller->start_gripper_position = controller->gripper_position;
            controller->target_gripper_position = payload[UART_GRIPPER_SET_POSITION_INDEX];
            controller->motion_start_tick = now_ms;
            controller->motion_duration_ms = gripper_control_max_u32(
                uart_gripper_set_duration_ms(payload),
                gripper_control_claw_minimum_duration(controller, controller->target_gripper_position));
            controller->active_motion_id = uart_gripper_set_motion_id(payload);
            controller->motion_type = UART_GRIPPER_MOTION_GRIPPER;
            controller->completion.valid = 0U;
            controller->state = UART_GRIPPER_STATE_MOVING_GRIPPER;
            return GRIPPER_CONTROL_OK;

        case UART_CMD_GRIPPER_HOME:
            if (gripper_control_is_moving(controller) != 0U) {
                return GRIPPER_CONTROL_BUSY;
            }
            motion_id = uart_gripper_home_motion_id(payload);
            controller->start_gripper_position = controller->gripper_position;
            controller->target_gripper_position = GRIPPER_HOME_POSITION_PERCENT;
            return gripper_control_start_arm_motion(
                controller, motion_id, UART_GRIPPER_MOTION_HOME, GRIPPER_BASE_HOME_ANGLE_DECI_DEG,
                GRIPPER_SHOULDER_HOME_ANGLE_DECI_DEG, GRIPPER_ELBOW_HOME_ANGLE_DECI_DEG, GRIPPER_HOME_DURATION_MS,
                now_ms);

        case UART_CMD_GRIPPER_STOP:
            controller->state = UART_GRIPPER_STATE_STOPPED;
            controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
            controller->completion.valid = 0U;
            return GRIPPER_CONTROL_OK;

        case UART_CMD_GRIPPER_GET_STATUS:
            return GRIPPER_CONTROL_OK;

        case UART_CMD_GRIPPER_RESET:
            controller->state = UART_GRIPPER_STATE_STOPPED;
            controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
            controller->completion.valid = 0U;
            controller->homed = 0U;
            return GRIPPER_CONTROL_OK;

        default:
            return GRIPPER_CONTROL_INVALID_PAYLOAD;
    }
}

void gripper_control_tick(gripper_control_t* controller, uint32_t now_ms) {
    uint32_t elapsed;

    if (controller == NULL || gripper_control_is_moving(controller) == 0U || controller->safety_latched != 0U) {
        return;
    }

    elapsed = now_ms - controller->motion_start_tick;
    if (elapsed >= controller->motion_duration_ms) {
        elapsed = controller->motion_duration_ms;
    }

    if (controller->motion_type == UART_GRIPPER_MOTION_GRIPPER) {
        controller->gripper_position = gripper_control_interpolate_u8(
            controller->start_gripper_position, controller->target_gripper_position, elapsed,
            controller->motion_duration_ms);
        if (controller->servo->write_gripper(controller->servo->context, controller->gripper_position) != 0) {
            gripper_control_set_fault(controller, UART_ERROR_SERVO);
            return;
        }
    } else {
        controller->base_angle = gripper_control_interpolate_u16(
            controller->start_base_angle, controller->target_base_angle, elapsed, controller->motion_duration_ms);
        controller->shoulder_angle = gripper_control_interpolate_u16(controller->start_shoulder_angle,
                                                                     controller->target_shoulder_angle, elapsed,
                                                                     controller->motion_duration_ms);
        controller->elbow_angle = gripper_control_interpolate_u16(
            controller->start_elbow_angle, controller->target_elbow_angle, elapsed, controller->motion_duration_ms);
        if (controller->servo->write_arm(controller->servo->context, controller->base_angle,
                                         controller->shoulder_angle, controller->elbow_angle) != 0) {
            gripper_control_set_fault(controller, UART_ERROR_SERVO);
            return;
        }

        if (controller->motion_type == UART_GRIPPER_MOTION_HOME) {
            controller->gripper_position = gripper_control_interpolate_u8(
                controller->start_gripper_position, controller->target_gripper_position, elapsed,
                controller->motion_duration_ms);
            if (controller->servo->write_gripper(controller->servo->context, controller->gripper_position) != 0) {
                gripper_control_set_fault(controller, UART_ERROR_SERVO);
                return;
            }
        }
    }

    if (elapsed == controller->motion_duration_ms) {
        if (controller->motion_type == UART_GRIPPER_MOTION_HOME) {
            controller->homed = 1U;
        }
        controller->completion.valid = 1U;
        controller->completion.fault = 0U;
        controller->completion.motion_id = controller->active_motion_id;
        controller->completion.motion_type = controller->motion_type;
        controller->completion.error = UART_ERROR_NONE;
        controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
        controller->state = UART_GRIPPER_STATE_IDLE;
    }
}

uint8_t gripper_control_take_completion(gripper_control_t* controller, gripper_control_completion_t* completion) {
    if (controller == NULL || completion == NULL || controller->completion.valid == 0U) {
        return 0U;
    }

    *completion = controller->completion;
    controller->completion.valid = 0U;
    return 1U;
}

void gripper_control_apply_safety_stop(gripper_control_t* controller) {
    if (controller == NULL) {
        return;
    }

    controller->safety_latched = 1U;
    controller->homed = 0U;
    controller->state = UART_GRIPPER_STATE_EMERGENCY_STOP;
    controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
    controller->completion.valid = 0U;
}

void gripper_control_release_safety(gripper_control_t* controller) {
    if (controller == NULL) {
        return;
    }

    controller->safety_latched = 0U;
    controller->state = UART_GRIPPER_STATE_STOPPED;
    controller->active_motion_id = UART_GRIPPER_MOTION_ID_NONE;
    controller->completion.valid = 0U;
}

void gripper_control_get_snapshot(const gripper_control_t* controller, gripper_control_snapshot_t* snapshot) {
    if (controller == NULL || snapshot == NULL) {
        return;
    }

    snapshot->state = controller->state;
    snapshot->active_motion_id = controller->active_motion_id;
    snapshot->base_angle = controller->base_angle;
    snapshot->shoulder_angle = controller->shoulder_angle;
    snapshot->elbow_angle = controller->elbow_angle;
    snapshot->gripper_position = controller->gripper_position;
    snapshot->homed = controller->homed;
    snapshot->safety_latched = controller->safety_latched;
}
