#include "input_motor_tb6612.h"

#include <stddef.h>

#include "conveyor_motor_power.h"
#include "logistics/contracts/uart/input_commands.h"
#include "main.h"
#include "tim.h"

typedef struct {
    uint8_t initialized;
} input_motor_tb6612_context_t;

static input_motor_tb6612_context_t inputMotorContext;

static input_motor_result_t input_motor_tb6612_initialize(void* context) {
    input_motor_tb6612_context_t* motor;

    motor = (input_motor_tb6612_context_t*)context;

    if (motor == NULL) {
        return INPUT_MOTOR_ERROR;
    }

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);

    HAL_GPIO_WritePin(INPUT_MOTOR_AIN1_GPIO_Port, INPUT_MOTOR_AIN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(INPUT_MOTOR_AIN2_GPIO_Port, INPUT_MOTOR_AIN2_Pin, GPIO_PIN_RESET);

    if (motor->initialized == 0U) {
        if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) {
            return INPUT_MOTOR_ERROR;
        }

        motor->initialized = 1U;
    }

    return INPUT_MOTOR_OK;
}

static input_motor_result_t input_motor_tb6612_apply(void* context, uint8_t running, uint8_t speed) {
    input_motor_tb6612_context_t* motor;
    uint32_t periodCounts;
    uint32_t pulseCounts;

    motor = (input_motor_tb6612_context_t*)context;

    if ((motor == NULL) || (motor->initialized == 0U) || (speed > UART_INPUT_CONVEYOR_SPEED_MAX)) {
        return INPUT_MOTOR_ERROR;
    }

    periodCounts = __HAL_TIM_GET_AUTORELOAD(&htim1) + 1U;
    pulseCounts = ((periodCounts * (uint32_t)speed) + 50U) / 100U;

    /* Remove PWM before changing direction pins. */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);

    HAL_GPIO_WritePin(INPUT_MOTOR_AIN1_GPIO_Port, INPUT_MOTOR_AIN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(INPUT_MOTOR_AIN2_GPIO_Port, INPUT_MOTOR_AIN2_Pin, GPIO_PIN_RESET);

    if ((running != 0U) && (speed != 0U)) {
        /* A latched Safety stop cannot be released by a motor command. */
        if (conveyor_motor_power_enable() == 0U) {
            return INPUT_MOTOR_ERROR;
        }

        /* Forward: AIN1=HIGH, AIN2=LOW. Swap AO1/AO2 if mechanics require reversal. */
        HAL_GPIO_WritePin(INPUT_MOTOR_AIN1_GPIO_Port, INPUT_MOTOR_AIN1_Pin, GPIO_PIN_SET);

        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pulseCounts);
    }
    /* Otherwise PWM=LOW selects the TB6612FNG short-brake stop mode. */

    return INPUT_MOTOR_OK;
}

const input_motor_port_t* input_motor_tb6612_port(void) {
    static const input_motor_port_t port = { .context = &inputMotorContext,
                                             .initialize = input_motor_tb6612_initialize,
                                             .apply = input_motor_tb6612_apply };

    return &port;
}
