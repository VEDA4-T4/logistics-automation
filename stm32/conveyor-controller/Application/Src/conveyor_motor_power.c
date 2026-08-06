#include "conveyor_motor_power.h"

#include "main.h"

static volatile uint8_t motorPowerDisableLatched;

static uint32_t conveyor_motor_power_lock(void) {
    uint32_t interruptMask;

    interruptMask = __get_PRIMASK();
    __disable_irq();
    return interruptMask;
}

static void conveyor_motor_power_unlock(uint32_t interruptMask) {
    __set_PRIMASK(interruptMask);
}

uint8_t conveyor_motor_power_enable(void) {
    uint32_t interruptMask;
    uint8_t enabled;

    interruptMask = conveyor_motor_power_lock();
    enabled = 0U;

    if (motorPowerDisableLatched == 0U) {
        HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_SET);
        enabled = 1U;
    }

    conveyor_motor_power_unlock(interruptMask);
    return enabled;
}

void conveyor_motor_power_latch_disable(void) {
    uint32_t interruptMask;

    interruptMask = conveyor_motor_power_lock();
    motorPowerDisableLatched = 1U;

    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_RESET);

    conveyor_motor_power_unlock(interruptMask);
}

void conveyor_motor_power_release_latch(void) {
    uint32_t interruptMask;

    interruptMask = conveyor_motor_power_lock();
    motorPowerDisableLatched = 0U;

    /* The pin remains LOW until a motor driver explicitly requests enable. */
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_RESET);

    conveyor_motor_power_unlock(interruptMask);
}
