#include "comm_rx_task.h"

#include <string.h>

#include "app_comm_tx.h"
#include "app_messages.h"
#include "app_queues.h"
#include "input_control_task.h"
#include "uart_rx.h"

#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"

#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"

#define COMM_RX_QUEUE_WAIT_TICKS 10U
#define UART_START_RETRY_MS      100U

static uart_parser_t uart1Parser;
static uart_parser_t uart6Parser;

static comm_rx_stats_t commRxStats;

static uint8_t uart1RecoveryPending;
static uint8_t uart6RecoveryPending;

static void comm_rx_count_parser_result(
    uart_parser_result_t result)
{
    if (result < UART_PARSER_NO_FRAME)
    {
        commRxStats.parserErrors++;
    }
}

static osStatus_t comm_rx_put_command(
    osMessageQueueId_t queue,
    const control_command_t *message)
{
    if ((queue == NULL) || (message == NULL))
    {
        return osErrorParameter;
    }

    return osMessageQueuePut(
        queue,
        message,
        0U,
        0U);
}

static void comm_rx_send_rejection(
    app_uart_channel_t destination,
    const uart_frame_t *frame,
    uart_status_t status,
    uart_error_t error)
{
    comm_tx_channel_t channel;
    uint8_t payload[UART_RESPONSE_HEADER_SIZE];

    if (frame == NULL)
    {
        return;
    }

    if (destination == APP_UART_CHANNEL_1)
    {
        channel = COMM_TX_CH_INPUT;
    }
    else if (destination == APP_UART_CHANNEL_6)
    {
        channel = COMM_TX_CH_SORTING;
    }
    else
    {
        commRxStats.responseDrops++;
        return;
    }

    payload[UART_RESPONSE_STATUS_INDEX] = (uint8_t)status;
    payload[UART_RESPONSE_COMMAND_INDEX] = frame->command;
    payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)error;

    if (CommTx_SendWithSequence(
            channel,
            frame->sequence,
            UART_CMD_RESPONSE,
            payload,
            sizeof(payload)) != 0)
    {
        commRxStats.responseDrops++;
        return;
    }

    if (status == UART_STATUS_BUSY)
    {
        commRxStats.busyResponses++;
    }
    else
    {
        commRxStats.nackResponses++;
    }
}

static uint8_t comm_rx_forward_command(
    osMessageQueueId_t queue,
    const control_command_t *message,
    uint32_t *successCounter)
{
    if ((message == NULL) || (successCounter == NULL))
    {
        return 0U;
    }

    if (comm_rx_put_command(queue, message) == osOK)
    {
        (*successCounter)++;
        return 1U;
    }
    else
    {
        commRxStats.controlQueueDrops++;
        return 0U;
    }
}

void comm_rx_process_frame(
    app_uart_channel_t source,
    const uart_frame_t *frame)
{
    control_command_t message;
    uint8_t payloadIsValid;

    if (frame == NULL)
    {
        return;
    }

    if ((source != APP_UART_CHANNEL_1) &&
        (source != APP_UART_CHANNEL_6))
    {
        commRxStats.unsupportedCommands++;
        return;
    }

    message.source = source;
    message.frame = *frame;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    message.safetyEpoch = 0U;

    commRxStats.receivedFrames++;

    /*
     * E-Stop과 장치 Reset은 양쪽 UART에서 수신할 수 있다.
     */
    if ((UART_IS_EMERGENCY_COMMAND(frame->command) != 0U) ||
        (frame->command == UART_CMD_RESET_DEVICE))
    {
        if (UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(
                frame->command,
                frame->length) == 0U)
        {
            commRxStats.invalidPayloads++;
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_NACK,
                UART_ERROR_INVALID_PAYLOAD);
            return;
        }

        if (comm_rx_forward_command(
                safetyCommandQueueHandle,
                &message,
                &commRxStats.safetyCommands) == 0U)
        {
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_BUSY,
                UART_ERROR_BUSY);
        }

        return;
    }

    /*
     * USART1: 투입 Raspberry Pi 전용
     */
    if (source == APP_UART_CHANNEL_1)
    {
        if (UART_IS_VALID_INPUT_COMMAND(frame->command) != 0U)
        {
            payloadIsValid =
                UART_IS_VALID_INPUT_PAYLOAD(
                    frame->command,
                    frame->payload,
                    frame->length);
        }
        else
        {
            commRxStats.unsupportedCommands++;
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_NACK,
                UART_ERROR_UNSUPPORTED_COMMAND);
            return;
        }

        if (payloadIsValid == 0U)
        {
            commRxStats.invalidPayloads++;
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_NACK,
                UART_ERROR_INVALID_PAYLOAD);
            return;
        }

        message.safetyEpoch =
            input_control_task_capture_command_epoch();

        if (comm_rx_forward_command(
                inputControlQueueHandle,
                &message,
                &commRxStats.inputCommands) == 0U)
        {
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_BUSY,
                UART_ERROR_BUSY);
        }

        return;
    }

    /*
     * USART6: 분류 Raspberry Pi 전용
     */
    if (source == APP_UART_CHANNEL_6)
    {
        if (UART_IS_VALID_SORTING_COMMAND(frame->command) != 0U)
        {
            payloadIsValid =
                UART_IS_VALID_SORTING_PAYLOAD(
                    frame->command,
                    frame->payload,
                    frame->length);
        }
        else
        {
            commRxStats.unsupportedCommands++;
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_NACK,
                UART_ERROR_UNSUPPORTED_COMMAND);
            return;
        }

        if (payloadIsValid == 0U)
        {
            commRxStats.invalidPayloads++;
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_NACK,
                UART_ERROR_INVALID_PAYLOAD);
            return;
        }

        if (comm_rx_forward_command(
                sortingControlQueueHandle,
                &message,
                &commRxStats.sortingCommands) == 0U)
        {
            comm_rx_send_rejection(
                source,
                frame,
                UART_STATUS_BUSY,
                UART_ERROR_BUSY);
        }

        return;
    }

    commRxStats.unsupportedCommands++;
}

static uart_parser_t *comm_rx_select_parser(
    app_uart_channel_t channel)
{
    if (channel == APP_UART_CHANNEL_1)
    {
        return &uart1Parser;
    }

    if (channel == APP_UART_CHANNEL_6)
    {
        return &uart6Parser;
    }

    return NULL;
}

static void comm_rx_process_chunk(
    const uart_rx_chunk_t *chunk)
{
    uart_parser_t *parser;
    uart_frame_t frame;
    uart_parser_result_t result;
    uint16_t index;

    if ((chunk == NULL) ||
        (chunk->length > UART_RX_CHUNK_SIZE))
    {
        return;
    }

    parser = comm_rx_select_parser(chunk->channel);

    if (parser == NULL)
    {
        return;
    }

    for (index = 0U; index < chunk->length; index++)
    {
        result = uart_parser_feed(
            parser,
            chunk->data[index],
            &frame);

        if (result == UART_PARSER_FRAME_READY)
        {
            comm_rx_process_frame(
                chunk->channel,
                &frame);
        }
        else
        {
            comm_rx_count_parser_result(result);
        }
    }
}

static void comm_rx_service_uart_recovery(void)
{
    if (uart_rx_take_error(APP_UART_CHANNEL_1) != 0U)
    {
        uart1RecoveryPending = 1U;
        uart_parser_reset(&uart1Parser);
    }

    if (uart_rx_take_error(APP_UART_CHANNEL_6) != 0U)
    {
        uart6RecoveryPending = 1U;
        uart_parser_reset(&uart6Parser);
    }

    if (uart1RecoveryPending != 0U)
    {
        if (uart_rx_restart(APP_UART_CHANNEL_1) == HAL_OK)
        {
            uart1RecoveryPending = 0U;
            commRxStats.uartRestarts++;
        }
    }

    if (uart6RecoveryPending != 0U)
    {
        if (uart_rx_restart(APP_UART_CHANNEL_6) == HAL_OK)
        {
            uart6RecoveryPending = 0U;
            commRxStats.uartRestarts++;
        }
    }
}

static void comm_rx_tick_parsers(
    uint32_t elapsedMs)
{
    uart_parser_result_t result;

    if (elapsedMs == 0U)
    {
        return;
    }

    result = uart_parser_tick(
        &uart1Parser,
        elapsedMs);

    comm_rx_count_parser_result(result);

    result = uart_parser_tick(
        &uart6Parser,
        elapsedMs);

    comm_rx_count_parser_result(result);
}

void StartCommRxTask(void *argument)
{
    uart_rx_chunk_t chunk;
    uint32_t previousTick;
    uint32_t currentTick;
    uint32_t elapsedMs;

    (void)argument;

    memset(&commRxStats, 0, sizeof(commRxStats));

    uart_parser_init(&uart1Parser);
    uart_parser_init(&uart6Parser);

    /*
     * UART 초기화가 일시적으로 실패하면
     * 시스템을 멈추지 않고 일정 간격으로 재시도한다.
     */
    while (uart_rx_start() != HAL_OK)
    {
        osDelay(UART_START_RETRY_MS);
    }

    previousTick = HAL_GetTick();

    for (;;)
    {
        if (osMessageQueueGet(
                uartRxQueueHandle,
                &chunk,
                NULL,
                COMM_RX_QUEUE_WAIT_TICKS) == osOK)
        {
            comm_rx_process_chunk(&chunk);
        }

        comm_rx_service_uart_recovery();

        currentTick = HAL_GetTick();
        elapsedMs = currentTick - previousTick;
        previousTick = currentTick;

        comm_rx_tick_parsers(elapsedMs);
    }
}

void comm_rx_get_stats(
    comm_rx_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    *stats = commRxStats;
    stats->rxQueueDrops =
        uart_rx_get_dropped_chunk_count();
}
