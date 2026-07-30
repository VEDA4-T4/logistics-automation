#include "comm_rx_task.h"

#include <stddef.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_messages.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "logistics/contracts/uart/gripper_commands.h"
#include "logistics/contracts/uart_parser.h"
#include "safety_task.h"
#include "stm32f4xx_hal.h"
#include "uart_rx.h"

#define COMM_RX_QUEUE_WAIT_MS 10U

static uart_parser_t commRxParser;
static comm_rx_stats_t commRxStats;
static uint32_t commRxLastTick;

static void comm_rx_send_response(uint8_t sequence, uint8_t original_command, uint8_t status, uint8_t error) {
    uint8_t payload[UART_RESPONSE_HEADER_SIZE];

    payload[UART_RESPONSE_STATUS_INDEX] = status;
    payload[UART_RESPONSE_COMMAND_INDEX] = original_command;
    payload[UART_RESPONSE_ERROR_INDEX] = error;
    if (CommTx_SendWithSequence(sequence, UART_CMD_RESPONSE, payload, sizeof(payload)) != 0) {
        commRxStats.queue_drops++;
    }
}

static uint8_t comm_rx_common_payload_is_valid(const uart_frame_t* frame) {
    return (frame->length == 0U) ? 1U : 0U;
}

static uint8_t comm_rx_enqueue(osMessageQueueId_t queue, const uart_frame_t* frame,
                               app_control_message_kind_t kind) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.frame = *frame;
    message.kind = kind;
    message.safety_epoch = safety_task_get_epoch();

    if (queue == NULL || osMessageQueuePut(queue, &message, 0U, 0U) != osOK) {
        commRxStats.queue_drops++;
        comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_BUSY, UART_ERROR_BUSY);
        return 0U;
    }
    return 1U;
}

void comm_rx_process_frame(const uart_frame_t* frame) {
    if (frame == NULL) {
        return;
    }

    commRxStats.received_frames++;
    if (frame->command == UART_CMD_PING) {
        if (comm_rx_common_payload_is_valid(frame) == 0U) {
            commRxStats.invalid_payloads++;
            comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        } else {
            comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_SUCCESS, UART_ERROR_NONE);
        }
        return;
    }

    if (frame->command == UART_CMD_EMERGENCY_STOP || frame->command == UART_CMD_RESET_DEVICE) {
        app_control_message_kind_t kind;

        if (comm_rx_common_payload_is_valid(frame) == 0U) {
            commRxStats.invalid_payloads++;
            comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
            return;
        }
        kind = (frame->command == UART_CMD_EMERGENCY_STOP) ? APP_CONTROL_MESSAGE_SAFETY_STOP
                                                           : APP_CONTROL_MESSAGE_SAFETY_RELEASE;
        if (comm_rx_enqueue(safetyCommandQueueHandle, frame, kind) != 0U) {
            commRxStats.safety_commands++;
        }
        return;
    }

    if (frame->command == UART_CMD_GET_STATUS) {
        if (comm_rx_common_payload_is_valid(frame) == 0U) {
            commRxStats.invalid_payloads++;
            comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        } else if (comm_rx_enqueue(gripperControlQueueHandle, frame, APP_CONTROL_MESSAGE_UART_COMMAND) != 0U) {
            commRxStats.gripper_commands++;
        }
        return;
    }

    if (UART_IS_VALID_GRIPPER_COMMAND(frame->command) != 0U) {
        if (UART_IS_VALID_GRIPPER_PAYLOAD(frame->command, frame->payload, frame->length) == 0U) {
            commRxStats.invalid_payloads++;
            comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
        } else if (comm_rx_enqueue(gripperControlQueueHandle, frame, APP_CONTROL_MESSAGE_UART_COMMAND) != 0U) {
            commRxStats.gripper_commands++;
        }
        return;
    }

    commRxStats.unsupported_commands++;
    comm_rx_send_response(frame->sequence, frame->command, UART_STATUS_NACK, UART_ERROR_UNSUPPORTED_COMMAND);
}

static void comm_rx_process_chunk(const uart_rx_chunk_t* chunk) {
    uart_frame_t frame;

    if (chunk == NULL || chunk->length > UART_RX_CHUNK_SIZE) {
        return;
    }

    for (uint16_t index = 0U; index < chunk->length; index++) {
        const uart_parser_result_t result = uart_parser_feed(&commRxParser, chunk->data[index], &frame);

        if (result == UART_PARSER_FRAME_READY) {
            comm_rx_process_frame(&frame);
        } else if (result < UART_PARSER_NO_FRAME) {
            commRxStats.parser_errors++;
        }
    }
    commRxLastTick = HAL_GetTick();
}

void comm_rx_get_stats(comm_rx_stats_t* stats) {
    if (stats != NULL) {
        *stats = commRxStats;
    }
}

uint32_t comm_rx_get_last_rx_tick(void) {
    return commRxLastTick;
}

void StartCommRxTask(void* argument) {
    uart_rx_chunk_t chunk;

    (void)argument;
    memset(&commRxStats, 0, sizeof(commRxStats));
    uart_parser_init(&commRxParser);
    while (uart_rx_start() != HAL_OK) {
        commRxStats.uart_restarts++;
        osDelay(UART_RETRY_INTERVAL_MS);
    }

    for (;;) {
        if (uart_rx_take_error() != 0U) {
            uart_parser_reset(&commRxParser);
            if (uart_rx_restart() == HAL_OK) {
                commRxStats.uart_restarts++;
            }
        }

        if (uartRxQueueHandle != NULL &&
            osMessageQueueGet(uartRxQueueHandle, &chunk, NULL, COMM_RX_QUEUE_WAIT_MS) == osOK) {
            comm_rx_process_chunk(&chunk);
        } else if (uart_parser_tick(&commRxParser, COMM_RX_QUEUE_WAIT_MS) < UART_PARSER_NO_FRAME) {
            commRxStats.parser_errors++;
        }
    }
}
