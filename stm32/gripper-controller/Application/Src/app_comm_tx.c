#include "app_comm_tx.h"

#include <stddef.h>
#include <string.h>

#include "app_messages.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "logistics/contracts/uart_codec.h"
#include "usart.h"

#define COMM_TX_NORMAL_PRIORITY 0U
#define COMM_TX_URGENT_PRIORITY 3U
#define COMM_TX_DMA_TIMEOUT_MS 50U
#define COMM_TX_ABORT_TIMEOUT_MS 20U
#define COMM_TX_HEARTBEAT_PERIOD_MS 1000U

#define COMM_TX_FLAG_COMPLETE (1UL << 0U)
#define COMM_TX_FLAG_ERROR (1UL << 1U)
#define COMM_TX_FLAG_ABORT_COMPLETE (1UL << 2U)

typedef enum {
    COMM_TX_STATE_IDLE = 0U,
    COMM_TX_STATE_IN_FLIGHT,
    COMM_TX_STATE_ABORTING
} comm_tx_state_t;

static comm_tx_stats_t commTxStats;
static uint8_t commTxSequence;
static uint8_t commTxDeviceState = UART_DEVICE_IDLE;
static uint8_t commTxError = UART_ERROR_NONE;
static osThreadId_t commTxTaskId;
static volatile comm_tx_state_t commTxState = COMM_TX_STATE_IDLE;
static uint8_t commTxEncodedBuffer[UART_MAX_FRAME_SIZE];

static int32_t comm_tx_enqueue(uint8_t sequence, uint8_t use_sequence, uint8_t command, const uint8_t* payload,
                               uint8_t length, uint8_t priority) {
    comm_tx_message_t message;

    if (commTxQueueHandle == NULL || length > UART_MAX_PAYLOAD_SIZE || (length > 0U && payload == NULL) ||
        UART_IS_VALID_COMMAND(command) == 0U) {
        commTxStats.dropped++;
        return -1;
    }

    memset(&message, 0, sizeof(message));
    message.sequence = sequence;
    message.use_provided_sequence = use_sequence;
    message.command = command;
    message.length = length;
    if (length > 0U) {
        memcpy(message.payload, payload, length);
    }

    if (osMessageQueuePut(commTxQueueHandle, &message, priority, 0U) != osOK) {
        commTxStats.dropped++;
        return -1;
    }

    if (priority == COMM_TX_URGENT_PRIORITY) {
        commTxStats.urgent_enqueued++;
    } else {
        commTxStats.enqueued++;
    }
    return 0;
}

int32_t CommTx_Send(uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(0U, 0U, command, payload, length, COMM_TX_NORMAL_PRIORITY);
}

int32_t CommTx_SendWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(sequence, 1U, command, payload, length, COMM_TX_NORMAL_PRIORITY);
}

int32_t CommTx_SendUrgent(uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(0U, 0U, command, payload, length, COMM_TX_URGENT_PRIORITY);
}

int32_t CommTx_SendUrgentWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(sequence, 1U, command, payload, length, COMM_TX_URGENT_PRIORITY);
}

void CommTx_SetDeviceStatus(uint8_t state, uint8_t error) {
    commTxDeviceState = state;
    commTxError = error;
}

void CommTx_GetStats(comm_tx_stats_t* stats) {
    if (stats != NULL) {
        *stats = commTxStats;
    }
}

static void comm_tx_abort_transmit(void) {
    uint32_t flags;

    commTxState = COMM_TX_STATE_ABORTING;
    (void)osThreadFlagsClear(COMM_TX_FLAG_ABORT_COMPLETE);
    if (HAL_UART_AbortTransmit_IT(&huart1) != HAL_OK) {
        (void)HAL_UART_AbortTransmit(&huart1);
        commTxState = COMM_TX_STATE_IDLE;
        return;
    }

    commTxStats.aborts++;
    flags = osThreadFlagsWait(COMM_TX_FLAG_ABORT_COMPLETE, osFlagsWaitAny, COMM_TX_ABORT_TIMEOUT_MS);
    if ((flags & osFlagsError) != 0U) {
        commTxStats.abort_timeouts++;
        (void)HAL_UART_AbortTransmit(&huart1);
    }
    commTxState = COMM_TX_STATE_IDLE;
}

static uint8_t comm_tx_transmit_dma(const uint8_t* encoded, uint16_t encoded_length) {
    uint32_t flags;

    (void)osThreadFlagsClear(COMM_TX_FLAG_COMPLETE | COMM_TX_FLAG_ERROR | COMM_TX_FLAG_ABORT_COMPLETE);
    commTxState = COMM_TX_STATE_IN_FLIGHT;
    if (HAL_UART_Transmit_DMA(&huart1, encoded, encoded_length) != HAL_OK) {
        commTxStats.dma_start_errors++;
        commTxState = COMM_TX_STATE_IDLE;
        return 0U;
    }

    flags = osThreadFlagsWait(COMM_TX_FLAG_COMPLETE | COMM_TX_FLAG_ERROR, osFlagsWaitAny, COMM_TX_DMA_TIMEOUT_MS);
    if ((flags & osFlagsError) == 0U && (flags & COMM_TX_FLAG_ERROR) == 0U &&
        (flags & COMM_TX_FLAG_COMPLETE) != 0U) {
        commTxState = COMM_TX_STATE_IDLE;
        return 1U;
    }

    if (flags == osFlagsErrorTimeout) {
        commTxStats.dma_timeouts++;
    } else {
        commTxStats.dma_errors++;
    }
    comm_tx_abort_transmit();
    return 0U;
}

static void comm_tx_send_message(const comm_tx_message_t* message) {
    uart_frame_t frame;
    size_t encoded_length = 0U;
    uint32_t attempt;

    memset(&frame, 0, sizeof(frame));
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = (message->use_provided_sequence != 0U) ? message->sequence : commTxSequence++;
    frame.command = message->command;
    frame.length = message->length;
    if (message->length > 0U) {
        memcpy(frame.payload, message->payload, message->length);
    }

    if (uart_encode_frame(&frame, commTxEncodedBuffer, sizeof(commTxEncodedBuffer), &encoded_length) != UART_CODEC_OK) {
        commTxStats.encode_errors++;
        return;
    }

    for (attempt = 0U; attempt <= UART_MAX_RETRY_COUNT; attempt++) {
        if (comm_tx_transmit_dma(commTxEncodedBuffer, (uint16_t)encoded_length) != 0U) {
            commTxStats.sent++;
            return;
        }
        if (attempt < UART_MAX_RETRY_COUNT) {
            commTxStats.retries++;
            osDelay(UART_RETRY_INTERVAL_MS);
        }
    }
    commTxStats.transmit_errors++;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    if (huart != NULL && huart->Instance == USART1 && commTxTaskId != NULL &&
        commTxState == COMM_TX_STATE_IN_FLIGHT) {
        (void)osThreadFlagsSet(commTxTaskId, COMM_TX_FLAG_COMPLETE);
    }
}

void HAL_UART_AbortTransmitCpltCallback(UART_HandleTypeDef* huart) {
    if (huart != NULL && huart->Instance == USART1 && commTxTaskId != NULL &&
        commTxState == COMM_TX_STATE_ABORTING) {
        (void)osThreadFlagsSet(commTxTaskId, COMM_TX_FLAG_ABORT_COMPLETE);
    }
}

uint8_t CommTx_HandleUartError(UART_HandleTypeDef* huart) {
    if (huart == NULL || huart->Instance != USART1 || commTxTaskId == NULL ||
        commTxState != COMM_TX_STATE_IN_FLIGHT || (huart->ErrorCode & HAL_UART_ERROR_DMA) == 0U ||
        huart->hdmatx == NULL || huart->hdmatx->ErrorCode == HAL_DMA_ERROR_NONE) {
        return 0U;
    }

    (void)osThreadFlagsSet(commTxTaskId, COMM_TX_FLAG_ERROR);
    return 1U;
}

static void comm_tx_enqueue_heartbeat(void) {
    uint8_t payload[APP_HEARTBEAT_PAYLOAD_SIZE];
    const uint32_t uptime = HAL_GetTick() / 1000U;

    payload[0] = APP_EVENT_HEARTBEAT;
    payload[APP_HEARTBEAT_STATE_INDEX] = commTxDeviceState;
    payload[APP_HEARTBEAT_ERROR_INDEX] = commTxError;
    payload[APP_HEARTBEAT_UPTIME_INDEX] = (uint8_t)(uptime & 0xFFU);
    payload[APP_HEARTBEAT_UPTIME_INDEX + 1U] = (uint8_t)((uptime >> 8U) & 0xFFU);
    payload[APP_HEARTBEAT_UPTIME_INDEX + 2U] = (uint8_t)((uptime >> 16U) & 0xFFU);
    payload[APP_HEARTBEAT_UPTIME_INDEX + 3U] = (uint8_t)((uptime >> 24U) & 0xFFU);

    if (CommTx_Send(UART_CMD_EVENT, payload, sizeof(payload)) == 0) {
        commTxStats.heartbeats++;
    }
}

void StartCommTxTask(void* argument) {
    comm_tx_message_t message;
    uint32_t last_heartbeat;

    (void)argument;
    memset(&commTxStats, 0, sizeof(commTxStats));
    commTxTaskId = osThreadGetId();
    commTxState = COMM_TX_STATE_IDLE;
    last_heartbeat = HAL_GetTick();

    for (;;) {
        if ((HAL_GetTick() - last_heartbeat) >= COMM_TX_HEARTBEAT_PERIOD_MS) {
            comm_tx_enqueue_heartbeat();
            last_heartbeat = HAL_GetTick();
        }

        if (commTxQueueHandle == NULL || osMessageQueueGet(commTxQueueHandle, &message, NULL, 10U) != osOK) {
            continue;
        }
        comm_tx_send_message(&message);
    }
}
