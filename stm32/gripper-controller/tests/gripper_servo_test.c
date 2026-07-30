#include "gripper_servo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gripper_calibration.h"
#include "stm32f4xx_hal.h"
#include "tim.h"

TIM_HandleTypeDef htim3;

static uint32_t compareValues[5];
static uint32_t pwmStartCount;

void fake_hal_tim_set_compare(TIM_HandleTypeDef* timer, uint32_t channel, uint32_t compare) {
    assert(timer == &htim3);
    assert(channel >= TIM_CHANNEL_1 && channel <= TIM_CHANNEL_4);
    compareValues[channel] = compare;
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* timer, uint32_t channel) {
    assert(timer == &htim3);
    assert(channel >= TIM_CHANNEL_1 && channel <= TIM_CHANNEL_4);
    pwmStartCount++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef* timer, uint32_t channel) {
    (void)timer;
    (void)channel;
    return HAL_OK;
}

#include "../Application/Src/gripper_servo.c"

int main(void) {
    const gripper_servo_port_t* port = gripper_servo_mg90s_port();

    memset(compareValues, 0, sizeof(compareValues));
    assert(port->enable(port->context) == 0);
    assert(pwmStartCount == 4U);
    assert(compareValues[TIM_CHANNEL_1] == 1500U);
    assert(compareValues[TIM_CHANNEL_2] == 1500U);
    assert(compareValues[TIM_CHANNEL_3] == 1500U);
    assert(compareValues[TIM_CHANNEL_4] == 1680U);
    puts("gripper_servo_test: PASS");
    return 0;
}
