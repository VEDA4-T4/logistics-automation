#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorting_motor_tb6612.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "conveyor_motor_power.h"
#include "main.h"
#include "tim.h"

GPIO_TypeDef sortingMotorTestGpioA;
TIM_HandleTypeDef htim11 = { .autoreload = 4199U, .compare = 0U };

static GPIO_PinState bin1State;
static GPIO_PinState bin2State;
static uint32_t pwmStartCalls;
static HAL_StatusTypeDef pwmStartResult;
static uint32_t powerEnableCalls;
static uint8_t powerEnableResult;
static uint8_t powerEnableSawSafeOutputs;

uint32_t HAL_GetTick(void) {
    return 0U;
}

void HAL_GPIO_WritePin(GPIO_TypeDef* gpioPort, uint16_t gpioPin, GPIO_PinState pinState) {
    assert(gpioPort == &sortingMotorTestGpioA);

    if (gpioPin == SORTING_MOTOR_BIN1_Pin) {
        bin1State = pinState;
    } else if (gpioPin == SORTING_MOTOR_BIN2_Pin) {
        bin2State = pinState;
    } else {
        assert(0);
    }
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t channel) {
    assert(handle == &htim11);
    assert(channel == TIM_CHANNEL_1);
    pwmStartCalls++;
    return pwmStartResult;
}

uint8_t conveyor_motor_power_enable(void) {
    powerEnableCalls++;
    powerEnableSawSafeOutputs =
        ((htim11.compare == 0U) && (bin1State == GPIO_PIN_RESET) && (bin2State == GPIO_PIN_RESET)) ? 1U : 0U;
    return powerEnableResult;
}

static void reset_observations(void) {
    htim11.compare = 1234U;
    bin1State = GPIO_PIN_SET;
    bin2State = GPIO_PIN_SET;
    powerEnableCalls = 0U;
    powerEnableResult = 1U;
    powerEnableSawSafeOutputs = 0U;
}

static void assert_stopped(void) {
    assert(htim11.compare == 0U);
    assert(bin1State == GPIO_PIN_RESET);
    assert(bin2State == GPIO_PIN_RESET);
}

int main(void) {
    const sorting_motor_port_t* port = sorting_motor_tb6612_port();

    assert(port != NULL);
    reset_observations();
    pwmStartResult = HAL_ERROR;
    assert(port->initialize(port->context) == SORTING_MOTOR_ERROR);
    assert_stopped();

    pwmStartResult = HAL_OK;
    assert(port->initialize(port->context) == SORTING_MOTOR_OK);
    assert(powerEnableCalls == 0U);
    assert_stopped();

    reset_observations();
    assert(port->apply(port->context, 1U, 50U) == SORTING_MOTOR_OK);
    assert(powerEnableCalls == 1U);
    assert(powerEnableSawSafeOutputs == 1U);
    assert(bin1State == GPIO_PIN_SET);
    assert(bin2State == GPIO_PIN_RESET);
    assert(htim11.compare == 2100U);

    reset_observations();
    powerEnableResult = 0U;
    assert(port->apply(port->context, 1U, 50U) == SORTING_MOTOR_ERROR);
    assert_stopped();
    return 0;
}
