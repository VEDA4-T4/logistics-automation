#ifndef TEST_STM32F4XX_HAL_H
#define TEST_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum {
    HAL_OK = 0,
    HAL_ERROR = 1
} HAL_StatusTypeDef;

typedef struct {
    uint32_t ErrorCode;
} DMA_HandleTypeDef;

typedef struct {
    void* Instance;
    DMA_HandleTypeDef* hdmarx;
    DMA_HandleTypeDef* hdmatx;
    uint32_t ErrorCode;
} UART_HandleTypeDef;

typedef struct {
    uint32_t unused;
} TIM_HandleTypeDef;

#define USART1 ((void*)0x40011000U)
#define DMA_IT_HT 0x01U
#define HAL_UART_ERROR_DMA 0x10U
#define HAL_DMA_ERROR_NONE 0x00U

#define TIM_CHANNEL_1 1U
#define TIM_CHANNEL_2 2U
#define TIM_CHANNEL_3 3U
#define TIM_CHANNEL_4 4U

#define __HAL_DMA_DISABLE_IT(handle, interrupt) \
    do {                                         \
        (void)(handle);                          \
        (void)(interrupt);                       \
    } while (0)

void fake_hal_tim_set_compare(TIM_HandleTypeDef* timer, uint32_t channel, uint32_t compare);
#define __HAL_TIM_SET_COMPARE(timer, channel, compare) fake_hal_tim_set_compare((timer), (channel), (compare))

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef* uart, uint8_t* data, uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* uart);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* uart, const uint8_t* data, uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef* uart);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* uart);
uint32_t HAL_UART_GetError(UART_HandleTypeDef* uart);
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* timer, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef* timer, uint32_t channel);
uint32_t HAL_GetTick(void);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef* uart);
void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef* uart);
void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart);

#endif /* TEST_STM32F4XX_HAL_H */
