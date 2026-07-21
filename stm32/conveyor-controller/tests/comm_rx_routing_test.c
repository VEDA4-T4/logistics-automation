#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "comm_rx_task.h"
#include "input_control_task.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "uart_rx.h"

#define TEST_INPUT_SAFETY_EPOCH 0x12345678U
#define TEST_SORTING_SAFETY_EPOCH 0x87654321U

static uint8_t inputQueueToken;
static uint8_t sortingQueueToken;
static uint8_t safetyQueueToken;

static control_command_t capturedInputMessage;
static control_command_t capturedSortingMessage;
static control_command_t capturedSafetyMessage;
static uint32_t inputQueuePuts;
static uint32_t sortingQueuePuts;
static uint32_t safetyQueuePuts;
static uint8_t failInputQueuePut;
static uint8_t failTxSend;
static comm_tx_channel_t capturedTxChannel;
static uart_frame_t capturedTxFrame;
static uint32_t txSends;

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle = &inputQueueToken;
osMessageQueueId_t sortingControlQueueHandle = &sortingQueueToken;
osMessageQueueId_t safetyCommandQueueHandle = &safetyQueueToken;

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    const control_command_t* message;

    (void)msg_prio;
    (void)timeout;

    if (msg_ptr == NULL) {
        return osErrorParameter;
    }

    message = (const control_command_t*)msg_ptr;

    if (mq_id == inputControlQueueHandle) {
        if (failInputQueuePut != 0U) {
            return osErrorResource;
        }

        capturedInputMessage = *message;
        inputQueuePuts++;
        return osOK;
    }

    if (mq_id == sortingControlQueueHandle) {
        capturedSortingMessage = *message;
        sortingQueuePuts++;
        return osOK;
    }

    if (mq_id == safetyCommandQueueHandle) {
        capturedSafetyMessage = *message;
        safetyQueuePuts++;
        return osOK;
    }

    return osErrorParameter;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void* msg_ptr, uint8_t* msg_prio, uint32_t timeout) {
    (void)mq_id;
    (void)msg_ptr;
    (void)msg_prio;
    (void)timeout;
    return osErrorResource;
}

osStatus_t osDelay(uint32_t ticks) {
    (void)ticks;
    return osOK;
}

uint32_t HAL_GetTick(void) {
    return 0U;
}

HAL_StatusTypeDef uart_rx_start(void) {
    return HAL_OK;
}

HAL_StatusTypeDef uart_rx_restart(app_uart_channel_t channel) {
    (void)channel;
    return HAL_OK;
}

uint32_t uart_rx_take_error(app_uart_channel_t channel) {
    (void)channel;
    return 0U;
}

uint32_t uart_rx_get_dropped_chunk_count(void) {
    return 0U;
}

uint32_t input_control_task_capture_command_epoch(void) {
    return TEST_INPUT_SAFETY_EPOCH;
}

uint32_t sorting_control_task_capture_command_epoch(void) {
    return TEST_SORTING_SAFETY_EPOCH;
}

int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                const uint8_t* payload, uint8_t length) {
    if (failTxSend != 0U) {
        return -3;
    }

    assert(channel < COMM_TX_CH_COUNT);
    assert(length <= UART_MAX_PAYLOAD_SIZE);
    assert((length == 0U) || (payload != NULL));

    memset(&capturedTxFrame, 0, sizeof(capturedTxFrame));
    capturedTxChannel = channel;
    capturedTxFrame.version = UART_PROTOCOL_VERSION;
    capturedTxFrame.sequence = sequence;
    capturedTxFrame.command = command;
    capturedTxFrame.length = length;

    if (length != 0U) {
        memcpy(capturedTxFrame.payload, payload, length);
    }

    txSends++;
    return 0;
}

static uart_frame_t frame_with_command(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    uart_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = command;
    frame.length = length;

    if ((payload != NULL) && (length != 0U)) {
        memcpy(frame.payload, payload, length);
    }

    return frame;
}

static void test_input_command_is_tagged_with_current_safety_epoch(void) {
    uart_frame_t frame;
    const uint8_t speed[] = { 45U };

    frame = frame_with_command(0x31U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);

    assert(inputQueuePuts == 1U);
    assert(sortingQueuePuts == 0U);
    assert(safetyQueuePuts == 0U);
    assert(capturedInputMessage.kind == APP_CONTROL_MESSAGE_UART_COMMAND);
    assert(capturedInputMessage.safetyEpoch == TEST_INPUT_SAFETY_EPOCH);
    assert(capturedInputMessage.source == APP_UART_CHANNEL_1);
    assert(memcmp(&capturedInputMessage.frame, &frame, sizeof(frame)) == 0);
}

static void test_sorting_and_safety_commands_keep_neutral_epoch(void) {
    uart_frame_t frame;

    frame = frame_with_command(0x32U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_6, &frame);

    assert(sortingQueuePuts == 1U);
    assert(capturedSortingMessage.kind == APP_CONTROL_MESSAGE_UART_COMMAND);
    assert(capturedSortingMessage.safetyEpoch == TEST_SORTING_SAFETY_EPOCH);
    assert(capturedSortingMessage.source == APP_UART_CHANNEL_6);
    assert(memcmp(&capturedSortingMessage.frame, &frame, sizeof(frame)) == 0);

    frame = frame_with_command(0x33U, UART_CMD_EMERGENCY_STOP, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);

    assert(safetyQueuePuts == 1U);
    assert(capturedSafetyMessage.kind == APP_CONTROL_MESSAGE_UART_COMMAND);
    assert(capturedSafetyMessage.safetyEpoch == 0U);
    assert(capturedSafetyMessage.source == APP_UART_CHANNEL_1);
    assert(memcmp(&capturedSafetyMessage.frame, &frame, sizeof(frame)) == 0);
}

static void test_return_home_command_is_forwarded_to_sorting_queue(void) {
    const uint8_t payload[] = { 0x34U, 0x12U };
    uart_frame_t frame;
    uint32_t sortingPutsBefore;

    sortingPutsBefore = sortingQueuePuts;
    frame = frame_with_command(0x34U, UART_CMD_SORTING_RETURN_HOME, payload, sizeof(payload));
    comm_rx_process_frame(APP_UART_CHANNEL_6, &frame);

    assert(sortingQueuePuts == (sortingPutsBefore + 1U));
    assert(capturedSortingMessage.kind == APP_CONTROL_MESSAGE_UART_COMMAND);
    assert(capturedSortingMessage.safetyEpoch == TEST_SORTING_SAFETY_EPOCH);
    assert(capturedSortingMessage.source == APP_UART_CHANNEL_6);
    assert(capturedSortingMessage.frame.command == UART_CMD_SORTING_RETURN_HOME);
    assert(capturedSortingMessage.frame.length == UART_SORTING_RETURN_HOME_PAYLOAD_SIZE);
    assert(uart_sorting_return_home_cycle_id(capturedSortingMessage.frame.payload) == 0x1234U);
}

static void test_invalid_return_home_payload_is_rejected(void) {
    const uint8_t shortPayload[] = { 0x34U };
    uart_frame_t frame;
    uint32_t sortingPutsBefore;
    uint32_t txSendsBefore;

    sortingPutsBefore = sortingQueuePuts;
    txSendsBefore = txSends;
    frame = frame_with_command(0x35U, UART_CMD_SORTING_RETURN_HOME, shortPayload, sizeof(shortPayload));
    comm_rx_process_frame(APP_UART_CHANNEL_6, &frame);

    assert(sortingQueuePuts == sortingPutsBefore);
    assert(txSends == (txSendsBefore + 1U));
    assert(capturedTxChannel == COMM_TX_CH_SORTING);
    assert(capturedTxFrame.sequence == 0x35U);
    assert(capturedTxFrame.command == UART_CMD_RESPONSE);
    assert(capturedTxFrame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_NACK);
    assert(capturedTxFrame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_SORTING_RETURN_HOME);
    assert(capturedTxFrame.payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_INVALID_PAYLOAD);
}

static void test_invalid_input_payload_is_not_forwarded(void) {
    uart_frame_t frame;
    uint32_t inputPutsBefore;
    uint32_t safetyPutsBefore;
    uint32_t sortingPutsBefore;
    uint32_t txSendsBefore;

    inputPutsBefore = inputQueuePuts;
    sortingPutsBefore = sortingQueuePuts;
    safetyPutsBefore = safetyQueuePuts;
    txSendsBefore = txSends;
    frame = frame_with_command(0x34U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);

    assert(inputQueuePuts == inputPutsBefore);
    assert(sortingQueuePuts == sortingPutsBefore);
    assert(safetyQueuePuts == safetyPutsBefore);
    assert(txSends == (txSendsBefore + 1U));
    assert(capturedTxChannel == COMM_TX_CH_INPUT);
    assert(capturedTxFrame.sequence == 0x34U);
    assert(capturedTxFrame.command == UART_CMD_RESPONSE);
    assert(capturedTxFrame.length == UART_RESPONSE_HEADER_SIZE);
    assert(capturedTxFrame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_NACK);
    assert(capturedTxFrame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_INPUT_CONVEYOR_SET_SPEED);
    assert(capturedTxFrame.payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_INVALID_PAYLOAD);
}

static void test_unsupported_input_command_returns_nack(void) {
    uart_frame_t frame;
    uint32_t txSendsBefore;

    txSendsBefore = txSends;
    frame = frame_with_command(0x35U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);

    assert(txSends == (txSendsBefore + 1U));
    assert(capturedTxChannel == COMM_TX_CH_INPUT);
    assert(capturedTxFrame.sequence == 0x35U);
    assert(capturedTxFrame.command == UART_CMD_RESPONSE);
    assert(capturedTxFrame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_NACK);
    assert(capturedTxFrame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_SORTING_CONVEYOR_START);
    assert(capturedTxFrame.payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_UNSUPPORTED_COMMAND);
}

static void test_full_input_queue_returns_busy(void) {
    uart_frame_t frame;
    uint32_t inputPutsBefore;
    uint32_t txSendsBefore;

    inputPutsBefore = inputQueuePuts;
    txSendsBefore = txSends;
    failInputQueuePut = 1U;
    frame = frame_with_command(0x36U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);
    failInputQueuePut = 0U;

    assert(inputQueuePuts == inputPutsBefore);
    assert(txSends == (txSendsBefore + 1U));
    assert(capturedTxChannel == COMM_TX_CH_INPUT);
    assert(capturedTxFrame.sequence == 0x36U);
    assert(capturedTxFrame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_BUSY);
    assert(capturedTxFrame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_INPUT_CONVEYOR_START);
    assert(capturedTxFrame.payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_BUSY);
}

static void test_failed_rejection_send_is_counted(void) {
    comm_rx_stats_t before;
    comm_rx_stats_t after;
    uart_frame_t frame;

    comm_rx_get_stats(&before);
    failTxSend = 1U;
    frame = frame_with_command(0x37U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, NULL, 0U);
    comm_rx_process_frame(APP_UART_CHANNEL_1, &frame);
    failTxSend = 0U;
    comm_rx_get_stats(&after);

    assert(after.responseDrops == (before.responseDrops + 1U));
}

int main(void) {
    test_input_command_is_tagged_with_current_safety_epoch();
    test_sorting_and_safety_commands_keep_neutral_epoch();
    test_return_home_command_is_forwarded_to_sorting_queue();
    test_invalid_return_home_payload_is_rejected();
    test_invalid_input_payload_is_not_forwarded();
    test_unsupported_input_command_returns_nack();
    test_full_input_queue_returns_busy();
    test_failed_rejection_send_is_counted();
    return 0;
}
