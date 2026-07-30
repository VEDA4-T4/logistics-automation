#include "uart_rx.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_messages.h"
#include "app_queues.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

UART_HandleTypeDef huart1 = {.Instance = USART1};
osMessageQueueId_t uartRxQueueHandle = (osMessageQueueId_t)1;
osMessageQueueId_t gripperControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;
osMessageQueueId_t commTxQueueHandle;

static uart_rx_chunk_t capturedChunks[8];
static uint32_t capturedChunkCount;
static uint8_t txErrorHandled;

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void* message, uint8_t priority, uint32_t timeout) {
    (void)priority;
    (void)timeout;
    assert(queue == uartRxQueueHandle);
    assert(capturedChunkCount < (sizeof(capturedChunks) / sizeof(capturedChunks[0])));
    capturedChunks[capturedChunkCount++] = *(const uart_rx_chunk_t*)message;
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void* message, uint8_t* priority, uint32_t timeout) {
    (void)queue;
    (void)message;
    (void)priority;
    (void)timeout;
    return osError;
}

void osDelay(uint32_t ticks) {
    (void)ticks;
}

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef* uart, uint8_t* data, uint16_t length) {
    (void)uart;
    (void)data;
    (void)length;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* uart) {
    (void)uart;
    return HAL_OK;
}

uint32_t HAL_UART_GetError(UART_HandleTypeDef* uart) {
    return uart->ErrorCode;
}

uint8_t CommTx_HandleUartError(UART_HandleTypeDef* uart) {
    (void)uart;
    return txErrorHandled;
}

#include "../Application/Src/uart_rx.c"

static void reset_capture(void) {
    memset(capturedChunks, 0, sizeof(capturedChunks));
    capturedChunkCount = 0U;
}

static void test_transfer_complete_enqueues_full_dma_buffer(void) {
    for (uint32_t index = 0U; index < UART_RX_DMA_BUFFER_SIZE; index++) {
        uartRxDmaBuffer[index] = (uint8_t)index;
    }
    uartRxLastPosition = 0U;
    reset_capture();

    HAL_UARTEx_RxEventCallback(&huart1, UART_RX_DMA_BUFFER_SIZE);

    assert(capturedChunkCount == 4U);
    for (uint32_t chunk = 0U; chunk < capturedChunkCount; chunk++) {
        assert(capturedChunks[chunk].length == UART_RX_CHUNK_SIZE);
        assert(capturedChunks[chunk].data[0] == (uint8_t)(chunk * UART_RX_CHUNK_SIZE));
    }
    assert(uartRxLastPosition == 0U);
}

static void test_wraparound_enqueues_tail_and_head(void) {
    uartRxLastPosition = 250U;
    reset_capture();

    HAL_UARTEx_RxEventCallback(&huart1, 10U);

    assert(capturedChunkCount == 2U);
    assert(capturedChunks[0].length == 6U);
    assert(capturedChunks[1].length == 10U);
    assert(uartRxLastPosition == 10U);
}

static void test_tx_dma_error_is_not_reported_as_rx_error(void) {
    DMA_HandleTypeDef rx_dma = {.ErrorCode = HAL_DMA_ERROR_NONE};

    huart1.hdmarx = &rx_dma;
    huart1.ErrorCode = HAL_UART_ERROR_DMA;
    uartRxError = 0U;
    txErrorHandled = 1U;

    HAL_UART_ErrorCallback(&huart1);

    assert(uart_rx_take_error() == 0U);
}

static void test_rx_dma_error_remains_visible_when_tx_error_is_also_handled(void) {
    DMA_HandleTypeDef rx_dma = {.ErrorCode = 1U};

    huart1.hdmarx = &rx_dma;
    huart1.ErrorCode = HAL_UART_ERROR_DMA;
    uartRxError = 0U;
    txErrorHandled = 1U;

    HAL_UART_ErrorCallback(&huart1);

    assert(uart_rx_take_error() == HAL_UART_ERROR_DMA);
}

int main(void) {
    test_transfer_complete_enqueues_full_dma_buffer();
    test_wraparound_enqueues_tail_and_head();
    test_tx_dma_error_is_not_reported_as_rx_error();
    test_rx_dma_error_remains_visible_when_tx_error_is_also_handled();
    puts("uart_rx_test: PASS");
    return 0;
}
