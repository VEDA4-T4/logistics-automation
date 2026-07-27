#include "uart_rx.h"

#include "app_messages.h"
#include "app_queues.h"
#include "usart.h"

#define UART_RX_DMA_BUFFER_SIZE 256U

static uint8_t uartRxDmaBuffer[UART_RX_DMA_BUFFER_SIZE];
static volatile uint16_t uartRxLastPosition;
static volatile uint32_t uartRxError;
static volatile uint32_t uartRxDroppedChunks;

static void uart_rx_enqueue_range(uint16_t start, uint16_t end) {
    uart_rx_chunk_t chunk;
    uint16_t position = start;

    while (position < end) {
        const uint16_t remaining = (uint16_t)(end - position);
        uint16_t chunk_length = remaining;

        if (chunk_length > UART_RX_CHUNK_SIZE) {
            chunk_length = UART_RX_CHUNK_SIZE;
        }
        chunk.length = chunk_length;
        for (uint16_t index = 0U; index < chunk_length; index++) {
            chunk.data[index] = uartRxDmaBuffer[position + index];
        }

        if (uartRxQueueHandle == NULL || osMessageQueuePut(uartRxQueueHandle, &chunk, 0U, 0U) != osOK) {
            uartRxDroppedChunks++;
        }
        position = (uint16_t)(position + chunk_length);
    }
}

HAL_StatusTypeDef uart_rx_start(void) {
    HAL_StatusTypeDef result;

    uartRxLastPosition = 0U;
    result = HAL_UARTEx_ReceiveToIdle_DMA(&huart1, uartRxDmaBuffer, UART_RX_DMA_BUFFER_SIZE);
    if (result == HAL_OK && huart1.hdmarx != NULL) {
        __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
    }
    return result;
}

HAL_StatusTypeDef uart_rx_restart(void) {
    (void)HAL_UART_AbortReceive(&huart1);
    return uart_rx_start();
}

uint32_t uart_rx_take_error(void) {
    const uint32_t error = uartRxError;

    uartRxError = 0U;
    return error;
}

uint32_t uart_rx_get_dropped_chunk_count(void) {
    return uartRxDroppedChunks;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size) {
    uint16_t position;

    if (huart == NULL || huart->Instance != USART1 || size > UART_RX_DMA_BUFFER_SIZE) {
        return;
    }

    position = (size == UART_RX_DMA_BUFFER_SIZE) ? 0U : size;
    if (position > uartRxLastPosition) {
        uart_rx_enqueue_range(uartRxLastPosition, position);
    } else if (position < uartRxLastPosition) {
        uart_rx_enqueue_range(uartRxLastPosition, UART_RX_DMA_BUFFER_SIZE);
        uart_rx_enqueue_range(0U, position);
    }
    uartRxLastPosition = position;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart) {
    if (huart != NULL && huart->Instance == USART1) {
        uartRxError |= HAL_UART_GetError(huart);
    }
}
