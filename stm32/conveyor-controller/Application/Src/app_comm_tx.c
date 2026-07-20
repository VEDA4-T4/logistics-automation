/*
 * ============================================================================
 * CommTxTask 구현
 * ============================================================================
 *
 * STM32 UART 송신의 논리적 단일 소유자. urgent/normal 2단 TX Queue에서
 * 송신 요청을 꺼내 대상 채널(USART1/USART6)로 라우팅한다.
 *
 * 긴급 우선:
 *   urgent 큐가 비어 있을 때만 normal 큐를 처리한다. QueueSet으로 두 큐를
 *   동시에 대기하므로 폴링 지연이 없다.
 *
 * 채널 독립성:
 *   채널마다 인코딩된 프레임 링버퍼를 두고, DMA 송신 완료 인터럽트가
 *   링버퍼의 다음 프레임을 이어서 송신한다. CommTxTask는 DMA 완료를
 *   기다리지 않으므로 한 채널의 송신 중 상태가 다른 채널을 블로킹하지
 *   않는다. 링버퍼 마지막 한 칸은 urgent 전용으로 예약해서 normal 폭주
 *   중에도 urgent 프레임이 드랍되지 않게 한다.
 *
 * timeout·재시도 (채널별 분리):
 *   DMA 송신이 COMM_TX_TX_TIMEOUT_MS 안에 끝나지 않으면 전송을 중단하고
 *   같은 프레임을 UART_MAX_RETRY_COUNT까지 재시도한 뒤 버린다. 한 채널의
 *   timeout이 다른 채널에 영향을 주지 않는다.
 *
 * heartbeat:
 *   COMM_TX_HEARTBEAT_PERIOD_MS 주기로 장치 상태·현재 오류·uptime·공정별
 *   센서 상태를 담은 EVENT(APP_EVENT_HEARTBEAT) 프레임을 두 채널에 송신한다.
 *
 * freertos.c의 __weak StartCommTxTask를 이 파일의 구현이 덮는다.
 */

#include "app_comm_tx.h"

#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "logistics/contracts/uart_codec.h"
#include "queue.h"
#include "task.h"
#include "usart.h"

/* TX Queue 깊이 */
#define COMM_TX_NORMAL_QUEUE_DEPTH 8U
#define COMM_TX_URGENT_QUEUE_DEPTH 4U

/* 채널별 인코딩된 프레임 링버퍼 깊이 (마지막 1칸은 urgent 예약) */
#define COMM_TX_RING_DEPTH 8U

/* heartbeat 주기 */
#define COMM_TX_HEARTBEAT_PERIOD_MS 1000U

/*
 * DMA 송신 timeout.
 * 최대 프레임(135바이트)은 115200bps에서 약 12ms가 걸린다.
 */
#define COMM_TX_TX_TIMEOUT_MS 50U

/* HAL/DMA 시작 오류 후 같은 프레임을 다시 시도하기 전 최소 간격. */
#define COMM_TX_RETRY_INTERVAL_MS UART_RETRY_INTERVAL_MS

/* 큐가 비어 있어도 timeout·heartbeat 점검을 위해 최대 이만큼만 대기한다. */
#define COMM_TX_MAX_WAIT_MS 20U

/* 채널별 송신 상태 */
typedef struct {
    UART_HandleTypeDef* huart;
    uint8_t sequence;    /* 채널별 SEQUENCE 카운터 */
    uint8_t retry_count; /* 현재 프레임의 timeout 재시도 수 */
    uint32_t tx_start_tick;

    /* 인코딩된 프레임 링버퍼. ISR(DMA 완료)과 태스크가 함께 접근한다. */
    uint8_t frames[COMM_TX_RING_DEPTH][UART_MAX_FRAME_SIZE];
    uint16_t frame_lengths[COMM_TX_RING_DEPTH];
    volatile uint8_t head;  /* 태스크가 채우는 위치 */
    volatile uint8_t tail;  /* DMA가 송신 중/송신할 위치 */
    volatile uint8_t count; /* 링버퍼에 있는 프레임 수 */
    volatile uint8_t dma_busy;
    volatile uint8_t restart_pending;
} comm_tx_channel_state_t;

typedef enum {
    COMM_TX_ROUTE_OK = 0,
    COMM_TX_ROUTE_BUSY,
    COMM_TX_ROUTE_DROPPED
} comm_tx_route_result_t;

static QueueHandle_t comm_tx_normal_queue;
static QueueHandle_t comm_tx_urgent_queue;
static QueueSetHandle_t comm_tx_queue_set;
static uint32_t comm_tx_next_heartbeat;

static comm_tx_channel_state_t comm_tx_channels[COMM_TX_CH_COUNT];

/* heartbeat에 실리는 장치 상태 (setter로 갱신) */
static volatile uint8_t comm_tx_device_state = (uint8_t)UART_DEVICE_READY;
static volatile uint8_t comm_tx_error_code = (uint8_t)UART_ERROR_NONE;
static volatile uint8_t comm_tx_sensor_states[COMM_TX_CH_COUNT] = {
    (uint8_t)UART_SENSOR_CLEAR,
    (uint8_t)UART_SENSOR_CLEAR,
};

/* static을 붙이지 않는다 - 디버거 Live Expressions에서 관찰한다. */
comm_tx_stats_t comm_tx_stats;

/*
 * huart 포인터로 채널 상태를 찾는다. ISR에서 호출된다.
 */
static comm_tx_channel_state_t* comm_tx_channel_from_huart(const UART_HandleTypeDef* huart) {
    for (uint32_t i = 0U; i < COMM_TX_CH_COUNT; i++) {
        if (comm_tx_channels[i].huart == huart) {
            return &comm_tx_channels[i];
        }
    }

    return (comm_tx_channel_state_t*)0;
}

static uint32_t comm_tx_channel_index(const comm_tx_channel_state_t* channel) {
    return (uint32_t)(channel - comm_tx_channels);
}

/*
 * 링버퍼 tail의 프레임 DMA 송신을 시작하고 timeout 기준 시각을 기록한다.
 *
 * 호출 조건: dma_busy를 1로 만든 뒤 호출하거나, ISR에서 count > 0일 때
 * 호출한다.
 */
static uint8_t comm_tx_start_dma(comm_tx_channel_state_t* channel) {
    channel->tx_start_tick = HAL_GetTick();

    HAL_StatusTypeDef status =
        HAL_UART_Transmit_DMA(channel->huart, channel->frames[channel->tail], channel->frame_lengths[channel->tail]);

    if (status != HAL_OK) {
        comm_tx_stats.tx_error[comm_tx_channel_index(channel)]++;
        channel->dma_busy = 0U;
        channel->restart_pending = 1U;
        return 0U;
    }

    channel->restart_pending = 0U;
    return 1U;
}

/*
 * 메시지를 프레임으로 인코딩해서 채널 링버퍼에 넣고,
 * 채널이 쉬고 있으면 DMA 송신을 시작한다.
 *
 * urgent 메시지는 normal이 쓸 수 없는 마지막 예약 칸까지 사용할 수 있다.
 *
 * CommTxTask 컨텍스트에서만 호출한다.
 */
static comm_tx_route_result_t comm_tx_route(const comm_tx_msg_t* msg, uint8_t urgent) {
    if (msg->channel >= (uint8_t)COMM_TX_CH_COUNT) {
        comm_tx_stats.dropped_invalid++;
        return COMM_TX_ROUTE_DROPPED;
    }

    comm_tx_channel_state_t* channel = &comm_tx_channels[msg->channel];

    /*
     * 링버퍼 자리 확인. normal은 1칸을 남기고 멈추고(urgent 예약),
     * urgent는 끝까지 쓴다. head 슬롯은 ISR이 건드리지 않으므로
     * 자리 확인 후에는 잠금 없이 인코딩해도 안전하다.
     */
    uint8_t limit = (urgent != 0U) ? COMM_TX_RING_DEPTH : (COMM_TX_RING_DEPTH - 1U);

    if (channel->count >= limit) {
        return COMM_TX_ROUTE_BUSY;
    }

    uart_frame_t frame;
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = (msg->use_provided_sequence != 0U) ? msg->sequence : channel->sequence;
    frame.command = msg->command;
    frame.length = msg->length;

    if (msg->length > 0U) {
        memcpy(frame.payload, msg->payload, msg->length);
    }

    size_t encoded_length = 0U;
    uart_codec_result_t result =
        uart_encode_frame(&frame, channel->frames[channel->head], UART_MAX_FRAME_SIZE, &encoded_length);

    if (result != UART_CODEC_OK) {
        comm_tx_stats.dropped_encode_error++;
        return COMM_TX_ROUTE_DROPPED;
    }

    channel->frame_lengths[channel->head] = (uint16_t)encoded_length;

    if (msg->use_provided_sequence == 0U) {
        channel->sequence++;
    }

    /*
     * 링버퍼 갱신과 DMA 시작 판단은 송신 완료 ISR과 경합하므로
     * 임계 구역으로 보호한다.
     */
    uint8_t start_needed = 0U;

    taskENTER_CRITICAL();

    channel->head = (uint8_t)((channel->head + 1U) % COMM_TX_RING_DEPTH);
    channel->count++;

    if (channel->dma_busy == 0U) {
        channel->dma_busy = 1U;
        start_needed = 1U;
    }

    taskEXIT_CRITICAL();

    if (start_needed != 0U) {
        (void)comm_tx_start_dma(channel);
    }

    return COMM_TX_ROUTE_OK;
}

/*
 * heartbeat 프레임을 두 채널에 등록한다.
 *
 * Payload 구조는 app_comm_tx.h의 APP_HEARTBEAT_* 참조:
 * 장치 상태, 현재 오류, uptime(초), 공정별 센서 상태를 담는다.
 */
static void comm_tx_send_heartbeat(void) {
    uint32_t uptime_seconds = HAL_GetTick() / 1000U;

    comm_tx_msg_t msg;
    msg.sequence = 0U;
    msg.use_provided_sequence = 0U;
    msg.command = (uint8_t)UART_CMD_EVENT;
    msg.length = APP_HEARTBEAT_PAYLOAD_SIZE;
    msg.payload[UART_EVENT_ID_INDEX] = APP_EVENT_HEARTBEAT;
    msg.payload[APP_HEARTBEAT_STATE_INDEX] = comm_tx_device_state;
    msg.payload[APP_HEARTBEAT_ERROR_INDEX] = comm_tx_error_code;
    msg.payload[APP_HEARTBEAT_UPTIME_INDEX] = (uint8_t)(uptime_seconds & 0xFFU);
    msg.payload[APP_HEARTBEAT_UPTIME_INDEX + 1U] = (uint8_t)((uptime_seconds >> 8U) & 0xFFU);
    msg.payload[APP_HEARTBEAT_UPTIME_INDEX + 2U] = (uint8_t)((uptime_seconds >> 16U) & 0xFFU);
    msg.payload[APP_HEARTBEAT_UPTIME_INDEX + 3U] = (uint8_t)((uptime_seconds >> 24U) & 0xFFU);
    msg.payload[APP_HEARTBEAT_INPUT_SENSOR_INDEX] = comm_tx_sensor_states[COMM_TX_CH_INPUT];
    msg.payload[APP_HEARTBEAT_SORTING_SENSOR_INDEX] = comm_tx_sensor_states[COMM_TX_CH_SORTING];

    for (uint32_t i = 0U; i < COMM_TX_CH_COUNT; i++) {
        msg.channel = (uint8_t)i;

        if (comm_tx_route(&msg, 0U) == COMM_TX_ROUTE_OK) {
            comm_tx_stats.heartbeat_sent++;
        } else {
            comm_tx_stats.dropped_ring_full++;
        }
    }
}

/*
 * DMA 송신이 timeout된 채널을 복구한다.
 *
 * 같은 프레임을 UART_MAX_RETRY_COUNT까지 재시도한 뒤 버린다.
 * 채널별로 독립 판정하므로 한 채널의 timeout이 다른 채널을 막지 않는다.
 */
static void comm_tx_check_timeouts(void) {
    uint32_t now = HAL_GetTick();

    for (uint32_t i = 0U; i < COMM_TX_CH_COUNT; i++) {
        comm_tx_channel_state_t* channel = &comm_tx_channels[i];

        taskENTER_CRITICAL();

        if (channel->dma_busy != 0U && channel->count > 0U &&
            (now - channel->tx_start_tick) > COMM_TX_TX_TIMEOUT_MS) {
            comm_tx_stats.tx_timeout[i]++;

            (void)HAL_UART_AbortTransmit(channel->huart);
            channel->dma_busy = 0U;
            channel->restart_pending = 1U;
            channel->tx_start_tick = now;
        }

        taskEXIT_CRITICAL();
    }
}

static void comm_tx_service_restarts(void) {
    uint32_t now = HAL_GetTick();

    for (uint32_t i = 0U; i < COMM_TX_CH_COUNT; i++) {
        comm_tx_channel_state_t* channel = &comm_tx_channels[i];
        uint8_t start_needed = 0U;

        taskENTER_CRITICAL();

        if ((channel->dma_busy == 0U) && (channel->count > 0U)) {
            if ((channel->restart_pending != 0U) &&
                ((now - channel->tx_start_tick) < COMM_TX_RETRY_INTERVAL_MS)) {
                taskEXIT_CRITICAL();
                continue;
            }

            if (channel->restart_pending != 0U) {
                if (channel->retry_count >= UART_MAX_RETRY_COUNT) {
                    channel->tail = (uint8_t)((channel->tail + 1U) % COMM_TX_RING_DEPTH);
                    channel->count--;
                    channel->retry_count = 0U;
                    channel->restart_pending = 0U;
                    comm_tx_stats.dropped_retry_exhausted++;
                } else {
                    channel->retry_count++;
                    channel->restart_pending = 0U;
                    comm_tx_stats.tx_retry[i]++;
                }
            }

            if (channel->count > 0U) {
                channel->dma_busy = 1U;
                start_needed = 1U;
            }
        }

        taskEXIT_CRITICAL();

        if (start_needed != 0U) {
            (void)comm_tx_start_dma(channel);
        }
    }
}

static int32_t comm_tx_enqueue(QueueHandle_t queue, comm_tx_channel_t channel, uint8_t sequence,
                               uint8_t use_provided_sequence, uint8_t command, const uint8_t* payload, uint8_t length,
                               uint8_t urgent) {
    if (queue == (QueueHandle_t)0) {
        return -1;
    }

    if (channel >= COMM_TX_CH_COUNT || !UART_IS_VALID_PAYLOAD_LENGTH(length)) {
        comm_tx_stats.dropped_invalid++;
        return -2;
    }

    if (length > 0U && payload == (const uint8_t*)0) {
        comm_tx_stats.dropped_invalid++;
        return -2;
    }

    comm_tx_msg_t msg;
    msg.channel = (uint8_t)channel;
    msg.command = command;
    msg.length = length;
    msg.sequence = sequence;
    msg.use_provided_sequence = use_provided_sequence;

    if (length > 0U) {
        memcpy(msg.payload, payload, length);
    }

    /* 큐가 가득 차면 기다리지 않고 드랍한다 - 호출 태스크를 블로킹하지 않는다. */
    if (xQueueSend(queue, &msg, 0U) != pdTRUE) {
        if (urgent != 0U) {
            comm_tx_stats.dropped_urgent_queue_full++;
        } else {
            comm_tx_stats.dropped_queue_full++;
        }
        return -3;
    }

    if (urgent != 0U) {
        comm_tx_stats.enqueued_urgent++;
    } else {
        comm_tx_stats.enqueued++;
    }

    return 0;
}

int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(comm_tx_normal_queue, channel, 0U, 0U, command, payload, length, 0U);
}

int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command, const uint8_t* payload,
                                uint8_t length) {
    return comm_tx_enqueue(comm_tx_normal_queue, channel, sequence, 1U, command, payload, length, 0U);
}

int32_t CommTx_SendUrgent(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(comm_tx_urgent_queue, channel, 0U, 0U, command, payload, length, 1U);
}

int32_t CommTx_SendUrgentWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                      const uint8_t* payload, uint8_t length) {
    return comm_tx_enqueue(comm_tx_urgent_queue, channel, sequence, 1U, command, payload, length, 1U);
}

void CommTx_SetDeviceStatus(uint8_t device_state, uint8_t error_code) {
    comm_tx_device_state = device_state;
    comm_tx_error_code = error_code;
}

void CommTx_SetSensorState(comm_tx_channel_t process, uint8_t sensor_state) {
    if (process < COMM_TX_CH_COUNT) {
        comm_tx_sensor_states[process] = sensor_state;
    }
}

const comm_tx_stats_t* CommTx_GetStats(void) {
    return &comm_tx_stats;
}

static void comm_tx_cleanup_queues(uint8_t urgent_added, uint8_t normal_added) {
    if (comm_tx_queue_set != (QueueSetHandle_t)0) {
        if ((urgent_added != 0U) && (comm_tx_urgent_queue != (QueueHandle_t)0)) {
            (void)xQueueRemoveFromSet(comm_tx_urgent_queue, comm_tx_queue_set);
        }

        if ((normal_added != 0U) && (comm_tx_normal_queue != (QueueHandle_t)0)) {
            (void)xQueueRemoveFromSet(comm_tx_normal_queue, comm_tx_queue_set);
        }

        vQueueDelete(comm_tx_queue_set);
    }

    if (comm_tx_urgent_queue != (QueueHandle_t)0) {
        vQueueDelete(comm_tx_urgent_queue);
    }

    if (comm_tx_normal_queue != (QueueHandle_t)0) {
        vQueueDelete(comm_tx_normal_queue);
    }

    comm_tx_queue_set = (QueueSetHandle_t)0;
    comm_tx_urgent_queue = (QueueHandle_t)0;
    comm_tx_normal_queue = (QueueHandle_t)0;
}

int32_t CommTx_Init(void) {
    uint8_t urgent_added = 0U;

    if ((comm_tx_urgent_queue != (QueueHandle_t)0) && (comm_tx_normal_queue != (QueueHandle_t)0) &&
        (comm_tx_queue_set != (QueueSetHandle_t)0)) {
        return 0;
    }

    memset(comm_tx_channels, 0, sizeof(comm_tx_channels));

    comm_tx_channels[COMM_TX_CH_INPUT].huart = &huart1;
    comm_tx_channels[COMM_TX_CH_SORTING].huart = &huart6;

    comm_tx_urgent_queue = xQueueCreate(COMM_TX_URGENT_QUEUE_DEPTH, sizeof(comm_tx_msg_t));
    comm_tx_normal_queue = xQueueCreate(COMM_TX_NORMAL_QUEUE_DEPTH, sizeof(comm_tx_msg_t));

    if ((comm_tx_urgent_queue == (QueueHandle_t)0) || (comm_tx_normal_queue == (QueueHandle_t)0)) {
        comm_tx_stats.init_failures++;
        comm_tx_cleanup_queues(0U, 0U);
        return -1;
    }

    comm_tx_queue_set = xQueueCreateSet(COMM_TX_URGENT_QUEUE_DEPTH + COMM_TX_NORMAL_QUEUE_DEPTH);

    if (comm_tx_queue_set == (QueueSetHandle_t)0) {
        comm_tx_stats.init_failures++;
        comm_tx_cleanup_queues(0U, 0U);
        return -1;
    }

    if (xQueueAddToSet(comm_tx_urgent_queue, comm_tx_queue_set) != pdPASS) {
        comm_tx_stats.init_failures++;
        comm_tx_cleanup_queues(0U, 0U);
        return -2;
    }

    urgent_added = 1U;

    if (xQueueAddToSet(comm_tx_normal_queue, comm_tx_queue_set) != pdPASS) {
        comm_tx_stats.init_failures++;
        comm_tx_cleanup_queues(urgent_added, 0U);
        return -2;
    }

    comm_tx_next_heartbeat = osKernelGetTickCount() + COMM_TX_HEARTBEAT_PERIOD_MS;
    return 0;
}

static uint8_t comm_tx_drain_queues(void) {
    comm_tx_msg_t msg;
    comm_tx_route_result_t result;

    for (;;) {
        if (xQueuePeek(comm_tx_urgent_queue, &msg, 0U) == pdTRUE) {
            result = comm_tx_route(&msg, 1U);

            if (result == COMM_TX_ROUTE_BUSY) {
                comm_tx_stats.ring_full_waits++;
                return 1U;
            }

            (void)xQueueReceive(comm_tx_urgent_queue, &msg, 0U);
            continue;
        }

        if (xQueuePeek(comm_tx_normal_queue, &msg, 0U) == pdTRUE) {
            result = comm_tx_route(&msg, 0U);

            if (result == COMM_TX_ROUTE_BUSY) {
                comm_tx_stats.ring_full_waits++;
                return 1U;
            }

            (void)xQueueReceive(comm_tx_normal_queue, &msg, 0U);
            continue;
        }

        return 0U;
    }
}

void CommTx_ProcessOnce(void) {
    uint32_t now;
    uint32_t wait = COMM_TX_MAX_WAIT_MS;
    uint8_t ring_blocked;

    if (comm_tx_queue_set == (QueueSetHandle_t)0) {
        osDelay(COMM_TX_RETRY_INTERVAL_MS);
        return;
    }

    now = osKernelGetTickCount();

    int32_t remaining = (int32_t)(comm_tx_next_heartbeat - now);
    if (remaining <= 0) {
        wait = 0U;
    } else if ((uint32_t)remaining < wait) {
        wait = (uint32_t)remaining;
    }

    (void)xQueueSelectFromSet(comm_tx_queue_set, wait);

    comm_tx_check_timeouts();
    comm_tx_service_restarts();
    ring_blocked = comm_tx_drain_queues();

    now = osKernelGetTickCount();

    if ((int32_t)(now - comm_tx_next_heartbeat) >= 0) {
        comm_tx_send_heartbeat();
        comm_tx_next_heartbeat = now + COMM_TX_HEARTBEAT_PERIOD_MS;
    }

    if (ring_blocked != 0U) {
        osDelay(1U);
    }
}

/*
 * freertos.c의 __weak StartCommTxTask를 덮는 실제 구현.
 */
void StartCommTxTask(void* argument) {
    (void)argument;

    while (CommTx_Init() != 0) {
        osDelay(COMM_TX_RETRY_INTERVAL_MS);
    }

    for (;;) {
        CommTx_ProcessOnce();
    }
}

/*
 * DMA 송신 완료 콜백. DMA2 Stream6/7 인터럽트(우선순위 6)에서 호출된다.
 *
 * 완료된 프레임을 링버퍼에서 제거하고, 남은 프레임이 있으면 이어서
 * 송신한다. FreeRTOS API는 사용하지 않는다.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    comm_tx_channel_state_t* channel = comm_tx_channel_from_huart(huart);

    if ((channel == (comm_tx_channel_state_t*)0) || (channel->dma_busy == 0U) || (channel->count == 0U)) {
        return;
    }

    channel->tail = (uint8_t)((channel->tail + 1U) % COMM_TX_RING_DEPTH);
    channel->count--;
    channel->retry_count = 0U;
    channel->restart_pending = 0U;

    comm_tx_stats.sent[comm_tx_channel_index(channel)]++;

    if (channel->count > 0U) {
        (void)comm_tx_start_dma(channel);
    } else {
        channel->dma_busy = 0U;
    }
}

/*
 * UART 오류 콜백.
 *
 * 송신 오류 시 현재 프레임을 유지하고 task context에서 제한 횟수만큼
 * 재시도한다.
 *
 * uart_rx.c의 단일 HAL_UART_ErrorCallback이 RX 오류를 기록한 뒤 이
 * 함수를 호출한다. RX noise/overrun 오류로 정상 TX 프레임을 버리지 않도록
 * 실제 TX DMA 오류일 때만 송신 링을 복구한다.
 */
void CommTx_HandleUartError(UART_HandleTypeDef* huart) {
    comm_tx_channel_state_t* channel = comm_tx_channel_from_huart(huart);

    if ((channel == (comm_tx_channel_state_t*)0) || ((huart->ErrorCode & HAL_UART_ERROR_DMA) == 0U) ||
        (huart->hdmatx == (DMA_HandleTypeDef*)0) || (huart->hdmatx->ErrorCode == HAL_DMA_ERROR_NONE)) {
        return;
    }

    comm_tx_stats.tx_error[comm_tx_channel_index(channel)]++;

    if (channel->dma_busy != 0U && channel->count > 0U) {
        channel->dma_busy = 0U;
        channel->restart_pending = 1U;
        channel->tx_start_tick = HAL_GetTick();
    }
}
