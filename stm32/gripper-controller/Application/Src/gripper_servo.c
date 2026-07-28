#include "gripper_servo.h"

#include <stddef.h>

#include "gripper_calibration.h"
#include "logistics/contracts/uart/gripper_commands.h"
#include "tim.h"

typedef struct {
    uint8_t enabled;
} gripper_servo_mg90s_t;

static gripper_servo_mg90s_t gripperServo;

static uint32_t gripper_servo_angle_to_pulse(uint16_t angle_deci_deg, uint32_t minimum_pulse_us,
                                             uint32_t maximum_pulse_us) {
    const uint32_t pulse_range = maximum_pulse_us - minimum_pulse_us;

    return minimum_pulse_us + (((uint32_t)angle_deci_deg * pulse_range) / UART_GRIPPER_ANGLE_DECI_DEG_MAX);
}

static uint32_t gripper_servo_position_to_pulse(uint8_t position_percent) {
    const uint32_t pulse_range = GRIPPER_OPEN_PULSE_US - GRIPPER_CLOSED_PULSE_US;

    return GRIPPER_CLOSED_PULSE_US + (((uint32_t)position_percent * pulse_range) / UART_GRIPPER_POSITION_MAX);
}

static int32_t gripper_servo_enable(void* context) {
    gripper_servo_mg90s_t* servo = (gripper_servo_mg90s_t*)context;

    if (servo == NULL) {
        return -1;
    }

    if (servo->enabled != 0U) {
        return 0;
    }

    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK) {
        return -2;
    }
    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK) {
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
        return -2;
    }
    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3) != HAL_OK) {
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_2);
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
        return -2;
    }
    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4) != HAL_OK) {
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_3);
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_2);
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
        return -2;
    }

    servo->enabled = 1U;
    return 0;
}

static int32_t gripper_servo_write_arm(void* context, uint16_t base_angle, uint16_t shoulder_angle,
                                       uint16_t elbow_angle) {
    const gripper_servo_mg90s_t* servo = (const gripper_servo_mg90s_t*)context;

    if (servo == NULL || servo->enabled == 0U) {
        return -1;
    }

    __HAL_TIM_SET_COMPARE(
        &htim3, TIM_CHANNEL_1,
        gripper_servo_angle_to_pulse(base_angle, GRIPPER_SERVO_MIN_PULSE_US, GRIPPER_SERVO_MAX_PULSE_US));
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2,
                          gripper_servo_angle_to_pulse(shoulder_angle, GRIPPER_SHOULDER_SERVO_MIN_PULSE_US,
                                                       GRIPPER_SHOULDER_SERVO_MAX_PULSE_US));
    __HAL_TIM_SET_COMPARE(
        &htim3, TIM_CHANNEL_3,
        gripper_servo_angle_to_pulse(elbow_angle, GRIPPER_ELBOW_SERVO_MIN_PULSE_US, GRIPPER_ELBOW_SERVO_MAX_PULSE_US));
    return 0;
}

static int32_t gripper_servo_write_gripper(void* context, uint8_t position_percent) {
    const gripper_servo_mg90s_t* servo = (const gripper_servo_mg90s_t*)context;

    if (servo == NULL || servo->enabled == 0U) {
        return -1;
    }

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, gripper_servo_position_to_pulse(position_percent));
    return 0;
}

const gripper_servo_port_t* gripper_servo_mg90s_port(void) {
    static const gripper_servo_port_t port = {
        .context = &gripperServo,
        .enable = gripper_servo_enable,
        .write_arm = gripper_servo_write_arm,
        .write_gripper = gripper_servo_write_gripper,
    };

    return &port;
}
