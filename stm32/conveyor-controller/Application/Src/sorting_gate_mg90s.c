#include "sorting_gate_mg90s.h"

#include <stddef.h>

#include "main.h"
#include "tim.h"

typedef struct {
    uint8_t initialized;
    uint8_t motionActive;
    uint32_t motionStartedAt;
} sorting_gate_mg90s_context_t;

static sorting_gate_mg90s_context_t sortingGateContext;

static uint32_t sorting_gate_mg90s_pulse(uart_sorting_destination_t destination) {
    switch (destination) {
        case UART_SORTING_DESTINATION_NONE:
            return SORTING_GATE_HOME_PULSE_US;

        case UART_SORTING_DESTINATION_1:
            return SORTING_GATE_DESTINATION_1_PULSE_US;

        case UART_SORTING_DESTINATION_2:
            return SORTING_GATE_DESTINATION_2_PULSE_US;

        case UART_SORTING_DESTINATION_3:
            return SORTING_GATE_DESTINATION_3_PULSE_US;

        default:
            return 0U;
    }
}

static sorting_gate_result_t sorting_gate_mg90s_move(void* context, uart_sorting_destination_t destination) {
    sorting_gate_mg90s_context_t* gate;
    uint32_t periodCounts;
    uint32_t pulseCounts;

    gate = (sorting_gate_mg90s_context_t*)context;
    pulseCounts = sorting_gate_mg90s_pulse(destination);
    periodCounts = __HAL_TIM_GET_AUTORELOAD(&htim3) + 1U;

    if ((gate == NULL) || (gate->initialized == 0U) || (pulseCounts == 0U) || (pulseCounts >= periodCounts)) {
        return SORTING_GATE_ERROR;
    }

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, pulseCounts);
    gate->motionStartedAt = HAL_GetTick();
    gate->motionActive = 1U;
    return SORTING_GATE_OK;
}

static sorting_gate_result_t sorting_gate_mg90s_initialize(void* context) {
    sorting_gate_mg90s_context_t* gate;

    gate = (sorting_gate_mg90s_context_t*)context;

    if (gate == NULL) {
        return SORTING_GATE_ERROR;
    }

    if (gate->initialized == 0U) {
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, SORTING_GATE_HOME_PULSE_US);

        if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK) {
            __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0U);
            return SORTING_GATE_ERROR;
        }

        gate->initialized = 1U;
    }

    return sorting_gate_mg90s_move(gate, UART_SORTING_DESTINATION_NONE);
}

static sorting_gate_result_t sorting_gate_mg90s_motion_complete(void* context, uint8_t* complete) {
    sorting_gate_mg90s_context_t* gate;

    gate = (sorting_gate_mg90s_context_t*)context;

    if ((gate == NULL) || (complete == NULL) || (gate->initialized == 0U)) {
        return SORTING_GATE_ERROR;
    }

    if ((gate->motionActive != 0U) &&
        ((uint32_t)(HAL_GetTick() - gate->motionStartedAt) < SORTING_GATE_SETTLE_TIME_MS)) {
        *complete = 0U;
        return SORTING_GATE_OK;
    }

    gate->motionActive = 0U;
    *complete = 1U;
    return SORTING_GATE_OK;
}

const sorting_gate_port_t* sorting_gate_mg90s_port(void) {
    static const sorting_gate_port_t port = { .context = &sortingGateContext,
                                              .initialize = sorting_gate_mg90s_initialize,
                                              .move = sorting_gate_mg90s_move,
                                              .motion_complete = sorting_gate_mg90s_motion_complete };

    return &port;
}
