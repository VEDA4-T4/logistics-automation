#include "app_comm_tx.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_messages.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

static DMA_HandleTypeDef txDma;
UART_HandleTypeDef huart1 = {.Instance = USART1, .hdmatx = &txDma};
osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t gripperControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;
osMessageQueueId_t commTxQueueHandle = (osMessageQueueId_t)1;

static uint32_t threadFlags;
static uint32_t dmaStartCount;
static uint32_t abortCount;
static uint32_t delayCount;
static uint8_t timeoutFirstAttempt;
static uint8_t signalDmaError;

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void* message, uint8_t priority, uint32_t timeout) {
    (void)queue;
    (void)message;
    (void)priority;
    (void)timeout;
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void* message, uint8_t* priority, uint32_t timeout) {
    (void)queue;
    (void)message;
    (void)priority;
    (void)timeout;
    return osError;
}

osThreadId_t osThreadGetId(void) {
    return (osThreadId_t)1;
}

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags) {
    assert(thread_id == (osThreadId_t)1);
    threadFlags |= flags;
    return threadFlags;
}

uint32_t osThreadFlagsClear(uint32_t flags) {
    const uint32_t previous = threadFlags;

    threadFlags &= ~flags;
    return previous;
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout) {
    uint32_t matched;

    (void)options;
    (void)timeout;
    matched = threadFlags & flags;
    if (matched == 0U) {
        return osFlagsErrorTimeout;
    }
    threadFlags &= ~matched;
    return matched;
}

void osDelay(uint32_t ticks) {
    delayCount += ticks;
}

uint32_t HAL_GetTick(void) {
    return 0U;
}

HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* uart, const uint8_t* data, uint16_t length) {
    (void)data;
    assert(uart == &huart1);
    assert(length > 0U);
    dmaStartCount++;

    if (timeoutFirstAttempt != 0U && dmaStartCount == 1U) {
        return HAL_OK;
    }
    if (signalDmaError != 0U) {
        uart->ErrorCode = HAL_UART_ERROR_DMA;
        uart->hdmatx->ErrorCode = 1U;
        HAL_UART_ErrorCallback(uart);
    } else {
        HAL_UART_TxCpltCallback(uart);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortTransmit_IT(UART_HandleTypeDef* uart) {
    assert(uart == &huart1);
    abortCount++;
    HAL_UART_AbortTransmitCpltCallback(uart);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* uart) {
    assert(uart == &huart1);
    abortCount++;
    return HAL_OK;
}

uint32_t HAL_UART_GetError(UART_HandleTypeDef* uart) {
    return uart->ErrorCode;
}

#include "../Application/Src/app_comm_tx.c"

void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart) {
    CommTx_HandleUartError(uart);
}

static comm_tx_message_t make_message(void) {
    comm_tx_message_t message;

    memset(&message, 0, sizeof(message));
    message.command = UART_CMD_PING;
    return message;
}

static void reset_fixture(void) {
    memset(&commTxStats, 0, sizeof(commTxStats));
    threadFlags = 0U;
    dmaStartCount = 0U;
    abortCount = 0U;
    delayCount = 0U;
    timeoutFirstAttempt = 0U;
    signalDmaError = 0U;
    txDma.ErrorCode = HAL_DMA_ERROR_NONE;
    huart1.ErrorCode = 0U;
    commTxTaskId = osThreadGetId();
    commTxState = COMM_TX_STATE_IDLE;
}

static void test_dma_completion_sends_frame(void) {
    const comm_tx_message_t message = make_message();

    reset_fixture();
    comm_tx_send_message(&message);

    assert(dmaStartCount == 1U);
    assert(commTxStats.sent == 1U);
    assert(commTxStats.transmit_errors == 0U);
    assert(commTxState == COMM_TX_STATE_IDLE);
}

static void test_dma_timeout_aborts_and_retries(void) {
    const comm_tx_message_t message = make_message();

    reset_fixture();
    timeoutFirstAttempt = 1U;
    comm_tx_send_message(&message);

    assert(dmaStartCount == 2U);
    assert(abortCount == 1U);
    assert(commTxStats.dma_timeouts == 1U);
    assert(commTxStats.retries == 1U);
    assert(commTxStats.sent == 1U);
    assert(delayCount == UART_RETRY_INTERVAL_MS);
}

static void test_dma_error_is_reported_and_exhausts_retries(void) {
    const comm_tx_message_t message = make_message();

    reset_fixture();
    signalDmaError = 1U;
    comm_tx_send_message(&message);

    assert(dmaStartCount == UART_MAX_RETRY_COUNT + 1U);
    assert(abortCount == UART_MAX_RETRY_COUNT + 1U);
    assert(commTxStats.dma_errors == UART_MAX_RETRY_COUNT + 1U);
    assert(commTxStats.transmit_errors == 1U);
    assert(commTxStats.sent == 0U);
}

int main(void) {
    test_dma_completion_sends_frame();
    test_dma_timeout_aborts_and_retries();
    test_dma_error_is_reported_and_exhausts_retries();
    puts("app_comm_tx_test: PASS");
    return 0;
}
