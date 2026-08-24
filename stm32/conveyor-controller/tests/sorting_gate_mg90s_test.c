#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorting_gate_mg90s.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "tim.h"

TIM_HandleTypeDef htim3 = { .autoreload = 19999U, .compare = 0U };

static uint32_t fakeTick;
static uint32_t pwmStartCalls;
static uint32_t pwmStartFailureChannel;

uint32_t HAL_GetTick(void) {
    return fakeTick;
}

void HAL_GPIO_WritePin(GPIO_TypeDef* gpioPort, uint16_t gpioPin, GPIO_PinState pinState) {
    (void)gpioPort;
    (void)gpioPin;
    (void)pinState;
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t channel) {
    assert(handle == &htim3);
    assert((channel == TIM_CHANNEL_2) || (channel == TIM_CHANNEL_3));
    pwmStartCalls++;
    return (channel == pwmStartFailureChannel) ? HAL_ERROR : HAL_OK;
}

int main(void) {
    const sorting_gate_port_t* port = sorting_gate_mg90s_port();
    uint8_t complete;

    assert(port != NULL);
    assert(SORTING_GATE_MG90S_DESTINATION_3_PULSE_US == SORTING_GATE_MG90S_HOME_PULSE_US);
    assert(SORTING_GATE_SG90_DESTINATION_3_PULSE_US == SORTING_GATE_SG90_HOME_PULSE_US);

    pwmStartFailureChannel = TIM_CHANNEL_2;
    assert(port->initialize(port->context) == SORTING_GATE_ERROR);
    assert(htim3.compareByChannel[1] == 0U);
    assert(htim3.compareByChannel[2] == 0U);

    pwmStartFailureChannel = TIM_CHANNEL_3;
    assert(port->initialize(port->context) == SORTING_GATE_ERROR);
    assert(htim3.compareByChannel[1] == 0U);
    assert(htim3.compareByChannel[2] == 0U);

    fakeTick = 100U;
    pwmStartFailureChannel = UINT32_MAX;
    assert(port->initialize(port->context) == SORTING_GATE_OK);
    assert(pwmStartCalls == 5U);
    assert(htim3.compareByChannel[1] == SORTING_GATE_MG90S_HOME_PULSE_US);
    assert(htim3.compareByChannel[2] == SORTING_GATE_SG90_HOME_PULSE_US);
    assert(port->motion_complete(port->context, &complete) == SORTING_GATE_OK);
    assert(complete == 0U);

    fakeTick += SORTING_GATE_SETTLE_TIME_MS;
    assert(port->motion_complete(port->context, &complete) == SORTING_GATE_OK);
    assert(complete == 1U);

    assert(port->move(port->context, UART_SORTING_DESTINATION_1) == SORTING_GATE_OK);
    assert(htim3.compareByChannel[1] == SORTING_GATE_MG90S_DESTINATION_1_PULSE_US);
    assert(htim3.compareByChannel[2] == SORTING_GATE_SG90_DESTINATION_1_PULSE_US);
    assert(port->move(port->context, UART_SORTING_DESTINATION_2) == SORTING_GATE_OK);
    assert(htim3.compareByChannel[1] == SORTING_GATE_MG90S_DESTINATION_2_PULSE_US);
    assert(htim3.compareByChannel[2] == SORTING_GATE_SG90_DESTINATION_2_PULSE_US);
    assert(port->move(port->context, UART_SORTING_DESTINATION_3) == SORTING_GATE_OK);
    assert(htim3.compareByChannel[1] == SORTING_GATE_MG90S_DESTINATION_3_PULSE_US);
    assert(htim3.compareByChannel[2] == SORTING_GATE_SG90_DESTINATION_3_PULSE_US);
    assert(port->move(port->context, (uart_sorting_destination_t)4U) == SORTING_GATE_ERROR);
    return 0;
}
