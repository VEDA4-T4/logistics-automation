#include "unload_hw.h"

#include "tim.h"
#include "unload_config.h"

static volatile uint8_t s_safety_inhibited;
static volatile uint32_t s_safety_inhibit_generation;
static uint8_t s_pwm_started;

static uint32_t UnloadHw_EnterCriticalSection(void) {
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void UnloadHw_ExitCriticalSection(uint32_t primask) {
    if (primask == 0U) {
        __enable_irq();
    }
}

static uint8_t UnloadHw_StopPwm(void) {
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0U);
    if (s_pwm_started == 0U) {
        return 1U;
    }

    if (HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1) != HAL_OK) {
        return 0U;
    }

    s_pwm_started = 0U;
    return 1U;
}

uint8_t UnloadHw_Init(void) {
    /*
     * Do not clear the safety gate here. SafetyTask may have asserted it
     * before UnloadTask gets its first scheduling slot.
     */
    s_pwm_started = 0U;
    return UnloadHw_StopPwm();
}

uint8_t UnloadHw_Apply(unload_servo_output_t output) {
    uint32_t primask;
    uint32_t pulse_us;
    uint8_t status = 1U;

    if (output != UNLOAD_SERVO_OUTPUT_DISABLE && output != UNLOAD_SERVO_OUTPUT_HOME &&
        output != UNLOAD_SERVO_OUTPUT_RELEASE) {
        return 0U;
    }

    if (output == UNLOAD_SERVO_OUTPUT_DISABLE) {
        return UnloadHw_StopPwm();
    }

    if (s_safety_inhibited != 0U) {
        return UnloadHw_StopPwm();
    }

    pulse_us = (output == UNLOAD_SERVO_OUTPUT_RELEASE) ? UNLOAD_SERVO_RELEASE_PULSE_US : UNLOAD_SERVO_HOME_PULSE_US;

    /*
     * Keep the final inhibit check and PWM-enable writes atomic with respect
     * to SafetyTask. SafetyTask can force-stop immediately after this short
     * register update.
     */
    primask = UnloadHw_EnterCriticalSection();
    if (s_safety_inhibited != 0U) {
        UnloadHw_ExitCriticalSection(primask);
        return UnloadHw_StopPwm();
    }

    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, pulse_us);
    if (s_pwm_started == 0U && HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK) {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0U);
        status = 0U;
    } else {
        s_pwm_started = 1U;
    }
    UnloadHw_ExitCriticalSection(primask);
    return status;
}

void UnloadHw_SetSafetyInhibit(uint8_t inhibited) {
    uint32_t primask = UnloadHw_EnterCriticalSection();

    if (inhibited != 0U) {
        ++s_safety_inhibit_generation;
        s_safety_inhibited = 1U;
        /* Immediate register-level shutdown is safe from any task context. */
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0U);
    } else {
        s_safety_inhibited = 0U;
    }
    UnloadHw_ExitCriticalSection(primask);
}

uint8_t UnloadHw_IsSafetyInhibited(void) {
    return s_safety_inhibited;
}

uint32_t UnloadHw_GetSafetyInhibitGeneration(void) {
    uint32_t generation;
    uint32_t primask = UnloadHw_EnterCriticalSection();

    generation = s_safety_inhibit_generation;
    UnloadHw_ExitCriticalSection(primask);
    return generation;
}

uint8_t UnloadHw_ReleaseSafetyInhibit(uint32_t expected_generation) {
    uint8_t released = 0U;
    uint32_t primask = UnloadHw_EnterCriticalSection();

    if (s_safety_inhibit_generation == expected_generation) {
        s_safety_inhibited = 0U;
        released = 1U;
    }
    UnloadHw_ExitCriticalSection(primask);
    return released;
}
