/*
 * ============================================================================
 * CommTxTask 1단계 dummy 송신기
 * ============================================================================
 *
 * 시나리오 1 - 주기 송신·라우팅:
 *   200ms마다 SENSOR_STATUS 프레임을 투입/분류 채널에 번갈아 등록한다.
 *   수신 측 기대: 투입 Pi는 sensor_id 0만, 분류 Pi는 sensor_id 1만 수신.
 *
 * 시나리오 2 - 큐 포화·긴급 우선순위 (BURST_INTERVAL 회마다):
 *   스케줄러를 잠근 채 normal EVENT를 큐 깊이(8)보다 많은 10개 밀어 넣어
 *   포화 드랍을 유발하고, 마지막에 urgent EVENT를 넣는다.
 *   - 큐 포화 정책: 초과분은 드랍되고 comm_tx_stats.dropped_queue_full에
 *     집계된다 (디버거로 확인).
 *   - 긴급 우선순위: urgent가 나중에 등록됐어도 같은 burst의 normal보다
 *     먼저 송신되어야 한다 (수신 검증 프로그램이 도착 순서로 판정).
 *
 * LD2(보드 LED)는 주기 송신마다 토글되어 동작 확인용으로 사용한다.
 */

#include "app_comm_tx_dummy.h"

#include "app_comm_tx.h"
#include "cmsis_os.h"
#include "main.h"

/* 주기 송신 간격 */
#define COMM_TX_DUMMY_PERIOD_MS 200U

/* 포화·우선순위 burst 시험 간격 (주기 송신 횟수 기준, 30회 = 약 6초) */
#define COMM_TX_DUMMY_BURST_INTERVAL 30U

/* burst당 normal 메시지 수. 큐 깊이(8)보다 크게 잡아 포화를 유발한다. */
#define COMM_TX_DUMMY_BURST_COUNT 10U

/*
 * 큐 포화·긴급 우선순위 burst 시험.
 *
 * 스케줄러를 잠가서 CommTxTask(AboveNormal)가 중간에 큐를 비우지 못하게
 * 한 상태로 몰아넣는다. xQueueSend(timeout 0)는 블로킹하지 않으므로
 * 스케줄러 잠금 상태에서 호출해도 안전하다.
 */
static void comm_tx_dummy_burst(comm_tx_channel_t channel, uint8_t burst_id) {
    osKernelLock();

    for (uint8_t index = 0U; index < COMM_TX_DUMMY_BURST_COUNT; index++) {
        uint8_t payload[3];
        payload[0] = APP_EVENT_DUMMY_BURST;
        payload[1] = burst_id;
        payload[2] = index;

        (void)CommTx_Send(channel, (uint8_t)UART_CMD_EVENT, payload, sizeof(payload));
    }

    {
        uint8_t payload[2];
        payload[0] = APP_EVENT_DUMMY_URGENT;
        payload[1] = burst_id;

        (void)CommTx_SendUrgent(channel, (uint8_t)UART_CMD_EVENT, payload, sizeof(payload));
    }

    osKernelUnlock();
}

void CommTxDummy_Run(void) {
    /* CommTxTask의 큐 생성을 기다린다. */
    osDelay(1000U);

    uint32_t iteration = 0U;
    uint8_t burst_id = 0U;

    for (;;) {
        comm_tx_channel_t channel = ((iteration & 1U) == 0U) ? COMM_TX_CH_INPUT : COMM_TX_CH_SORTING;

        /* 100~399mm를 반복하는 가짜 거리 값 */
        uint16_t distance = (uint16_t)(100U + (iteration % 300U));

        uint8_t payload[UART_SENSOR_STATUS_PAYLOAD_SIZE];
        payload[UART_SENSOR_ID_INDEX] = (uint8_t)channel; /* 투입=0, 분류=1 */
        payload[UART_SENSOR_STATE_INDEX] = (uint8_t)UART_SENSOR_DETECTED;
        payload[UART_SENSOR_DISTANCE_LOW_INDEX] = (uint8_t)(distance & 0xFFU);
        payload[UART_SENSOR_DISTANCE_HIGH_INDEX] = (uint8_t)((distance >> 8U) & 0xFFU);

        (void)CommTx_Send(channel, (uint8_t)UART_CMD_SENSOR_STATUS, payload, sizeof(payload));

        /* heartbeat에 실리는 센서 상태도 함께 갱신한다. */
        CommTx_SetSensorState(channel, (uint8_t)UART_SENSOR_DETECTED);

        /* 주기적으로 큐 포화·긴급 우선순위 burst 시험 */
        if (iteration != 0U && (iteration % COMM_TX_DUMMY_BURST_INTERVAL) == 0U) {
            comm_tx_dummy_burst(channel, burst_id);
            burst_id++;
        }

        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);

        iteration++;

        osDelay(COMM_TX_DUMMY_PERIOD_MS);
    }
}
