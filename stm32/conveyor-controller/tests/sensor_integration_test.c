#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sensor_task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "cmsis_os2.h"
#include "hc_sr04.h"
#include "health_task.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "main.h"
#include "sensor_filter.h"
#include "tim.h"

void Health_TaskAlive(health_task_id_t id) {
    (void)id;
}

/*
 * ============================================================================
 * SensorTask <-> sensor_filter/CommTxTask 연동 테스트
 * ============================================================================
 *
 * sensor_task_test.c 대신 이 파일 하나로 SensorTask 오케스트레이션을 검증한다.
 * sensor_filter.c(실제 판정 로직)는 그대로 링크해, SensorTask가 raw 거리값을
 * 실제 필터를 거쳐 올바른 CommTx 호출(채널 라우팅/payload 인코딩/heartbeat
 * 최악상태 합산)로 이어지는지 확인한다. hc_sr04 드라이버(GPIO/TIM 직접 접근)와
 * CommTxTask 자체(큐/DMA)는 이 경계 밖이라 fake로 대체한다 - hc_sr04는
 * hc_sr04_test.c가, CommTxTask 인코딩/전송은 app_comm_tx_test.c가 각각 이미
 * 단독으로 검증한다.
 */

GPIO_TypeDef sensorTestTrigGpioC;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim10;

/* ---- fake: hc_sr04 드라이버 ---- */
#define FAKE_SENSOR_MAX 4U

typedef struct {
    hc_sr04_sensor_t* handle;
    hc_sr04_result_t triggerResult;
    uint8_t ready;
    hc_sr04_result_t readResult;
    uint16_t distanceCm;
} fake_hc_sr04_channel_t;

static fake_hc_sr04_channel_t fakeChannels[FAKE_SENSOR_MAX];
static uint8_t fakeChannelCount;

#define MAX_TRIGGER_ORDER_ENTRIES 64U
static uint8_t triggerOrder[MAX_TRIGGER_ORDER_ENTRIES];
static uint32_t triggerOrderCount;

static fake_hc_sr04_channel_t* fake_channel_for(const hc_sr04_sensor_t* sensor) {
    uint8_t i;

    for (i = 0U; i < fakeChannelCount; i++) {
        if (fakeChannels[i].handle == sensor) {
            return &fakeChannels[i];
        }
    }

    assert(0 && "unregistered hc_sr04 sensor");
    return NULL;
}

void hc_sr04_init(hc_sr04_sensor_t* sensor, TIM_HandleTypeDef* timer, uint32_t channel, GPIO_TypeDef* trigPort,
                  uint16_t trigPin) {
    (void)timer;
    (void)channel;
    (void)trigPort;
    (void)trigPin;

    assert(fakeChannelCount < FAKE_SENSOR_MAX);
    fakeChannels[fakeChannelCount].handle = sensor;
    fakeChannels[fakeChannelCount].triggerResult = HC_SR04_OK;
    fakeChannels[fakeChannelCount].ready = 1U;
    fakeChannels[fakeChannelCount].readResult = HC_SR04_OK;
    fakeChannels[fakeChannelCount].distanceCm = 0U;
    fakeChannelCount++;
}

hc_sr04_result_t hc_sr04_trigger(hc_sr04_sensor_t* sensor) {
    fake_hc_sr04_channel_t* channel = fake_channel_for(sensor);

    assert(triggerOrderCount < MAX_TRIGGER_ORDER_ENTRIES);
    triggerOrder[triggerOrderCount] = (uint8_t)(channel - fakeChannels);
    triggerOrderCount++;

    return channel->triggerResult;
}

uint8_t hc_sr04_is_ready(const hc_sr04_sensor_t* sensor) {
    return fake_channel_for(sensor)->ready;
}

hc_sr04_result_t hc_sr04_read(hc_sr04_sensor_t* sensor, uint16_t* distanceCm) {
    fake_hc_sr04_channel_t* channel = fake_channel_for(sensor);

    *distanceCm = channel->distanceCm;
    return channel->readResult;
}

void hc_sr04_abort(hc_sr04_sensor_t* sensor) {
    (void)fake_channel_for(sensor);
}

/* ---- fake: CommTxTask 공용 송신 인터페이스 ---- */
typedef struct {
    comm_tx_channel_t channel;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_SENSOR_STATUS_PAYLOAD_SIZE];
} comm_tx_send_call_t;

#define MAX_COMM_TX_CALLS 32U
static comm_tx_send_call_t commTxSendCalls[MAX_COMM_TX_CALLS];
static uint32_t commTxSendCallCount;
static uint8_t lastSensorState[COMM_TX_CH_COUNT];

int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    comm_tx_send_call_t* call;

    assert(commTxSendCallCount < MAX_COMM_TX_CALLS);
    assert(length <= UART_SENSOR_STATUS_PAYLOAD_SIZE);

    call = &commTxSendCalls[commTxSendCallCount];
    call->channel = channel;
    call->command = command;
    call->length = length;
    memcpy(call->payload, payload, length);
    commTxSendCallCount++;
    return 0;
}

void CommTx_SetSensorState(comm_tx_channel_t process, uint8_t sensor_state) {
    assert(process < COMM_TX_CH_COUNT);
    lastSensorState[process] = sensor_state;
}

/* ---- fake: cmsis_os2. StartSensorTask는 테스트에서 호출하지 않지만, 같은
 * 오브젝트 파일에 있어 링크 시 심볼이 필요하다. ---- */
osStatus_t osDelay(uint32_t ticks) {
    (void)ticks;
    return osOK;
}

uint32_t osKernelGetTickCount(void) {
    return 0U;
}

static uint32_t fakeTick;

uint32_t HAL_GetTick(void) {
    return fakeTick;
}

/* ---- 테스트 헬퍼 ---- */
static void reset_all(void) {
    fakeTick = 0U;
    memset(fakeChannels, 0, sizeof(fakeChannels));
    fakeChannelCount = 0U;
    memset(triggerOrder, 0, sizeof(triggerOrder));
    triggerOrderCount = 0U;
    memset(commTxSendCalls, 0, sizeof(commTxSendCalls));
    commTxSendCallCount = 0U;
    memset(lastSensorState, 0xFFU, sizeof(lastSensorState));

    SensorTask_Init(); /* US1..US4 순서로 hc_sr04_init 4회 호출 -> fakeChannels[0..3] 등록 */
}

/*
 * US1(투입)에서 근접 물체를 3회 연속 감지 -> sensor_filter가 DETECTED로 확정
 * -> COMM_TX_CH_INPUT/UART_CMD_SENSOR_STATUS로 정확히 보고되는지 확인한다.
 */
static void test_input_channel_detection_routes_to_input_comm_channel(void) {
    reset_all();
    fakeChannels[0].distanceCm = 5U;

    SensorTask_PollChannel(0U);
    SensorTask_PollChannel(0U);
    SensorTask_PollChannel(0U);

    assert(triggerOrderCount == 3U);
    assert(triggerOrder[0] == 0U && triggerOrder[1] == 0U && triggerOrder[2] == 0U);

    assert(commTxSendCallCount == 3U);
    {
        const comm_tx_send_call_t* last = &commTxSendCalls[commTxSendCallCount - 1U];
        assert(last->channel == COMM_TX_CH_INPUT);
        assert(last->command == UART_CMD_SENSOR_STATUS);
        assert(last->length == UART_SENSOR_STATUS_PAYLOAD_SIZE);
        assert(last->payload[UART_SENSOR_ID_INDEX] == UART_INPUT_SENSOR_ID_1);
        assert(last->payload[UART_SENSOR_STATE_INDEX] == UART_SENSOR_DETECTED);
        assert(last->payload[UART_SENSOR_DISTANCE_LOW_INDEX] == 5U);
        assert(last->payload[UART_SENSOR_DISTANCE_HIGH_INDEX] == 0U);
    }
    assert(lastSensorState[COMM_TX_CH_INPUT] == UART_SENSOR_DETECTED);
}

/*
 * 한 사이클(US1->US2->US3->US4)이 순서대로 폴링되고, 각 센서가 올바른 채널/
 * sensorId로 라우팅되는지 확인한다(동시 트리거 금지 요구사항의 순서 측면).
 */
static void test_full_cycle_polls_in_order_with_correct_routing(void) {
    reset_all();
    fakeChannels[0].distanceCm = 5U;   /* US1: 근접 */
    fakeChannels[1].distanceCm = 200U; /* US2: 원거리 */
    fakeChannels[2].distanceCm = 200U; /* US3: 원거리 */
    fakeChannels[3].distanceCm = 200U; /* US4: 원거리 */

    SensorTask_PollChannel(0U);
    SensorTask_PollChannel(1U);
    SensorTask_PollChannel(2U);
    SensorTask_PollChannel(3U);

    assert(triggerOrderCount == 4U);
    assert(triggerOrder[0] == 0U);
    assert(triggerOrder[1] == 1U);
    assert(triggerOrder[2] == 2U);
    assert(triggerOrder[3] == 3U);

    assert(commTxSendCallCount == 4U);
    assert(commTxSendCalls[0].channel == COMM_TX_CH_INPUT);
    assert(commTxSendCalls[0].payload[UART_SENSOR_ID_INDEX] == UART_INPUT_SENSOR_ID_1);
    assert(commTxSendCalls[1].channel == COMM_TX_CH_SORTING);
    assert(commTxSendCalls[1].payload[UART_SENSOR_ID_INDEX] == UART_SORTING_SENSOR_ID_1);
    assert(commTxSendCalls[2].channel == COMM_TX_CH_SORTING);
    assert(commTxSendCalls[2].payload[UART_SENSOR_ID_INDEX] == UART_SORTING_SENSOR_ID_2);
    assert(commTxSendCalls[3].channel == COMM_TX_CH_SORTING);
    assert(commTxSendCalls[3].payload[UART_SENSOR_ID_INDEX] == UART_SORTING_SENSOR_ID_3);
}

/*
 * 분류 센서 3개(US2/US3/US4)는 heartbeat 페이로드 1바이트를 공유하므로
 * "가장 나쁜 상태"로 합산된다. FAULT > DETECTED > CLEAR 우선순위를 확인한다.
 */
static void test_sorting_heartbeat_reports_worst_state_across_three_sensors(void) {
    uint32_t i;

    reset_all();

    /* US2: 근접 3회 -> DETECTED */
    fakeChannels[1].distanceCm = 5U;
    SensorTask_PollChannel(1U);
    SensorTask_PollChannel(1U);
    SensorTask_PollChannel(1U);
    assert(lastSensorState[COMM_TX_CH_SORTING] == UART_SENSOR_DETECTED);

    /* US3: 연속 무효 측정 threshold회 -> FAULT (DETECTED보다 나쁨) */
    fakeChannels[2].readResult = HC_SR04_OUT_OF_RANGE;
    for (i = 0U; i < SENSOR_FILTER_FAULT_THRESHOLD; i++) {
        SensorTask_PollChannel(2U);
    }

    /* US4는 한 번도 폴링되지 않아 기본 CLEAR로 남는다. */
    assert(lastSensorState[COMM_TX_CH_SORTING] == UART_SENSOR_FAULT);
}

/*
 * echo timeout이 threshold회 연속되면 FAULT로 전환되고, 보고 payload의 거리는
 * UART_SENSOR_DISTANCE_UNKNOWN(0xFFFF)로 인코딩되어야 한다(안전 신호 우선).
 */
static void test_fault_after_threshold_reports_fault_and_unknown_distance(void) {
    uint32_t i;

    reset_all();
    fakeChannels[0].ready = 0U; /* is_ready()가 계속 0 -> echo timeout 경로 */

    for (i = 0U; i < (SENSOR_FILTER_FAULT_THRESHOLD - 1U); i++) {
        SensorTask_PollChannel(0U);
    }
    assert(lastSensorState[COMM_TX_CH_INPUT] != UART_SENSOR_FAULT);

    SensorTask_PollChannel(0U); /* threshold회째: FAULT로 전이 */

    {
        const comm_tx_send_call_t* last = &commTxSendCalls[commTxSendCallCount - 1U];
        assert(last->payload[UART_SENSOR_STATE_INDEX] == UART_SENSOR_FAULT);
        assert(last->payload[UART_SENSOR_DISTANCE_LOW_INDEX] == 0xFFU);
        assert(last->payload[UART_SENSOR_DISTANCE_HIGH_INDEX] == 0xFFU);
    }
    assert(lastSensorState[COMM_TX_CH_INPUT] == UART_SENSOR_FAULT);
}

/*
 * echo를 계속 못 받아도 폴링되고 있는 한 lastPollTick은 계속 갱신되어야 한다.
 *
 * HealthTask가 이 값으로 판정하려는 것은 "SensorTask가 이 채널을 계속 서비스하고
 * 있는가"이지 "센서가 유효한 echo를 받았는가"가 아니다. echo 누락은 HC-SR04의
 * 정상 특성(범위 밖/비스듬한 표면/흡음재)이라, 예전처럼 측정 성공 시각만 기록하면
 * 정상 배선 상태에서도 SENSOR_STALE이 끊임없이 발생한다(2026-07-27 실기기에서
 * 분류 3채널 전부에서 재현). echo 누락이 지속되는 상황은 sensor_filter가
 * UART_SENSOR_FAULT로 따로 보고하므로 여기서 중복해서 알릴 필요가 없다.
 */
static void test_failed_measurements_still_advance_poll_tick(void) {
    uint32_t i;

    reset_all();
    fakeChannels[1].ready = 0U; /* is_ready()가 계속 0 -> echo timeout 경로로만 진행 */

    for (i = 0U; i < (SENSOR_FILTER_FAULT_THRESHOLD + 5U); i++) {
        fakeTick += 240U; /* 4채널 순차 폴링 기준 채널당 재방문 주기(60ms x 4) */
        SensorTask_PollChannel(1U);
        /* 한 번도 성공하지 못했어도 폴링 자체는 계속되므로 stale로 보이면 안 된다. */
        assert(SensorTask_GetChannelLastPollTick(1U) == fakeTick);
    }

    /* 대신 애플리케이션 경로가 FAULT로 이 상황을 보고한다(중복 아닌 각자의 역할). */
    assert(lastSensorState[COMM_TX_CH_SORTING] == UART_SENSOR_FAULT);
}

int main(void) {
    test_input_channel_detection_routes_to_input_comm_channel();
    test_full_cycle_polls_in_order_with_correct_routing();
    test_sorting_heartbeat_reports_worst_state_across_three_sensors();
    test_fault_after_threshold_reports_fault_and_unknown_distance();
    test_failed_measurements_still_advance_poll_tick();
    return 0;
}
