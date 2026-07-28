#include "sensor_task.h"

#include "app_comm_tx.h"
#include "cmsis_os2.h"
#include "hc_sr04.h"
#include "health_task.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "main.h"
#include "sensor_filter.h"
#include "tim.h"

/*
 * ============================================================================
 * SensorTask
 * ============================================================================
 *
 * 4개의 HC-SR04를 US1->US2->US3->US4 순서로 순차 폴링한다(동시 트리거 금지).
 * US1은 투입 컨베이어 로봇팔 앞 상자 검출(투입 Pi), US2/US3/US4는 분류 구역
 * A/B/C 도착 확인(분류 Pi)이다.
 *
 * 센서별 원시 판정(sensor_filter)을 UART_CMD_SENSOR_STATUS로 CommTxTask에
 * 전달한다. ENTERED/EXITED 같은 업무판단 이벤트나 공용 제어 큐는 팀 인터페이스
 * 확정 전까지 다루지 않는다 - 이 태스크는 raw 상태 보고까지만 담당한다.
 */

#define SENSOR_COUNT 4U
#define SENSOR_ECHO_WAIT_MS 45U /* echo timeout(~38ms) + 여유 */

/*
 * 측정 1회의 최소 소요 시간. 다음 센서를 트리거하기까지의 간격이기도 하다.
 *
 * HC-SR04 데이터시트가 권장하는 최소 측정 주기가 60ms다 - 이전 발사의 초음파가
 * 충분히 감쇠하기 전에 다시 발사하면 그 잔향이 다음 수신창에 잡힌다. 개별 센서
 * 재방문 주기는 4채널 순차라 이 값의 4배(240ms)로 넉넉하지만, *인접* 채널은 딱
 * 이 간격만큼만 떨어져 있다. 분류 US2/US3/US4는 같은 구역을 향하고 있어서 서로의
 * 잔향을 주울 수 있으므로 인접 간격 자체가 데이터시트 기준을 만족해야 한다
 * (50ms였을 때 실기기 캡처에 554cm/382cm 같은 비현실적 값이 섞여 나왔다).
 */
#define SENSOR_MIN_CYCLE_MS 60U

typedef struct {
    hc_sr04_sensor_t driver;
    sensor_filter_t filter;
    comm_tx_channel_t txChannel;
    uint8_t sensorId;
    uint32_t lastPollTick; /* HealthTask의 채널 서비스 지연 판정용, 폴링 시각 */
} sensor_channel_t;

static sensor_channel_t sensorChannels[SENSOR_COUNT];

static void sensor_task_init_channels(void) {
    uint8_t i;

    hc_sr04_init(&sensorChannels[0].driver, &htim2, TIM_CHANNEL_1, US1_TRIG_GPIO_Port, US1_TRIG_Pin);
    sensorChannels[0].txChannel = COMM_TX_CH_INPUT;
    sensorChannels[0].sensorId = UART_INPUT_SENSOR_ID_1;

    hc_sr04_init(&sensorChannels[1].driver, &htim2, TIM_CHANNEL_2, US2_TRIG_GPIO_Port, US2_TRIG_Pin);
    sensorChannels[1].txChannel = COMM_TX_CH_SORTING;
    sensorChannels[1].sensorId = UART_SORTING_SENSOR_ID_1;

    hc_sr04_init(&sensorChannels[2].driver, &htim2, TIM_CHANNEL_3, US3_TRIG_GPIO_Port, US3_TRIG_Pin);
    sensorChannels[2].txChannel = COMM_TX_CH_SORTING;
    sensorChannels[2].sensorId = UART_SORTING_SENSOR_ID_2;

    hc_sr04_init(&sensorChannels[3].driver, &htim10, TIM_CHANNEL_1, US4_TRIG_GPIO_Port, US4_TRIG_Pin);
    sensorChannels[3].txChannel = COMM_TX_CH_SORTING;
    sensorChannels[3].sensorId = UART_SORTING_SENSOR_ID_3;

    for (i = 0U; i < SENSOR_COUNT; i++) {
        sensor_filter_init(&sensorChannels[i].filter);
    }
}

static void sensor_task_measure(sensor_channel_t* channel) {
    hc_sr04_result_t triggerResult;
    hc_sr04_result_t readResult;
    uint16_t distanceCm;
    uint32_t waitedMs;

    triggerResult = hc_sr04_trigger(&channel->driver);
    if (triggerResult != HC_SR04_OK) {
        /* 이전 캡처가 아직 안 끝났다(BUSY). 포기하고 다음 주기에 재시도한다. */
        hc_sr04_abort(&channel->driver);
        sensor_filter_record_fault(&channel->filter);
        return;
    }

    waitedMs = 0U;
    while ((hc_sr04_is_ready(&channel->driver) == 0U) && (waitedMs < SENSOR_ECHO_WAIT_MS)) {
        osDelay(1U);
        waitedMs++;
    }

    if (hc_sr04_is_ready(&channel->driver) == 0U) {
        hc_sr04_abort(&channel->driver);
        sensor_filter_record_fault(&channel->filter);
        return;
    }

    readResult = hc_sr04_read(&channel->driver, &distanceCm);
    if (readResult != HC_SR04_OK) {
        sensor_filter_record_fault(&channel->filter);
        return;
    }

    sensor_filter_record_sample(&channel->filter, distanceCm);
}

static void sensor_task_report(const sensor_channel_t* channel) {
    uint8_t payload[UART_SENSOR_STATUS_PAYLOAD_SIZE];
    uint16_t distanceCm = sensor_filter_get_distance_cm(&channel->filter);
    uint8_t state = sensor_filter_get_state(&channel->filter);

    payload[UART_SENSOR_ID_INDEX] = channel->sensorId;
    payload[UART_SENSOR_STATE_INDEX] = state;
    payload[UART_SENSOR_DISTANCE_LOW_INDEX] = (uint8_t)(distanceCm & 0xFFU);
    payload[UART_SENSOR_DISTANCE_HIGH_INDEX] = (uint8_t)((distanceCm >> 8U) & 0xFFU);

    (void)CommTx_Send(channel->txChannel, UART_CMD_SENSOR_STATUS, payload, UART_SENSOR_STATUS_PAYLOAD_SIZE);
}

static uint8_t sensor_task_worst_state(uint8_t a, uint8_t b) {
    if ((a == UART_SENSOR_FAULT) || (b == UART_SENSOR_FAULT)) {
        return UART_SENSOR_FAULT;
    }

    if ((a == UART_SENSOR_DETECTED) || (b == UART_SENSOR_DETECTED)) {
        return UART_SENSOR_DETECTED;
    }

    return UART_SENSOR_CLEAR;
}

/* heartbeat payload는 채널당 1바이트뿐이라, 분류 3개 센서는 "가장 나쁜 상태"로 합산한다. */
static void sensor_task_update_heartbeat(void) {
    uint8_t sortingState;

    CommTx_SetSensorState(COMM_TX_CH_INPUT, sensor_filter_get_state(&sensorChannels[0].filter));

    sortingState = sensor_filter_get_state(&sensorChannels[1].filter);
    sortingState = sensor_task_worst_state(sortingState, sensor_filter_get_state(&sensorChannels[2].filter));
    sortingState = sensor_task_worst_state(sortingState, sensor_filter_get_state(&sensorChannels[3].filter));
    CommTx_SetSensorState(COMM_TX_CH_SORTING, sortingState);
}

void SensorTask_Init(void) {
    sensor_task_init_channels();
}

/*
 * HealthTask가 보는 것은 "이 채널이 계속 측정되고 있는가"(소프트웨어 생존성)이지
 * "센서가 지금 유효한 echo를 받았는가"가 아니다. echo 누락은 HC-SR04의 정상
 * 특성이고(범위 밖/비스듬한 표면/흡음재), 그 상태는 sensor_filter가 연속
 * SENSOR_FILTER_FAULT_THRESHOLD회 누락 시 UART_SENSOR_FAULT로 따로 보고한다.
 * 그래서 측정 성공 여부와 무관하게 폴링한 시각을 기록한다.
 */
void SensorTask_PollChannel(uint8_t index) {
    sensorChannels[index].lastPollTick = HAL_GetTick();
    sensor_task_measure(&sensorChannels[index]);
    sensor_task_report(&sensorChannels[index]);
    sensor_task_update_heartbeat();
}

uint8_t SensorTask_GetChannelCount(void) {
    return SENSOR_COUNT;
}

uint32_t SensorTask_GetChannelLastPollTick(uint8_t index) {
    return sensorChannels[index].lastPollTick;
}

comm_tx_channel_t SensorTask_GetChannelProcess(uint8_t index) {
    return sensorChannels[index].txChannel;
}

uint8_t SensorTask_GetChannelSensorId(uint8_t index) {
    return sensorChannels[index].sensorId;
}

/*
 * freertos.c의 __weak StartSensorTask를 덮는 실제 구현.
 */
void StartSensorTask(void* argument) {
    uint8_t i;
    uint32_t cycleStartTick;
    uint32_t elapsedMs;

    (void)argument;

    SensorTask_Init();

    for (;;) {
        for (i = 0U; i < SENSOR_COUNT; i++) {
            cycleStartTick = osKernelGetTickCount();

            SensorTask_PollChannel(i);
            Health_TaskAlive(HEALTH_TASK_SENSOR);

            elapsedMs = osKernelGetTickCount() - cycleStartTick;
            if (elapsedMs < SENSOR_MIN_CYCLE_MS) {
                osDelay(SENSOR_MIN_CYCLE_MS - elapsedMs);
            }
        }
    }
}
