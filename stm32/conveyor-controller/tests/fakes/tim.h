#ifndef INPUT_MOTOR_TEST_TIM_H
#define INPUT_MOTOR_TEST_TIM_H

#include <stdint.h>

#include "main.h"

typedef struct {
    uint32_t autoreload;
    uint32_t compare;
} TIM_HandleTypeDef;

extern TIM_HandleTypeDef htim1;

#define TIM_CHANNEL_1 1U

#define __HAL_TIM_SET_COMPARE(handle, channel, value) \
    do {                                              \
        (void)(channel);                              \
        (handle)->compare = (uint32_t)(value);        \
    } while (0)

#define __HAL_TIM_GET_AUTORELOAD(handle) ((handle)->autoreload)

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t channel);

#endif /* INPUT_MOTOR_TEST_TIM_H */
