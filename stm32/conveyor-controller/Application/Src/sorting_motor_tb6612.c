#include "sorting_motor_tb6612.h"

#include <stddef.h>

#include "conveyor_motor_power.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "main.h"
#include "tim.h"

typedef struct {
    uint8_t initialized;
} sorting_motor_tb6612_context_t;

static sorting_motor_tb6612_context_t sortingMotorContext;

static sorting_motor_result_t sorting_motor_tb6612_initialize(void* context) {
    sorting_motor_tb6612_context_t* motor;

    motor = (sorting_motor_tb6612_context_t*)context;

    if (motor == NULL) {
        return SORTING_MOTOR_ERROR;
    }

    __HAL_TIM_SET_COMPARE(&htim11, TIM_CHANNEL_1, 0U);
    HAL_GPIO_WritePin(SORTING_MOTOR_BIN1_GPIO_Port, SORTING_MOTOR_BIN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SORTING_MOTOR_BIN2_GPIO_Port, SORTING_MOTOR_BIN2_Pin, GPIO_PIN_RESET);

    if (motor->initialized == 0U) {
        if (HAL_TIM_PWM_Start(&htim11, TIM_CHANNEL_1) != HAL_OK) {
            return SORTING_MOTOR_ERROR;
        }

        motor->initialized = 1U;
    }

    return SORTING_MOTOR_OK;
}

static sorting_motor_result_t sorting_motor_tb6612_apply(void* context, uint8_t running, uint8_t speed) {
    sorting_motor_tb6612_context_t* motor;
    uint32_t periodCounts;
    uint32_t pulseCounts;

    motor = (sorting_motor_tb6612_context_t*)context;

    if ((motor == NULL) || (motor->initialized == 0U) || (speed > UART_SORTING_CONVEYOR_SPEED_MAX)) {
        return SORTING_MOTOR_ERROR;
    }

    periodCounts = __HAL_TIM_GET_AUTORELOAD(&htim11) + 1U;
    pulseCounts = ((periodCounts * (uint32_t)speed) + 50U) / 100U;

    __HAL_TIM_SET_COMPARE(&htim11, TIM_CHANNEL_1, 0U);
    HAL_GPIO_WritePin(SORTING_MOTOR_BIN1_GPIO_Port, SORTING_MOTOR_BIN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SORTING_MOTOR_BIN2_GPIO_Port, SORTING_MOTOR_BIN2_Pin, GPIO_PIN_RESET);

    if ((running != 0U) && (speed != 0U)) {
        if (conveyor_motor_power_enable() == 0U) {
            return SORTING_MOTOR_ERROR;
        }

        /* Forward: BIN1=HIGH, BIN2=LOW. Swap BO1/BO2 if mechanics require reversal. */
        HAL_GPIO_WritePin(SORTING_MOTOR_BIN1_GPIO_Port, SORTING_MOTOR_BIN1_Pin, GPIO_PIN_SET);
        __HAL_TIM_SET_COMPARE(&htim11, TIM_CHANNEL_1, pulseCounts);
    }

    return SORTING_MOTOR_OK;
}

const sorting_motor_port_t* sorting_motor_tb6612_port(void) {
    static const sorting_motor_port_t port = { .context = &sortingMotorContext,
                                               .initialize = sorting_motor_tb6612_initialize,
                                               .apply = sorting_motor_tb6612_apply };

    return &port;
}
