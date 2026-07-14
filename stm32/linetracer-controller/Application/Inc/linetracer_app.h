#ifndef LINETRACER_APP_H
#define LINETRACER_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "app_config.h"

void App_Init(TIM_HandleTypeDef *motor_pwm_timer,
              UART_HandleTypeDef *command_uart);
void App_Task(void);

/* HAL callback에서 호출된다. */
void App_UartRxCompleteFromISR(UART_HandleTypeDef *huart);
void App_UartErrorFromISR(UART_HandleTypeDef *huart);
void App_EStopFromISR(uint16_t gpio_pin);

#ifdef __cplusplus
}
#endif

#endif
