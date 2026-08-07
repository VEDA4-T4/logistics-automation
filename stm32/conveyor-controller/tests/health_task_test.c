#ifdef NDEBUG
#undef NDEBUG
#endif

#include "health_task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "cmsis_os2.h"
#include "comm_rx_task.h"
#include "health_hw.h"
#include "input_control_task.h"
#include "safety_task.h"
#include "sorting_control_task.h"

#include "logistics/contracts/uart_protocol.h"

/*
 * ============================================================================
 * HealthTask 단독(fake 상대) 테스트
 * ============================================================================
 *
 * 6개 감시대상 태스크의 stats getter/CommTx/SafetyTask/HealthHw(레지스터 접근
 * 계층)를 전부 fake로 대체해 HealthTask 자체 판정 로직만 검증한다.
 */

static uint32_t fakeTick;

/* 0이면 tick이 고정(대부분의 테스트). 0이 아니면 HAL_GetTick 호출마다 그만큼
 * 흘러서, RunCycle 도중 선점되어 시간이 지나는 상황을 재현할 수 있다. */
static uint32_t fakeTickAdvancePerCall;

uint32_t HAL_GetTick(void) {
    uint32_t value = fakeTick;
    fakeTick += fakeTickAdvancePerCall;
    return value;
}

osStatus_t osDelay(uint32_t ticks) {
    (void)ticks;
    return osOK;
}

/* ---- fake: comm_rx ---- */
static comm_rx_stats_t fakeCommRxStats;
static uint32_t fakeCommRxLastRxTick[2];

void comm_rx_get_stats(comm_rx_stats_t* stats) {
    *stats = fakeCommRxStats;
}

uint32_t comm_rx_get_channel_last_rx_tick(app_uart_channel_t channel) {
    return fakeCommRxLastRxTick[(channel == APP_UART_CHANNEL_6) ? 1U : 0U];
}

/* ---- fake: InputControlTask/SortingControlTask stats ---- */
static input_control_task_stats_t fakeInputStats;
static sorting_control_task_stats_t fakeSortingStats;

void input_control_task_get_stats(input_control_task_stats_t* stats) {
    *stats = fakeInputStats;
}

void sorting_control_task_get_stats(sorting_control_task_stats_t* stats) {
    *stats = fakeSortingStats;
}

/* ---- fake: SafetyTask ---- */
static safety_task_stats_t fakeSafetyStats;
static uint32_t safetyTriggerCalls;
static safety_cause_t lastSafetyTriggerCause;

void Safety_GetStats(safety_task_stats_t* stats) {
    *stats = fakeSafetyStats;
}

void Safety_TriggerEmergencyStop(safety_cause_t cause) {
    safetyTriggerCalls++;
    lastSafetyTriggerCause = cause;
}

/* ---- fake: CommTx ---- */
static comm_tx_stats_t fakeCommTxStats;

typedef struct {
    comm_tx_channel_t channel;
    uint8_t command;
    uint8_t payload[APP_HEALTH_EVENT_PAYLOAD_SIZE];
} comm_tx_send_call_t;

#define MAX_COMM_TX_CALLS 16U
static comm_tx_send_call_t commTxSendCalls[MAX_COMM_TX_CALLS];
static uint32_t commTxSendCallCount;

const comm_tx_stats_t* CommTx_GetStats(void) {
    return &fakeCommTxStats;
}

int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    assert(commTxSendCallCount < MAX_COMM_TX_CALLS);
    assert(length <= APP_HEALTH_EVENT_PAYLOAD_SIZE);
    commTxSendCalls[commTxSendCallCount].channel = channel;
    commTxSendCalls[commTxSendCallCount].command = command;
    memcpy(commTxSendCalls[commTxSendCallCount].payload, payload, length);
    commTxSendCallCount++;
    return 0;
}

/* ---- fake: SensorTask ---- */
static uint32_t fakeSensorLastPollTick[4];
static comm_tx_channel_t fakeSensorProcess[4] = { COMM_TX_CH_INPUT, COMM_TX_CH_SORTING, COMM_TX_CH_SORTING,
                                                  COMM_TX_CH_SORTING };
/* 실기기 매핑과 동일: US1(input)=1, US2(sorting dest1)=1, US3(dest2)=2, US4(dest3)=3. */
static uint8_t fakeSensorId[4] = { 1U, 1U, 2U, 3U };

uint8_t SensorTask_GetChannelCount(void) {
    return 4U;
}

uint32_t SensorTask_GetChannelLastPollTick(uint8_t index) {
    return fakeSensorLastPollTick[index];
}

comm_tx_channel_t SensorTask_GetChannelProcess(uint8_t index) {
    return fakeSensorProcess[index];
}

uint8_t SensorTask_GetChannelSensorId(uint8_t index) {
    return fakeSensorId[index];
}

/* ---- fake: HealthHw(레지스터 접근 계층) ---- */
static uint32_t iwdgStartCalls;
static uint32_t iwdgRefreshCalls;
static health_reset_cause_t fakeResetCause;
static uint32_t fakeStackHighWaterMark[HEALTH_TASK_MONITORED_COUNT];
static health_persisted_record_t fakePersistedRecord; /* 실기기 .noinit 흉내 - reset_all에서도 안 지움 */

void HealthHw_IwdgStart(uint32_t prescalerReg, uint32_t reload) {
    (void)prescalerReg;
    (void)reload;
    iwdgStartCalls++;
}

void HealthHw_IwdgRefresh(void) {
    iwdgRefreshCalls++;
}

health_reset_cause_t HealthHw_CaptureResetCause(void) {
    return fakeResetCause;
}

uint32_t HealthHw_GetStackHighWaterMark(health_task_id_t id) {
    return fakeStackHighWaterMark[id];
}

health_persisted_record_t* HealthHw_GetPersistedRecord(void) {
    return &fakePersistedRecord;
}

/* ---- 테스트 헬퍼 ---- */
static void health_ping_all(void) {
    health_task_id_t id;
    for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
        Health_TaskAlive(id);
    }
}

static void health_ping_all_except(health_task_id_t exceptId) {
    health_task_id_t id;
    for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
        if (id != exceptId) {
            Health_TaskAlive(id);
        }
    }
}

static void reset_all(void) {
    fakeTick = 0U;
    fakeTickAdvancePerCall = 0U;
    memset(&fakeCommRxStats, 0, sizeof(fakeCommRxStats));
    memset(fakeCommRxLastRxTick, 0, sizeof(fakeCommRxLastRxTick));
    memset(&fakeInputStats, 0, sizeof(fakeInputStats));
    memset(&fakeSortingStats, 0, sizeof(fakeSortingStats));
    memset(&fakeSafetyStats, 0, sizeof(fakeSafetyStats));
    safetyTriggerCalls = 0U;
    lastSafetyTriggerCause = SAFETY_CAUSE_NONE;
    memset(&fakeCommTxStats, 0, sizeof(fakeCommTxStats));
    memset(commTxSendCalls, 0, sizeof(commTxSendCalls));
    commTxSendCallCount = 0U;
    memset(fakeSensorLastPollTick, 0, sizeof(fakeSensorLastPollTick));
    iwdgStartCalls = 0U;
    iwdgRefreshCalls = 0U;
    fakeResetCause = HEALTH_RESET_POWER_ON;

    {
        uint8_t i;
        for (i = 0U; i < HEALTH_TASK_MONITORED_COUNT; i++) {
            fakeStackHighWaterMark[i] = 512U; /* 넉넉한 기본값 - 정상 */
        }
    }
}

/*
 * 모든 태스크가 매 사이클 ping하고, Queue/센서/UART 전부 정상이면 IWDG가
 * 계속 갱신되고 어떤 EVENT도 안 나가야 한다.
 */
static void test_normal_cycle_refreshes_watchdog_without_events(void) {
    reset_all();
    HealthTask_Init();
    assert(iwdgStartCalls == 1U);
    assert(Health_GetResetCause() == HEALTH_RESET_POWER_ON);

    health_ping_all();
    HealthTask_RunCycle();

    assert(iwdgRefreshCalls == 1U);
    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 0U);
}

/*
 * 한 태스크가 STALE 기준시간 이상 ping을 멈추면 hang으로 판정되어
 * Safety_TriggerEmergencyStop이 호출되고, .noinit 격 fake 레코드에 원인이
 * 남으며, 그 뒤로는 latch되어 IWDG 갱신도 멈춘다.
 */
static void test_task_hang_triggers_fatal_and_halts_watchdog(void) {
    health_persisted_record_t record;

    reset_all();
    HealthTask_Init();

    health_ping_all();
    HealthTask_RunCycle();
    assert(safetyTriggerCalls == 0U);

    /* SortingControlTask만 ping을 멈추고, 기준시간을 넉넉히 넘긴다. */
    fakeTick += 1000U;
    health_ping_all_except(HEALTH_TASK_SORTING_CONTROL);
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 1U);
    assert(lastSafetyTriggerCause == SAFETY_CAUSE_FATAL_ERROR);

    Health_GetLastPersistedRecord(&record);
    assert(record.reason == (uint32_t)HEALTH_FATAL_TASK_HANG);
    assert(record.offendingTask == (uint32_t)HEALTH_TASK_SORTING_CONTROL);

    /* latch됐으니 이후 사이클에서 다시 살아나도(=ping 재개) fatal은 반복 호출 안 되고,
     * IWDG 갱신도 계속 멈춘 채로 남는다(그레이스풀 경로가 아니라 hw backstop 대기). */
    fakeTick += 100U;
    health_ping_all();
    uint32_t refreshesBefore = iwdgRefreshCalls;
    HealthTask_RunCycle();
    assert(iwdgRefreshCalls == refreshesBefore);
    assert(safetyTriggerCalls == 1U);
}

/*
 * 남은 스택이 마진 밑으로 내려가면 STACK_MARGIN 치명으로 승격된다.
 */
static void test_stack_margin_triggers_fatal(void) {
    reset_all();
    HealthTask_Init();

    health_ping_all();
    fakeStackHighWaterMark[HEALTH_TASK_SENSOR] = 4U; /* 마진 미만 */
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 1U);
    assert(lastSafetyTriggerCause == SAFETY_CAUSE_FATAL_ERROR);

    health_persisted_record_t record;
    Health_GetLastPersistedRecord(&record);
    assert(record.reason == (uint32_t)HEALTH_FATAL_STACK_MARGIN);
    assert(record.offendingTask == (uint32_t)HEALTH_TASK_SENSOR);
}

/*
 * 한쪽 UART 채널이 오래 조용하면(단절) 비치명 HEALTH EVENT가 "살아있는
 * 반대쪽" 채널로만 보고된다 - 끊긴 채널로는 보내봐야 도착 안 하므로.
 * fatal은 아니라 IWDG는 계속 갱신된다.
 */
static void test_uart_channel_timeout_reports_to_opposite_channel(void) {
    reset_all();
    HealthTask_Init();

    health_ping_all();
    HealthTask_RunCycle(); /* baseline: 둘 다 tick=0, 아직 stale 아님 */
    assert(commTxSendCallCount == 0U);

    fakeTick = 6000U; /* 채널 timeout 기준(5000ms)보다 크게 */
    fakeCommRxLastRxTick[1] = fakeTick; /* 분류(sorting) 채널만 방금 활동, 투입은 여전히 0 */
    /* 센서 갱신 지연 체크와 간섭되지 않게 4채널 다 최신으로 고정(이 테스트의 관심사 아님). */
    fakeSensorLastPollTick[0] = fakeTick;
    fakeSensorLastPollTick[1] = fakeTick;
    fakeSensorLastPollTick[2] = fakeTick;
    fakeSensorLastPollTick[3] = fakeTick;
    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(iwdgRefreshCalls == 2U); /* fatal 아니므로 계속 갱신 */
    assert(commTxSendCallCount == 1U);
    assert(commTxSendCalls[0].channel == COMM_TX_CH_SORTING); /* 끊긴 input의 반대쪽 */
    assert(commTxSendCalls[0].command == UART_CMD_EVENT);
    assert(commTxSendCalls[0].payload[UART_EVENT_ID_INDEX] == APP_EVENT_HEALTH);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_KIND_INDEX] == (uint8_t)HEALTH_ISSUE_UART_CHANNEL_TIMEOUT);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_CAUSE_INDEX] == (uint8_t)COMM_TX_CH_INPUT);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] == HEALTH_ISSUE_SENSOR_ID_NONE);

    /* 같은 상태가 지속돼도 재전송하지 않는다(latch). */
    fakeTick += 100U;
    fakeCommRxLastRxTick[1] = fakeTick;
    health_ping_all();
    HealthTask_RunCycle();
    assert(commTxSendCallCount == 1U);
}

/*
 * 센서 채널 하나가 오래 갱신되지 않으면 그 채널이 속한 공정으로 비치명
 * SENSOR_STALE EVENT가 보고된다.
 */
static void test_sensor_staleness_reports_to_owning_process(void) {
    reset_all();
    HealthTask_Init();

    health_ping_all();
    HealthTask_RunCycle();
    assert(commTxSendCallCount == 0U);

    fakeTick = 1100U; /* 센서 stale 기준(1000ms)보다 크게, 나머지 채널은 갱신되게 함 */
    fakeSensorLastPollTick[0] = fakeTick; /* index0=US1=input, 최신 */
    fakeSensorLastPollTick[1] = fakeTick; /* US2=sorting, 최신 */
    fakeSensorLastPollTick[2] = fakeTick; /* US3=sorting, 최신 */
    /* index3(US4=sorting)만 tick=0으로 방치 -> stale */
    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 1U);
    assert(commTxSendCalls[0].channel == COMM_TX_CH_SORTING);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_KIND_INDEX] == (uint8_t)HEALTH_ISSUE_SENSOR_STALE);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_CAUSE_INDEX] == (uint8_t)COMM_TX_CH_SORTING);
    /* index3=US4, fakeSensorId[3]=3 -> 분류 쪽 3개 센서 중 어느 것인지 구분 가능해야 함. */
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] == 3U);
}

/*
 * HealthTask가 RunCycle 도중 선점되어 늦게 도착해도, 그 사이 정상적으로 폴링된
 * 센서를 stale로 오판하면 안 된다.
 *
 * HealthTask는 앱 태스크 중 최하위 우선순위라, RunCycle 시작에서 센서 검사까지
 * 도달하는 동안 다른 태스크에 밀릴 수 있다. 그 사이 SensorTask는 계속 돌아
 * lastPollTick을 RunCycle 시작 시점보다 더 나중 값으로 갱신한다. 이때 시작
 * 시점의 now를 그대로 쓰면 unsigned 뺄셈이 언더플로우해(약 42억) 실제 지연과
 * 무관하게 항상 stale로 오판됐다 - 2026-07-28 실기기 라이브 디버깅에서 네 채널
 * lastPollTick이 전부 200ms 이내인데도 STALE 보고 지점에 도달한 것으로 확인.
 */
static void test_late_health_cycle_does_not_underflow_sensor_age(void) {
    reset_all();
    HealthTask_Init();

    health_ping_all();
    HealthTask_RunCycle();
    assert(commTxSendCallCount == 0U);

    /* RunCycle이 now를 뜬 뒤 매 검사 사이 150ms씩 밀리는 상황을 만든다. */
    fakeTick = 5000U;
    fakeTickAdvancePerCall = 150U;

    /* 네 채널 모두 RunCycle 시작(5000)보다 나중인 5100에 정상 폴링됨 -
     * 즉 stale이 아니라 오히려 now보다 최신이다. */
    fakeSensorLastPollTick[0] = 5100U;
    fakeSensorLastPollTick[1] = 5100U;
    fakeSensorLastPollTick[2] = 5100U;
    fakeSensorLastPollTick[3] = 5100U;
    /* UART 채널도 같은 이유로 최신 상태여야 단절 오판이 끼어들지 않는다. */
    fakeCommRxLastRxTick[0] = 5100U;
    fakeCommRxLastRxTick[1] = 5100U;

    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 0U); /* 어떤 EVENT도 나가면 안 된다 */
}

/*
 * Queue drop이 한두 사이클만 늘고 멈추면(일시적) 비치명 보고만 하고 fatal로
 * 승격하지 않는다.
 */
static void test_transient_queue_overflow_reports_without_fatal(void) {
    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline snapshot */

    fakeInputStats.txQueueDrops = 1U; /* 1회만 증가 */
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 1U);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_KIND_INDEX] ==
           (uint8_t)HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT);
    assert(commTxSendCalls[0].channel == COMM_TX_CH_INPUT); /* InputControlTask -> 투입 공정 */
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] == HEALTH_ISSUE_SENSOR_ID_NONE);

    /* drop이 더 안 늘면 streak가 리셋된다(연속이 아니므로 fatal로 안 감). */
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();
    assert(safetyTriggerCalls == 0U);
}

/*
 * Queue drop이 연속 여러 사이클 계속 늘면(지속적 포화) 치명으로 승격된다.
 */
static void test_sustained_queue_overflow_escalates_to_fatal(void) {
    uint32_t i;

    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline */

    for (i = 0U; i < 5U; i++) {
        fakeCommTxStats.dropped_queue_full++;
        fakeTick += 100U;
        health_ping_all();
        HealthTask_RunCycle();
    }

    assert(safetyTriggerCalls == 1U);

    health_persisted_record_t record;
    Health_GetLastPersistedRecord(&record);
    assert(record.reason == (uint32_t)HEALTH_FATAL_QUEUE_OVERFLOW);
    assert(record.offendingTask == (uint32_t)HEALTH_TASK_COMM_TX);
}

/*
 * RX DMA 재시작(uartRestarts)은 오류를 감지해 복구를 돌렸다는 신호지 메시지가
 * 버려졌다는 뜻이 아니다. queue overflow(유실)가 아니라 UART_RECOVERY 진단으로
 * 보고돼야 한다.
 */
static void test_uart_restart_reports_recovery_not_overflow(void) {
    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline snapshot */

    fakeCommRxStats.uartRestarts = 1U; /* 1회만 증가 */
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 2U); /* DEVICE_WIDE -> 양쪽 채널 모두 보고 */
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_KIND_INDEX] == (uint8_t)HEALTH_ISSUE_UART_RECOVERY);
    assert(commTxSendCalls[1].payload[APP_HEALTH_EVENT_KIND_INDEX] == (uint8_t)HEALTH_ISSUE_UART_RECOVERY);
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] == HEALTH_ISSUE_SENSOR_ID_NONE);

    {
        health_task_stats_t stats;
        Health_GetStats(&stats);
        assert(stats.uartRecoveryEvents == 1U);
        assert(stats.queueOverflowTransient == 0U); /* 유실 집계에는 안 잡혀야 한다 */
    }
}

/*
 * tx_error/tx_timeout도 마찬가지다. 프레임을 유지한 채 재시도를 거는 경로라
 * (app_comm_tx.c comm_tx_service_restarts) 재시도가 성공하면 유실이 아니다.
 */
static void test_tx_error_reports_recovery_not_overflow(void) {
    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline snapshot */

    fakeCommTxStats.tx_error[COMM_TX_CH_INPUT] = 1U; /* 1회만 증가 */
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    assert(safetyTriggerCalls == 0U);
    assert(commTxSendCallCount == 2U); /* DEVICE_WIDE -> 양쪽 채널 모두 보고 */
    assert(commTxSendCalls[0].payload[APP_HEALTH_EVENT_KIND_INDEX] == (uint8_t)HEALTH_ISSUE_UART_RECOVERY);

    {
        health_task_stats_t stats;
        Health_GetStats(&stats);
        assert(stats.uartRecoveryEvents == 1U);
        assert(stats.queueOverflowTransient == 0U);
    }
}

/*
 * 이 테스트가 이번 수정의 핵심이다.
 *
 * 재시도가 매번 성공하더라도 UART 오류가 주기적으로 계속 발생할 수 있다. 예전에는
 * tx_error/tx_timeout/uartRestarts가 유실 합계에 섞여 있어서, 이 상황이
 * HEALTH_QUEUE_OVERFLOW_SUSTAIN_CYCLES(5) 사이클만 이어져도 지속적 포화로 오판해
 * E-Stop을 걸고 IWDG 갱신까지 멈췄다. 실제로 버려진 메시지는 하나도 없는데
 * 시스템이 종료되는 셈이었다.
 */
static void test_sustained_uart_recovery_never_escalates_to_fatal(void) {
    uint32_t i;
    uint32_t refreshesBefore;

    reset_all();
    memset(&fakePersistedRecord, 0, sizeof(fakePersistedRecord)); /* 앞선 테스트의 기록과 섞이지 않게 */
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline */

    refreshesBefore = iwdgRefreshCalls;

    /* 승격 기준(5)의 4배를 넘겨도 - 전송은 매번 재시도 끝에 성공한다. */
    for (i = 0U; i < 20U; i++) {
        fakeCommTxStats.tx_timeout[COMM_TX_CH_INPUT]++;
        fakeCommTxStats.tx_error[COMM_TX_CH_SORTING]++;
        fakeCommRxStats.uartRestarts++;
        fakeTick += 100U;
        health_ping_all();
        HealthTask_RunCycle();
    }

    assert(safetyTriggerCalls == 0U);

    {
        health_task_stats_t stats;
        Health_GetStats(&stats);
        assert(stats.queueOverflowFatal == 0U);
        assert(stats.queueOverflowTransient == 0U);
        assert(stats.fatalTriggers == 0U);
        /* 진단 자체는 남되, latch 때문에 100ms마다 도배되지는 않는다. */
        assert(stats.uartRecoveryEvents == 1U);
    }

    /* 치명이 아니므로 IWDG 갱신도 계속돼야 한다. */
    assert(iwdgRefreshCalls == refreshesBefore + 20U);

    {
        health_persisted_record_t record;
        Health_GetLastPersistedRecord(&record);
        assert(record.reason == (uint32_t)HEALTH_FATAL_NONE);
    }
}

/*
 * 반대로, 재시도까지 소진해 실제로 버려진 경우(dropped_retry_exhausted)는
 * 그대로 유실로 잡혀 지속되면 치명으로 승격돼야 한다 - 이번 수정이 감시망에
 * 구멍을 내지 않았는지 확인한다.
 */
static void test_retry_exhausted_still_escalates_to_fatal(void) {
    uint32_t i;

    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline */

    for (i = 0U; i < 5U; i++) {
        fakeCommTxStats.dropped_retry_exhausted++;
        fakeTick += 100U;
        health_ping_all();
        HealthTask_RunCycle();
    }

    assert(safetyTriggerCalls == 1U);

    {
        health_persisted_record_t record;
        Health_GetLastPersistedRecord(&record);
        assert(record.reason == (uint32_t)HEALTH_FATAL_QUEUE_OVERFLOW);
        assert(record.offendingTask == (uint32_t)HEALTH_TASK_COMM_TX);
    }
}

/*
 * 복구가 한 사이클이라도 잦아들면 latch가 풀려 다음 발생을 다시 보고한다
 * (진단이 첫 1회만 보이고 영영 침묵하지 않는지 확인).
 */
static void test_uart_recovery_reports_again_after_quiet_cycle(void) {
    reset_all();
    HealthTask_Init();
    health_ping_all();
    HealthTask_RunCycle(); /* baseline */

    fakeCommRxStats.uartRestarts = 1U;
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    /* 조용한 사이클 - 카운터 변화 없음 */
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    fakeCommRxStats.uartRestarts = 2U;
    fakeTick += 100U;
    health_ping_all();
    HealthTask_RunCycle();

    {
        health_task_stats_t stats;
        Health_GetStats(&stats);
        assert(stats.uartRecoveryEvents == 2U);
    }
    assert(safetyTriggerCalls == 0U);
}

/*
 * .noinit 격 fake 레코드가 아직 유효하지 않으면(magic 불일치) NONE으로
 * 보고된다 - 첫 부팅이나 전원 손실 직후를 흉내낸다.
 */
static void test_no_persisted_record_reports_none(void) {
    health_persisted_record_t record;

    reset_all();
    memset(&fakePersistedRecord, 0, sizeof(fakePersistedRecord)); /* magic=0, 무효 */
    HealthTask_Init();

    Health_GetLastPersistedRecord(&record);
    assert(record.reason == (uint32_t)HEALTH_FATAL_NONE);
    assert(record.offendingTask == (uint32_t)HEALTH_TASK_MONITORED_COUNT);
}

int main(void) {
    test_normal_cycle_refreshes_watchdog_without_events();
    test_task_hang_triggers_fatal_and_halts_watchdog();
    test_stack_margin_triggers_fatal();
    test_uart_channel_timeout_reports_to_opposite_channel();
    test_sensor_staleness_reports_to_owning_process();
    test_late_health_cycle_does_not_underflow_sensor_age();
    test_transient_queue_overflow_reports_without_fatal();
    test_sustained_queue_overflow_escalates_to_fatal();
    test_uart_restart_reports_recovery_not_overflow();
    test_tx_error_reports_recovery_not_overflow();
    test_sustained_uart_recovery_never_escalates_to_fatal();
    test_retry_exhausted_still_escalates_to_fatal();
    test_uart_recovery_reports_again_after_quiet_cycle();
    test_no_persisted_record_reports_none();
    return 0;
}
