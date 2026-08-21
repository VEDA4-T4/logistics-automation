#include "motor_control.h"

#include <stddef.h>

#include "main.h"
#include "motor_control_config.h"
#include "tim.h"

/*
 * Physical wiring on this vehicle is intentionally crossed relative to the
 * JMOD labels: the left wheel is connected to channel B (BO1/BO2) and the
 * right wheel is connected to channel A (AO1/AO2).  Keep the rest of the
 * application in logical vehicle coordinates (left/right); only this HAL
 * boundary translates them to the actual driver channels.
 */
#define MOTOR_LEFT_IN1_PIN GPIO_PIN_2 /* BIN1 */
#define MOTOR_LEFT_IN2_PIN GPIO_PIN_3 /* BIN2 */
#define MOTOR_RIGHT_IN1_PIN GPIO_PIN_0 /* AIN1 */
#define MOTOR_RIGHT_IN2_PIN GPIO_PIN_1 /* AIN2 */
#define MOTOR_STBY_PIN GPIO_PIN_4
#define MOTOR_GPIO_PORT GPIOC

static uint8_t motorControlInitialized;
static motor_output_t motorControlLastOutput;

static motor_direction_t MotorControl_PhysicalDirection(motor_direction_t direction, uint8_t reversed) {
    if (reversed == 0U || direction == MOTOR_DIRECTION_COAST) {
        return direction;
    }

    return (direction == MOTOR_DIRECTION_FORWARD) ? MOTOR_DIRECTION_REVERSE : MOTOR_DIRECTION_FORWARD;
}

static void MotorControl_WriteDirection(uint16_t in1_pin, uint16_t in2_pin, motor_direction_t direction) {
    GPIO_PinState in1 = GPIO_PIN_RESET;
    GPIO_PinState in2 = GPIO_PIN_RESET;

    if (direction == MOTOR_DIRECTION_FORWARD) {
        in1 = GPIO_PIN_SET;
    } else if (direction == MOTOR_DIRECTION_REVERSE) {
        in2 = GPIO_PIN_SET;
    }

    HAL_GPIO_WritePin(MOTOR_GPIO_PORT, in1_pin, in1);
    HAL_GPIO_WritePin(MOTOR_GPIO_PORT, in2_pin, in2);
}

static uint8_t MotorControl_OutputIsValid(const motor_output_t* output) {
    if (output == NULL || output->left_pwm > MOTOR_CONTROL_PWM_MAX || output->right_pwm > MOTOR_CONTROL_PWM_MAX ||
        output->left_direction > MOTOR_DIRECTION_REVERSE || output->right_direction > MOTOR_DIRECTION_REVERSE ||
        output->standby > 1U) {
        return 0U;
    }

    return 1U;
}

static uint8_t MotorControl_DirectionChanged(const motor_output_t* output) {
    return (output->left_direction != motorControlLastOutput.left_direction ||
            output->right_direction != motorControlLastOutput.right_direction ||
            output->standby != motorControlLastOutput.standby)
               ? 1U
               : 0U;
}

void MotorControl_ForceStop(void) {
    /* Logical left is physical channel B/TIM3_CH2; logical right is A/CH1. */
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0U);
    HAL_GPIO_WritePin(MOTOR_GPIO_PORT, MOTOR_STBY_PIN, GPIO_PIN_RESET);
    MotorControl_WriteDirection(MOTOR_LEFT_IN1_PIN, MOTOR_LEFT_IN2_PIN, MOTOR_DIRECTION_COAST);
    MotorControl_WriteDirection(MOTOR_RIGHT_IN1_PIN, MOTOR_RIGHT_IN2_PIN, MOTOR_DIRECTION_COAST);
    MotorControlLogic_MakeSafeStop(&motorControlLastOutput);
}

uint8_t MotorControl_Init(void) {
    motorControlInitialized = 0U;
    MotorControl_ForceStop();

    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK) {
        return 0U;
    }

    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK) {
        (void)HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
        MotorControl_ForceStop();
        return 0U;
    }

    motorControlInitialized = 1U;
    MotorControl_ForceStop();
    return 1U;
}

uint8_t MotorControl_Apply(const motor_output_t* output) {
    motor_direction_t left_direction;
    motor_direction_t right_direction;

    if (motorControlInitialized == 0U || MotorControl_OutputIsValid(output) == 0U) {
        MotorControl_ForceStop();
        return 0U;
    }

    if (output->standby == 0U || (output->left_pwm == 0U && output->right_pwm == 0U)) {
        MotorControl_ForceStop();
        return 1U;
    }

    left_direction = MotorControl_PhysicalDirection(output->left_direction, MOTOR_CONTROL_LEFT_REVERSED);
    right_direction = MotorControl_PhysicalDirection(output->right_direction, MOTOR_CONTROL_RIGHT_REVERSED);

    if (MotorControl_DirectionChanged(output) != 0U) {
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0U);
        HAL_GPIO_WritePin(MOTOR_GPIO_PORT, MOTOR_STBY_PIN, GPIO_PIN_RESET);
        MotorControl_WriteDirection(MOTOR_LEFT_IN1_PIN, MOTOR_LEFT_IN2_PIN, left_direction);
        MotorControl_WriteDirection(MOTOR_RIGHT_IN1_PIN, MOTOR_RIGHT_IN2_PIN, right_direction);
    }

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, output->left_pwm);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, output->right_pwm);
    HAL_GPIO_WritePin(MOTOR_GPIO_PORT, MOTOR_STBY_PIN, GPIO_PIN_SET);
    motorControlLastOutput = *output;
    return 1U;
}

void MotorControl_GetLastOutput(motor_output_t* output) {
    if (output != NULL) {
        *output = motorControlLastOutput;
    }
}
