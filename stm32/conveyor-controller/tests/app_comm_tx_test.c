#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "app_comm_tx.h"
#include "logistics/contracts/uart_codec.h"
#include "queue.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

#define FAKE_OBJECT_QUEUE 0x51554555UL
#define FAKE_OBJECT_SET 0x53455420UL
#define FAKE_MAX_SET_MEMBERS 4U
#define FAKE_MAX_TX_CAPTURES 128U

typedef struct {
    uint32_t type;
    UBaseType_t capacity;
    UBaseType_t itemSize;
    UBaseType_t head;
    UBaseType_t tail;
    UBaseType_t count;
    uint8_t* storage;
} fake_queue_t;

typedef struct {
    uint32_t type;
    UBaseType_t memberCount;
    QueueHandle_t members[FAKE_MAX_SET_MEMBERS];
} fake_queue_set_t;

typedef struct {
    UART_HandleTypeDef* huart;
    uint16_t length;
    uint8_t data[UART_MAX_FRAME_SIZE];
} fake_tx_capture_t;

static uint32_t fakeTick;
static uint32_t fakeCreateCalls;
static uint32_t fakeFailCreateCall;
static uint32_t fakeFailAddCall;
static uint32_t fakeAddCalls;
static uint32_t fakeCriticalDepth;
static HAL_StatusTypeDef fakeNextTransmitStatus = HAL_OK;
static uint32_t fakeTransmitCalls;
static uint32_t fakeAbortCalls;
static fake_tx_capture_t fakeCaptures[FAKE_MAX_TX_CAPTURES];
static uint32_t fakeCaptureCount;

static DMA_HandleTypeDef hdmaUart1Tx;
static DMA_HandleTypeDef hdmaUart6Tx;
UART_HandleTypeDef huart1 = { .ErrorCode = HAL_UART_ERROR_NONE, .hdmatx = &hdmaUart1Tx };
UART_HandleTypeDef huart6 = { .ErrorCode = HAL_UART_ERROR_NONE, .hdmatx = &hdmaUart6Tx };

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart);

static fake_queue_t* fake_queue(QueueHandle_t handle) {
    fake_queue_t* queue = (fake_queue_t*)handle;
    assert(queue != NULL);
    assert(queue->type == FAKE_OBJECT_QUEUE);
    return queue;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    fakeCreateCalls++;
    if (fakeFailCreateCall != 0U && fakeCreateCalls == fakeFailCreateCall) {
        return NULL;
    }

    fake_queue_t* queue = (fake_queue_t*)calloc(1U, sizeof(*queue));
    assert(queue != NULL);
    queue->storage = (uint8_t*)calloc(length, item_size);
    assert(queue->storage != NULL);
    queue->type = FAKE_OBJECT_QUEUE;
    queue->capacity = length;
    queue->itemSize = item_size;
    return queue;
}

QueueSetHandle_t xQueueCreateSet(UBaseType_t event_queue_length) {
    (void)event_queue_length;
    fakeCreateCalls++;
    if (fakeFailCreateCall != 0U && fakeCreateCalls == fakeFailCreateCall) {
        return NULL;
    }

    fake_queue_set_t* queueSet = (fake_queue_set_t*)calloc(1U, sizeof(*queueSet));
    assert(queueSet != NULL);
    queueSet->type = FAKE_OBJECT_SET;
    return queueSet;
}

BaseType_t xQueueAddToSet(QueueSetMemberHandle_t member, QueueSetHandle_t queue_set) {
    fake_queue_set_t* queueSet = (fake_queue_set_t*)queue_set;
    assert(queueSet != NULL);
    assert(queueSet->type == FAKE_OBJECT_SET);
    fakeAddCalls++;

    if ((fakeFailAddCall != 0U && fakeAddCalls == fakeFailAddCall) ||
        queueSet->memberCount >= FAKE_MAX_SET_MEMBERS) {
        return pdFAIL;
    }

    queueSet->members[queueSet->memberCount] = member;
    queueSet->memberCount++;
    return pdPASS;
}

BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t member, QueueSetHandle_t queue_set) {
    fake_queue_set_t* queueSet = (fake_queue_set_t*)queue_set;
    assert(queueSet != NULL);
    assert(queueSet->type == FAKE_OBJECT_SET);

    for (UBaseType_t i = 0U; i < queueSet->memberCount; i++) {
        if (queueSet->members[i] == member) {
            queueSet->members[i] = queueSet->members[queueSet->memberCount - 1U];
            queueSet->memberCount--;
            return pdPASS;
        }
    }

    return pdFAIL;
}

QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t queue_set, TickType_t timeout) {
    fake_queue_set_t* queueSet = (fake_queue_set_t*)queue_set;
    assert(queueSet != NULL);
    assert(queueSet->type == FAKE_OBJECT_SET);

    for (UBaseType_t i = 0U; i < queueSet->memberCount; i++) {
        if (fake_queue(queueSet->members[i])->count > 0U) {
            return queueSet->members[i];
        }
    }

    fakeTick += timeout;
    return NULL;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void* item, TickType_t timeout) {
    fake_queue_t* queue = fake_queue(handle);
    (void)timeout;

    if (queue->count >= queue->capacity) {
        return pdFALSE;
    }

    memcpy(&queue->storage[queue->head * queue->itemSize], item, queue->itemSize);
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void* item, TickType_t timeout) {
    fake_queue_t* queue = fake_queue(handle);
    (void)timeout;

    if (queue->count == 0U) {
        return pdFALSE;
    }

    memcpy(item, &queue->storage[queue->tail * queue->itemSize], queue->itemSize);
    queue->tail = (queue->tail + 1U) % queue->capacity;
    queue->count--;
    return pdTRUE;
}

BaseType_t xQueuePeek(QueueHandle_t handle, void* item, TickType_t timeout) {
    fake_queue_t* queue = fake_queue(handle);
    (void)timeout;

    if (queue->count == 0U) {
        return pdFALSE;
    }

    memcpy(item, &queue->storage[queue->tail * queue->itemSize], queue->itemSize);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t handle) {
    uint32_t type;
    assert(handle != NULL);
    type = *(const uint32_t*)handle;

    if (type == FAKE_OBJECT_QUEUE) {
        fake_queue_t* queue = (fake_queue_t*)handle;
        free(queue->storage);
    } else {
        assert(type == FAKE_OBJECT_SET);
    }

    free(handle);
}

void test_task_enter_critical(void) {
    fakeCriticalDepth++;
}

void test_task_exit_critical(void) {
    assert(fakeCriticalDepth > 0U);
    fakeCriticalDepth--;
}

uint32_t HAL_GetTick(void) {
    return fakeTick;
}

uint32_t osKernelGetTickCount(void) {
    return fakeTick;
}

void osDelay(uint32_t ticks) {
    fakeTick += ticks;
}

HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* huart, const uint8_t* data, uint16_t length) {
    HAL_StatusTypeDef status = fakeNextTransmitStatus;
    fakeNextTransmitStatus = HAL_OK;
    fakeTransmitCalls++;

    if (status == HAL_OK) {
        assert(fakeCaptureCount < FAKE_MAX_TX_CAPTURES);
        assert(length <= UART_MAX_FRAME_SIZE);
        fakeCaptures[fakeCaptureCount].huart = huart;
        fakeCaptures[fakeCaptureCount].length = length;
        memcpy(fakeCaptures[fakeCaptureCount].data, data, length);
        fakeCaptureCount++;
    }

    return status;
}

HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* huart) {
    (void)huart;
    fakeAbortCalls++;
    return HAL_OK;
}

static uart_frame_t decode_capture(uint32_t index, UART_HandleTypeDef* expectedUart) {
    uart_frame_t frame;
    assert(index < fakeCaptureCount);
    assert(fakeCaptures[index].huart == expectedUart);
    assert(uart_decode_frame(fakeCaptures[index].data, fakeCaptures[index].length, &frame) == UART_CODEC_OK);
    return frame;
}

static void complete_channel(UART_HandleTypeDef* huart) {
    HAL_UART_TxCpltCallback(huart);
    assert(fakeCriticalDepth == 0U);
}

static void test_init_failure_is_recoverable(void) {
    const comm_tx_stats_t* stats;
    fakeFailCreateCall = 2U;
    assert(CommTx_Init() == -1);
    stats = CommTx_GetStats();
    assert(stats->init_failures == 1U);

    fakeFailCreateCall = 0U;
    assert(CommTx_Init() == 0);
    assert(CommTx_Init() == 0);
}

static void test_sequence_and_payload_are_encoded(void) {
    const uint8_t payload[] = { UART_STATUS_SUCCESS, UART_ERROR_NONE };
    uint32_t captureBefore = fakeCaptureCount;

    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 0x5AU, UART_CMD_OPERATION_RESULT, payload,
                                   (uint8_t)sizeof(payload)) == 0);
    CommTx_ProcessOnce();

    uart_frame_t frame = decode_capture(captureBefore, &huart1);
    assert(frame.sequence == 0x5AU);
    assert(frame.command == UART_CMD_OPERATION_RESULT);
    assert(frame.length == sizeof(payload));
    assert(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    complete_channel(&huart1);
}

static void test_automatic_sequence_increments(void) {
    uint32_t captureBefore = fakeCaptureCount;

    assert(CommTx_Send(COMM_TX_CH_SORTING, UART_CMD_PING, NULL, 0U) == 0);
    assert(CommTx_Send(COMM_TX_CH_SORTING, UART_CMD_GET_STATUS, NULL, 0U) == 0);
    CommTx_ProcessOnce();

    uart_frame_t first = decode_capture(captureBefore, &huart6);
    complete_channel(&huart6);
    uart_frame_t second = decode_capture(captureBefore + 1U, &huart6);
    assert(second.sequence == (uint8_t)(first.sequence + 1U));
    complete_channel(&huart6);
}

static void test_queue_full_is_reported_to_caller(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t dropsBefore = stats->dropped_queue_full;

    for (uint8_t i = 0U; i < 8U; i++) {
        assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, i, UART_CMD_PING, NULL, 0U) == 0);
    }
    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 8U, UART_CMD_PING, NULL, 0U) == -3);
    assert(stats->dropped_queue_full == dropsBefore + 1U);

    CommTx_ProcessOnce();
    for (uint8_t i = 0U; i < 7U; i++) {
        complete_channel(&huart1);
    }
    CommTx_ProcessOnce();
    complete_channel(&huart1);
}

static void test_ring_backpressure_preserves_accepted_message(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t waitsBefore = stats->ring_full_waits;
    uint32_t sentBefore = stats->sent[COMM_TX_CH_INPUT];
    uint32_t captureBefore = fakeCaptureCount;

    for (uint8_t i = 0U; i < 8U; i++) {
        uint8_t payload[] = { i };
        assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, (uint8_t)(0x20U + i), UART_CMD_EVENT, payload,
                                       (uint8_t)sizeof(payload)) == 0);
    }

    CommTx_ProcessOnce();
    assert(stats->ring_full_waits == waitsBefore + 1U);

    complete_channel(&huart1);
    CommTx_ProcessOnce();
    for (uint8_t i = 1U; i < 8U; i++) {
        complete_channel(&huart1);
    }

    assert(stats->sent[COMM_TX_CH_INPUT] == sentBefore + 8U);
    for (uint8_t i = 0U; i < 8U; i++) {
        uart_frame_t frame = decode_capture(captureBefore + i, &huart1);
        assert(frame.sequence == (uint8_t)(0x20U + i));
        assert(frame.payload[0] == i);
    }
}

static void test_immediate_dma_failure_retries_same_frame(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t retriesBefore = stats->tx_retry[COMM_TX_CH_INPUT];
    uint32_t errorsBefore = stats->tx_error[COMM_TX_CH_INPUT];
    uint32_t transmitBefore = fakeTransmitCalls;
    uint32_t captureBefore = fakeCaptureCount;
    const uint8_t payload[] = { 0xA5U };

    fakeNextTransmitStatus = HAL_BUSY;
    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 0xA1U, UART_CMD_EVENT, payload,
                                   (uint8_t)sizeof(payload)) == 0);
    CommTx_ProcessOnce();
    assert(fakeTransmitCalls == transmitBefore + 1U);
    assert(fakeCaptureCount == captureBefore);
    assert(stats->tx_error[COMM_TX_CH_INPUT] == errorsBefore + 1U);

    CommTx_ProcessOnce();
    assert(fakeTransmitCalls == transmitBefore + 2U);
    assert(stats->tx_retry[COMM_TX_CH_INPUT] == retriesBefore + 1U);
    uart_frame_t frame = decode_capture(captureBefore, &huart1);
    assert(frame.sequence == 0xA1U);
    assert(frame.payload[0] == 0xA5U);
    complete_channel(&huart1);
}

static void test_timeout_retries_then_advances_after_exhaustion(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t droppedBefore = stats->dropped_retry_exhausted;
    uint32_t timeoutBefore = stats->tx_timeout[COMM_TX_CH_INPUT];
    uint32_t captureBefore = fakeCaptureCount;
    const uint8_t firstPayload[] = { 0x11U };
    const uint8_t secondPayload[] = { 0x22U };

    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 0xB1U, UART_CMD_EVENT, firstPayload,
                                   (uint8_t)sizeof(firstPayload)) == 0);
    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 0xB2U, UART_CMD_EVENT, secondPayload,
                                   (uint8_t)sizeof(secondPayload)) == 0);
    CommTx_ProcessOnce();

    for (uint8_t attempt = 0U; attempt <= UART_MAX_RETRY_COUNT; attempt++) {
        osDelay(51U);
        CommTx_ProcessOnce();
        osDelay(UART_RETRY_INTERVAL_MS);
        CommTx_ProcessOnce();
    }

    assert(stats->tx_timeout[COMM_TX_CH_INPUT] == timeoutBefore + UART_MAX_RETRY_COUNT + 1U);
    assert(stats->dropped_retry_exhausted == droppedBefore + 1U);

    uart_frame_t next = decode_capture(fakeCaptureCount - 1U, &huart1);
    assert(next.sequence == 0xB2U);
    assert(next.payload[0] == 0x22U);
    assert(fakeCaptureCount == captureBefore + UART_MAX_RETRY_COUNT + 2U);
    complete_channel(&huart1);
}

static void test_uart_error_only_retries_real_tx_dma_error(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t errorsBefore = stats->tx_error[COMM_TX_CH_INPUT];
    uint32_t captureBefore = fakeCaptureCount;

    assert(CommTx_SendWithSequence(COMM_TX_CH_INPUT, 0xC1U, UART_CMD_PING, NULL, 0U) == 0);
    CommTx_ProcessOnce();

    huart1.ErrorCode = HAL_UART_ERROR_ORE;
    hdmaUart1Tx.ErrorCode = HAL_DMA_ERROR_NONE;
    CommTx_HandleUartError(&huart1);
    assert(stats->tx_error[COMM_TX_CH_INPUT] == errorsBefore);

    huart1.ErrorCode = HAL_UART_ERROR_DMA;
    hdmaUart1Tx.ErrorCode = HAL_DMA_ERROR_TE;
    CommTx_HandleUartError(&huart1);
    assert(stats->tx_error[COMM_TX_CH_INPUT] == errorsBefore + 1U);

    huart1.ErrorCode = HAL_UART_ERROR_NONE;
    hdmaUart1Tx.ErrorCode = HAL_DMA_ERROR_NONE;
    osDelay(UART_RETRY_INTERVAL_MS);
    CommTx_ProcessOnce();
    assert(fakeCaptureCount == captureBefore + 2U);
    assert(decode_capture(captureBefore + 1U, &huart1).sequence == 0xC1U);
    complete_channel(&huart1);
}

static void test_spurious_completion_is_ignored(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t sentBefore = stats->sent[COMM_TX_CH_INPUT];
    complete_channel(&huart1);
    assert(stats->sent[COMM_TX_CH_INPUT] == sentBefore);
}

static void test_heartbeat_routes_to_both_channels(void) {
    const comm_tx_stats_t* stats = CommTx_GetStats();
    uint32_t heartbeatBefore = stats->heartbeat_sent;
    uint32_t captureBefore = fakeCaptureCount;

    CommTx_SetDeviceStatus(UART_DEVICE_RUNNING, UART_ERROR_NONE);
    CommTx_SetSensorState(COMM_TX_CH_INPUT, UART_SENSOR_DETECTED);
    CommTx_SetSensorState(COMM_TX_CH_SORTING, UART_SENSOR_CLEAR);
    osDelay(1000U);
    CommTx_ProcessOnce();

    uart_frame_t inputFrame = decode_capture(captureBefore, &huart1);
    uart_frame_t sortingFrame = decode_capture(captureBefore + 1U, &huart6);
    assert(inputFrame.command == UART_CMD_EVENT);
    assert(sortingFrame.command == UART_CMD_EVENT);
    assert(inputFrame.payload[UART_EVENT_ID_INDEX] == APP_EVENT_HEARTBEAT);
    assert(inputFrame.payload[APP_HEARTBEAT_STATE_INDEX] == UART_DEVICE_RUNNING);
    assert(inputFrame.payload[APP_HEARTBEAT_INPUT_SENSOR_INDEX] == UART_SENSOR_DETECTED);
    assert(sortingFrame.payload[APP_HEARTBEAT_SORTING_SENSOR_INDEX] == UART_SENSOR_CLEAR);
    assert(stats->heartbeat_sent == heartbeatBefore + 2U);
    complete_channel(&huart1);
    complete_channel(&huart6);
}

int main(void) {
    test_init_failure_is_recoverable();
    test_sequence_and_payload_are_encoded();
    test_automatic_sequence_increments();
    test_queue_full_is_reported_to_caller();
    test_ring_backpressure_preserves_accepted_message();
    test_immediate_dma_failure_retries_same_frame();
    test_timeout_retries_then_advances_after_exhaustion();
    test_uart_error_only_retries_real_tx_dma_error();
    test_spurious_completion_is_ignored();
    test_heartbeat_routes_to_both_channels();
    assert(fakeAbortCalls >= UART_MAX_RETRY_COUNT + 1U);
    assert(fakeCriticalDepth == 0U);
    return 0;
}
