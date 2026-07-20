#ifdef NDEBUG
#undef NDEBUG
#endif

#include "input_motor_tb6612.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "conveyor_motor_power.h"
#include "main.h"
#include "tim.h"

GPIO_TypeDef inputMotorTestGpioB;
TIM_HandleTypeDef htim1 = { .autoreload = 4199U, .compare = 0U };

static GPIO_PinState ain1State;
static GPIO_PinState ain2State;
static uint32_t pwmStartCalls;
static HAL_StatusTypeDef pwmStartResult;
static uint32_t powerEnableCalls;
static uint8_t powerEnableResult;
static uint8_t powerEnableSawSafeOutputs;

void HAL_GPIO_WritePin(GPIO_TypeDef* gpioPort, uint16_t gpioPin, GPIO_PinState pinState) {
    assert(gpioPort == &inputMotorTestGpioB);

    if (gpioPin == INPUT_MOTOR_AIN1_Pin) {
        ain1State = pinState;
    } else if (gpioPin == INPUT_MOTOR_AIN2_Pin) {
        ain2State = pinState;
    } else {
        assert(0);
    }
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t channel) {
    assert(handle == &htim1);
    assert(channel == TIM_CHANNEL_1);
    pwmStartCalls++;
    return pwmStartResult;
}

uint8_t conveyor_motor_power_enable(void) {
    powerEnableCalls++;
    powerEnableSawSafeOutputs =
        ((htim1.compare == 0U) && (ain1State == GPIO_PIN_RESET) && (ain2State == GPIO_PIN_RESET)) ? 1U : 0U;
    return powerEnableResult;
}

static void reset_apply_observations(void) {
    htim1.compare = 1234U;
    ain1State = GPIO_PIN_SET;
    ain2State = GPIO_PIN_SET;
    powerEnableCalls = 0U;
    powerEnableResult = 1U;
    powerEnableSawSafeOutputs = 0U;
}

static void assert_stopped(void) {
    assert(htim1.compare == 0U);
    assert(ain1State == GPIO_PIN_RESET);
    assert(ain2State == GPIO_PIN_RESET);
}

static void test_initialization_keeps_shared_power_disabled(void) {
    const input_motor_port_t* port;

    port = input_motor_tb6612_port();
    assert(port != NULL);
    assert(port->initialize != NULL);
    assert(port->apply != NULL);

    reset_apply_observations();
    pwmStartCalls = 0U;
    pwmStartResult = HAL_ERROR;

    assert(port->initialize(port->context) == INPUT_MOTOR_ERROR);
    assert(pwmStartCalls == 1U);
    assert(powerEnableCalls == 0U);
    assert_stopped();

    pwmStartResult = HAL_OK;
    assert(port->initialize(port->context) == INPUT_MOTOR_OK);
    assert(pwmStartCalls == 2U);
    assert(powerEnableCalls == 0U);
    assert_stopped();

    assert(port->initialize(port->context) == INPUT_MOTOR_OK);
    assert(pwmStartCalls == 2U);
    assert(powerEnableCalls == 0U);
    assert_stopped();
}

static void test_stopped_outputs_never_enable_shared_power(void) {
    const input_motor_port_t* port;

    port = input_motor_tb6612_port();

    reset_apply_observations();
    assert(port->apply(port->context, 0U, 0U) == INPUT_MOTOR_OK);
    assert(powerEnableCalls == 0U);
    assert_stopped();

    reset_apply_observations();
    assert(port->apply(port->context, 0U, 50U) == INPUT_MOTOR_OK);
    assert(powerEnableCalls == 0U);
    assert_stopped();

    reset_apply_observations();
    assert(port->apply(port->context, 1U, 0U) == INPUT_MOTOR_OK);
    assert(powerEnableCalls == 0U);
    assert_stopped();
}

static void assert_running_at_speed(uint8_t speed, uint32_t expectedCompare) {
    const input_motor_port_t* port;

    port = input_motor_tb6612_port();
    reset_apply_observations();

    assert(port->apply(port->context, 1U, speed) == INPUT_MOTOR_OK);
    assert(powerEnableCalls == 1U);
    assert(powerEnableSawSafeOutputs == 1U);
    assert(ain1State == GPIO_PIN_SET);
    assert(ain2State == GPIO_PIN_RESET);
    assert(htim1.compare == expectedCompare);
}

static void test_running_duty_cycle(void) {
    assert_running_at_speed(1U, 42U);
    assert_running_at_speed(50U, 2100U);
    assert_running_at_speed(100U, 4200U);
}

static void test_latched_power_rejects_start_safely(void) {
    const input_motor_port_t* port;

    port = input_motor_tb6612_port();
    reset_apply_observations();
    powerEnableResult = 0U;

    assert(port->apply(port->context, 1U, 50U) == INPUT_MOTOR_ERROR);
    assert(powerEnableCalls == 1U);
    assert(powerEnableSawSafeOutputs == 1U);
    assert_stopped();
}

int main(void) {
    test_initialization_keeps_shared_power_disabled();
    test_stopped_outputs_never_enable_shared_power();
    test_running_duty_cycle();
    test_latched_power_rejects_start_safely();
    return 0;
}
