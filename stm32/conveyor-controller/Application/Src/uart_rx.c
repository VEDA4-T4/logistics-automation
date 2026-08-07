#include "uart_rx.h"

#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "usart.h"

#define UART_RX_DMA_BUFFER_SIZE 256U

/* 수신 쪽에서만 발생하는 UART 오류 비트 */
#define UART_RX_ERROR_MASK \
    (HAL_UART_ERROR_PE | HAL_UART_ERROR_NE | HAL_UART_ERROR_FE | HAL_UART_ERROR_ORE)

static uint8_t uart1DmaBuffer[UART_RX_DMA_BUFFER_SIZE];
static uint8_t uart6DmaBuffer[UART_RX_DMA_BUFFER_SIZE];

static volatile uint16_t uart1LastPosition;
static volatile uint16_t uart6LastPosition;

static volatile uint32_t uart1PendingError;
static volatile uint32_t uart6PendingError;

static volatile uint32_t droppedChunkCount;

static void uart_rx_set_error(
    app_uart_channel_t channel,
    uint32_t error)
{
    if (channel == APP_UART_CHANNEL_1)
    {
        uart1PendingError |= error;
    }
    else if (channel == APP_UART_CHANNEL_6)
    {
        uart6PendingError |= error;
    }
}

static void uart_rx_enqueue_bytes(
    app_uart_channel_t channel,
    const uint8_t *data,
    uint16_t length)
{
    uart_rx_chunk_t chunk;
    uint16_t copyLength;

    if ((data == NULL) || (uartRxQueueHandle == NULL))
    {
        return;
    }

    while (length > 0U)
    {
        copyLength = length;

        if (copyLength > UART_RX_CHUNK_SIZE)
        {
            copyLength = UART_RX_CHUNK_SIZE;
        }

        chunk.channel = channel;
        chunk.length = copyLength;

        memcpy(chunk.data, data, copyLength);

        /*
         * 이 함수는 UART/DMA 인터럽트에서 호출된다.
         * ISR에서는 timeout을 반드시 0으로 사용한다.
         */
        if (osMessageQueuePut(
                uartRxQueueHandle,
                &chunk,
                0U,
                0U) != osOK)
        {
            droppedChunkCount++;
        }

        data += copyLength;
        length -= copyLength;
    }
}

static void uart_rx_handle_event(
    app_uart_channel_t channel,
    uint8_t *buffer,
    volatile uint16_t *lastPosition,
    uint16_t position)
{
    if ((buffer == NULL) ||
        (lastPosition == NULL) ||
        (position > UART_RX_DMA_BUFFER_SIZE))
    {
        uart_rx_set_error(channel, HAL_UART_ERROR_DMA);
        return;
    }

    /*
     * position은 DMA 버퍼에서 다음 데이터가 기록될 위치다.
     * lastPosition부터 position 전까지가 새로 들어온 데이터다.
     */
    if (position > *lastPosition)
    {
        uart_rx_enqueue_bytes(
            channel,
            &buffer[*lastPosition],
            position - *lastPosition);
    }
    else if (position < *lastPosition)
    {
        /* Circular DMA 버퍼 끝부분 */
        uart_rx_enqueue_bytes(
            channel,
            &buffer[*lastPosition],
            UART_RX_DMA_BUFFER_SIZE - *lastPosition);

        /* Circular DMA 버퍼 처음부터 현재 위치까지 */
        if (position > 0U)
        {
            uart_rx_enqueue_bytes(
                channel,
                buffer,
                position);
        }
    }
    else
    {
        /* 새 데이터가 없음 */
    }

    if (position == UART_RX_DMA_BUFFER_SIZE)
    {
        *lastPosition = 0U;
    }
    else
    {
        *lastPosition = position;
    }
}

static HAL_StatusTypeDef uart_rx_start_channel(
    app_uart_channel_t channel)
{
    UART_HandleTypeDef *uartHandle;
    uint8_t *buffer;
    volatile uint16_t *lastPosition;

    if (channel == APP_UART_CHANNEL_1)
    {
        uartHandle = &huart1;
        buffer = uart1DmaBuffer;
        lastPosition = &uart1LastPosition;
        uart1PendingError = 0U;
    }
    else if (channel == APP_UART_CHANNEL_6)
    {
        uartHandle = &huart6;
        buffer = uart6DmaBuffer;
        lastPosition = &uart6LastPosition;
        uart6PendingError = 0U;
    }
    else
    {
        return HAL_ERROR;
    }

    *lastPosition = 0U;

    return HAL_UARTEx_ReceiveToIdle_DMA(
        uartHandle,
        buffer,
        UART_RX_DMA_BUFFER_SIZE);
}

HAL_StatusTypeDef uart_rx_start(void)
{
    HAL_StatusTypeDef status;

    status = uart_rx_start_channel(APP_UART_CHANNEL_1);

    if (status != HAL_OK)
    {
        return status;
    }

    status = uart_rx_start_channel(APP_UART_CHANNEL_6);

    if (status != HAL_OK)
    {
        (void)HAL_UART_AbortReceive(&huart1);
        return status;
    }

    return HAL_OK;
}

/*
 * 수신만 중단하고 다시 시작한다.
 *
 * HAL_UART_DMAStop()은 같은 huart의 송신 DMA까지 함께 중단시킨다.
 * 그래서 예전에는 RX를 복구할 때마다 전송 중이던 heartbeat/response가
 * 프레임 중간에서 잘렸고, 송신 완료 콜백도 오지 않아 CommTx가
 * COMM_TX_TX_TIMEOUT_MS를 기다린 뒤에야 재시도를 시작했다.
 * HAL_UART_AbortReceive()는 RX 인터럽트와 RX DMA만 정리하므로
 * 진행 중인 송신은 그대로 끝까지 나간다.
 */
HAL_StatusTypeDef uart_rx_restart(
    app_uart_channel_t channel)
{
    if (channel == APP_UART_CHANNEL_1)
    {
        (void)HAL_UART_AbortReceive(&huart1);
    }
    else if (channel == APP_UART_CHANNEL_6)
    {
        (void)HAL_UART_AbortReceive(&huart6);
    }
    else
    {
        return HAL_ERROR;
    }

    return uart_rx_start_channel(channel);
}

uint32_t uart_rx_take_error(
    app_uart_channel_t channel)
{
    uint32_t primask;
    uint32_t error = 0U;

    /*
     * ISR과 Task가 같은 오류 변수를 사용하므로
     * 아주 짧게 인터럽트를 막고 값을 가져온다.
     */
    primask = __get_PRIMASK();
    __disable_irq();

    if (channel == APP_UART_CHANNEL_1)
    {
        error = uart1PendingError;
        uart1PendingError = 0U;
    }
    else if (channel == APP_UART_CHANNEL_6)
    {
        error = uart6PendingError;
        uart6PendingError = 0U;
    }

    if (primask == 0U)
    {
        __enable_irq();
    }

    return error;
}

uint32_t uart_rx_get_dropped_chunk_count(void)
{
    return droppedChunkCount;
}

void HAL_UARTEx_RxEventCallback(
    UART_HandleTypeDef *huart,
    uint16_t size)
{
    if (huart == &huart1)
    {
        uart_rx_handle_event(
            APP_UART_CHANNEL_1,
            uart1DmaBuffer,
            &uart1LastPosition,
            size);
    }
    else if (huart == &huart6)
    {
        uart_rx_handle_event(
            APP_UART_CHANNEL_6,
            uart6DmaBuffer,
            &uart6LastPosition,
            size);
    }
}

/*
 * 이 오류로 수신을 다시 살려야 하는지 판정한다.
 *
 * HAL_UART_ErrorCallback은 송신 사고에도 똑같이 호출된다.
 * 예전에는 그 구분 없이 복구 플래그를 세워서, 순수 TX DMA 오류가
 * 멀쩡히 돌아가던 RX를 끊는 원인이 됐다.
 */
static uint8_t uart_rx_needs_restart(
    const UART_HandleTypeDef *huart)
{
    if ((huart->ErrorCode & UART_RX_ERROR_MASK) != 0U)
    {
        return 1U;
    }

    /*
     * DMA 오류는 ErrorCode만으로 송신/수신을 구분할 수 없다.
     * HAL이 실제로 수신을 내렸는지(RxState)를 기준으로 판정하면
     * RX DMA 오류와, TX DMA 오류가 RX까지 함께 끊어버린 경우를 모두 잡으면서
     * RX가 살아 있는 순수 송신 사고에서는 재시작하지 않는다.
     */
    if (((huart->ErrorCode & HAL_UART_ERROR_DMA) != 0U) &&
        (huart->RxState != HAL_UART_STATE_BUSY_RX))
    {
        return 1U;
    }

    return 0U;
}

void HAL_UART_ErrorCallback(
    UART_HandleTypeDef *huart)
{
    if (uart_rx_needs_restart(huart) != 0U)
    {
        if (huart == &huart1)
        {
            uart_rx_set_error(
                APP_UART_CHANNEL_1,
                huart->ErrorCode);
        }
        else if (huart == &huart6)
        {
            uart_rx_set_error(
                APP_UART_CHANNEL_6,
                huart->ErrorCode);
        }
    }

    CommTx_HandleUartError(huart);
}
