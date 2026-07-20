#ifndef TEST_FAKES_STM32F4XX_HAL_H
#define TEST_FAKES_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum { HAL_OK = 0x00U, HAL_ERROR = 0x01U, HAL_BUSY = 0x02U } HAL_StatusTypeDef;

typedef struct __DMA_HandleTypeDef {
    uint32_t ErrorCode;
} DMA_HandleTypeDef;

typedef struct __UART_HandleTypeDef {
    uint32_t ErrorCode;
    DMA_HandleTypeDef* hdmatx;
} UART_HandleTypeDef;

#define HAL_UART_ERROR_NONE 0x00000000U
#define HAL_UART_ERROR_ORE 0x00000008U
#define HAL_UART_ERROR_DMA 0x00000010U
#define HAL_DMA_ERROR_NONE 0x00000000U
#define HAL_DMA_ERROR_TE 0x00000001U

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* huart, const uint8_t* data, uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* huart);

#endif /* TEST_FAKES_STM32F4XX_HAL_H */
