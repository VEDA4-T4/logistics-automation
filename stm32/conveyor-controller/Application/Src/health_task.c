#include "health_task.h"

#include <stdatomic.h>
#include <string.h>

#include "cmsis_os2.h"
#include "comm_rx_task.h"
#include "health_hw.h"
#include "input_control_task.h"
#include "safety_task.h"
#include "sensor_task.h"
#include "sorting_control_task.h"
#include "stm32f4xx_hal.h"

#include "logistics/contracts/uart_protocol.h"

/* HealthTask 감시 주기. */
#define HEALTH_CYCLE_MS 100U

/* 이 시간 안에 Health_TaskAlive()가 안 들어오면 hang으로 본다. 감시대상 태스크
 * 루프는 전부 <=100ms bounded wait라 300ms면 스케줄링 지터를 감안해도 충분히
 * 여유 있으면서 IWDG 타임아웃(~2s)보다 훨씬 짧다. */
#define HEALTH_TASK_STALE_MS 300U

/* 남은 스택이 이 워드 수 미만이면 치명으로 본다. */
#define HEALTH_STACK_MARGIN_WORDS 32U

/* 센서 채널의 마지막 유효 표본으로부터 이 시간이 지나면 갱신 지연으로 본다.
 * 실측 결과(2026-07-23, 손으로 직접 든 실제 HC-SR04) sensor_filter의 10회 연속
 * fault 기준보다 훨씬 엄격해서, 손 각도 등으로 인한 짧은 echo 실패에도 500ms는
 * 너무 자주 걸렸다 - 1000ms로 완화. */
#define HEALTH_SENSOR_STALE_MS 1000U

/* UART 채널에서 이 시간 동안 바이트 수신이 전혀 없으면 단절로 본다.
 * Pi가 유휴 시 얼마나 조용할 수 있는지 아직 실측 전이라 넉넉하게 잡음 -
 * 실제 트래픽 패턴 확인 후 조정 필요. */
#define HEALTH_UART_CHANNEL_TIMEOUT_MS 5000U

/* Queue overflow가 이 사이클 수만큼 연속 발생해야 치명으로 승격한다(일시적
 * 튐과 지속적 포화를 구분). */
#define HEALTH_QUEUE_OVERFLOW_SUSTAIN_CYCLES 5U

/* IWDG: LSI(~32kHz) / 64 분주, reload 1000 -> 약 2.0초. */
#define HEALTH_IWDG_PRESCALER_REG 4U
#define HEALTH_IWDG_RELOAD 1000U

#define HEALTH_PERSISTED_MAGIC 0x48544B31U /* "HTK1" */

#define HEALTH_SENSOR_MAX_CHANNELS 4U

typedef struct {
    uint8_t reported;
} health_latch_t;

static _Atomic uint32_t healthAliveCounters[HEALTH_TASK_MONITORED_COUNT];
static uint32_t healthLastSeenCounters[HEALTH_TASK_MONITORED_COUNT];
static uint32_t healthLastChangeTick[HEALTH_TASK_MONITORED_COUNT];
static uint8_t healthTaskHangLatched[HEALTH_TASK_MONITORED_COUNT];
static uint8_t healthStackMarginLatched[HEALTH_TASK_MONITORED_COUNT];
static uint32_t healthQueueDropSnapshot[HEALTH_TASK_MONITORED_COUNT];
static uint32_t healthQueueOverflowStreak[HEALTH_TASK_MONITORED_COUNT];
static uint8_t healthQueueOverflowFatalLatched;

static health_latch_t healthUartLatch[COMM_TX_CH_COUNT];
static health_latch_t healthSensorLatch[HEALTH_SENSOR_MAX_CHANNELS];

static health_reset_cause_t healthResetCause;
static health_task_stats_t healthStats;
static uint8_t healthAnyFatalLatched;

void Health_TaskAlive(health_task_id_t id) {
    if (id < HEALTH_TASK_MONITORED_COUNT) {
        atomic_fetch_add_explicit(&healthAliveCounters[id], 1U, memory_order_relaxed);
    }
}

static uint32_t health_checksum(uint32_t reason, uint32_t offendingTask, uint32_t tick) {
    return reason ^ (offendingTask * 0x9E3779B1U) ^ (tick * 0x85EBCA77U) ^ 0xA5A5A5A5U;
}

static void health_persist_fatal(health_fatal_reason_t reason, health_task_id_t offendingTask, uint32_t tick) {
    health_persisted_record_t* record = HealthHw_GetPersistedRecord();

    record->reason = (uint32_t)reason;
    record->offendingTask = (uint32_t)offendingTask;
    record->tick = tick;
    record->checksum = health_checksum(record->reason, record->offendingTask, record->tick);
    record->magic = HEALTH_PERSISTED_MAGIC; /* 마지막에 써서 중간에 리셋되면 무효로 남게 */
}

static void health_trigger_fatal(health_fatal_reason_t reason, health_task_id_t offendingTask) {
    health_persist_fatal(reason, offendingTask, HAL_GetTick());
    healthStats.fatalTriggers++;
    healthAnyFatalLatched = 1U;
    Safety_TriggerEmergencyStop(SAFETY_CAUSE_FATAL_ERROR);
}

static void health_send_payload(comm_tx_channel_t channel, const uint8_t* payload) {
    if (CommTx_Send(channel, UART_CMD_EVENT, payload, APP_HEALTH_EVENT_PAYLOAD_SIZE) != 0) {
        healthStats.reportDrops++;
    }
}

/*
 * 채널 자체가 단절 대상(UART_CHANNEL_TIMEOUT)이면 그 채널로는 보고해도
 * 도착하지 않으므로 살아있는 반대쪽에 알린다. 그 외(예: 센서 갱신 지연)는
 * 해당 공정의 UART 자체는 살아있다고 보고 그 채널로 보고한다.
 * cause==DEVICE_WIDE면 양쪽에 best-effort로 보고한다.
 */
static void health_report_event(health_issue_kind_t kind, uint8_t cause) {
    uint8_t payload[APP_HEALTH_EVENT_PAYLOAD_SIZE];
    uint32_t tick = HAL_GetTick();

    payload[UART_EVENT_ID_INDEX] = APP_EVENT_HEALTH;
    payload[APP_HEALTH_EVENT_KIND_INDEX] = (uint8_t)kind;
    payload[APP_HEALTH_EVENT_CAUSE_INDEX] = cause;
    payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX] = (uint8_t)(tick & 0xFFU);
    payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 1U] = (uint8_t)((tick >> 8U) & 0xFFU);
    payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 2U] = (uint8_t)((tick >> 16U) & 0xFFU);
    payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 3U] = (uint8_t)((tick >> 24U) & 0xFFU);

    if (cause == HEALTH_ISSUE_CAUSE_DEVICE_WIDE) {
        health_send_payload(COMM_TX_CH_INPUT, payload);
        health_send_payload(COMM_TX_CH_SORTING, payload);
        return;
    }

    if (kind == HEALTH_ISSUE_UART_CHANNEL_TIMEOUT) {
        health_send_payload((cause == (uint8_t)COMM_TX_CH_INPUT) ? COMM_TX_CH_SORTING : COMM_TX_CH_INPUT, payload);
        return;
    }

    health_send_payload((comm_tx_channel_t)cause, payload);
}

static uint8_t health_report_cause(health_task_id_t id) {
    if (id == HEALTH_TASK_INPUT_CONTROL) {
        return (uint8_t)COMM_TX_CH_INPUT;
    }
    if (id == HEALTH_TASK_SORTING_CONTROL) {
        return (uint8_t)COMM_TX_CH_SORTING;
    }
    return HEALTH_ISSUE_CAUSE_DEVICE_WIDE;
}

static uint32_t health_task_drop_count(health_task_id_t id) {
    switch (id) {
        case HEALTH_TASK_COMM_RX: {
            comm_rx_stats_t stats;
            comm_rx_get_stats(&stats);
            return stats.controlQueueDrops + stats.rxQueueDrops + stats.responseDrops;
        }
        case HEALTH_TASK_INPUT_CONTROL: {
            input_control_task_stats_t stats;
            input_control_task_get_stats(&stats);
            return stats.txQueueDrops + stats.safetyQueueDrops;
        }
        case HEALTH_TASK_SORTING_CONTROL: {
            sorting_control_task_stats_t stats;
            sorting_control_task_get_stats(&stats);
            return stats.txQueueDrops + stats.safetyQueueDrops;
        }
        case HEALTH_TASK_SAFETY: {
            safety_task_stats_t stats;
            Safety_GetStats(&stats);
            return stats.reportDrops;
        }
        case HEALTH_TASK_COMM_TX: {
            const comm_tx_stats_t* stats = CommTx_GetStats();
            return stats->dropped_queue_full + stats->dropped_urgent_queue_full + stats->dropped_ring_full +
                   stats->dropped_retry_exhausted + stats->dropped_invalid + stats->dropped_encode_error;
        }
        case HEALTH_TASK_SENSOR:
        default:
            /* SensorTask는 자체 큐가 없다 - CommTxTask 쪽 드랍으로 이미 집계됨. */
            return 0U;
    }
}

static void health_check_task_alive(uint32_t now) {
    health_task_id_t id;

    for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
        uint32_t current = atomic_load_explicit(&healthAliveCounters[id], memory_order_relaxed);

        if (current != healthLastSeenCounters[id]) {
            healthLastSeenCounters[id] = current;
            healthLastChangeTick[id] = now;
            continue;
        }

        if ((healthTaskHangLatched[id] == 0U) && ((now - healthLastChangeTick[id]) >= HEALTH_TASK_STALE_MS)) {
            healthTaskHangLatched[id] = 1U;
            healthStats.taskHangEvents[id]++;
            health_trigger_fatal(HEALTH_FATAL_TASK_HANG, id);
        }
    }
}

static void health_check_stack_margin(void) {
    health_task_id_t id;

    for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
        uint32_t hwm = HealthHw_GetStackHighWaterMark(id);

        if ((hwm != 0xFFFFFFFFU) && (hwm < HEALTH_STACK_MARGIN_WORDS) && (healthStackMarginLatched[id] == 0U)) {
            healthStackMarginLatched[id] = 1U;
            healthStats.stackMarginEvents[id]++;
            health_trigger_fatal(HEALTH_FATAL_STACK_MARGIN, id);
        }
    }
}

static void health_check_queue_overflow(void) {
    health_task_id_t id;

    for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
        uint32_t current = health_task_drop_count(id);

        if (current != healthQueueDropSnapshot[id]) {
            healthQueueDropSnapshot[id] = current;
            healthQueueOverflowStreak[id]++;

            if (healthQueueOverflowStreak[id] >= HEALTH_QUEUE_OVERFLOW_SUSTAIN_CYCLES) {
                if (healthQueueOverflowFatalLatched == 0U) {
                    healthQueueOverflowFatalLatched = 1U;
                    healthStats.queueOverflowFatal++;
                    health_trigger_fatal(HEALTH_FATAL_QUEUE_OVERFLOW, id);
                }
            } else {
                healthStats.queueOverflowTransient++;
                health_report_event(HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT, health_report_cause(id));
            }
        } else {
            healthQueueOverflowStreak[id] = 0U;
        }
    }
}

static void health_check_uart_channels(uint32_t now) {
    uint32_t lastRxTick;
    uint8_t channel;

    for (channel = 0U; channel < (uint8_t)COMM_TX_CH_COUNT; channel++) {
        lastRxTick = comm_rx_get_channel_last_rx_tick((channel == (uint8_t)COMM_TX_CH_INPUT) ? APP_UART_CHANNEL_1
                                                                                             : APP_UART_CHANNEL_6);

        if ((now - lastRxTick) >= HEALTH_UART_CHANNEL_TIMEOUT_MS) {
            if (healthUartLatch[channel].reported == 0U) {
                healthUartLatch[channel].reported = 1U;
                healthStats.uartChannelTimeouts[channel]++;
                health_report_event(HEALTH_ISSUE_UART_CHANNEL_TIMEOUT, channel);
            }
        } else {
            healthUartLatch[channel].reported = 0U;
        }
    }
}

static void health_check_sensor_staleness(uint32_t now) {
    uint8_t count = SensorTask_GetChannelCount();
    uint8_t i;

    if (count > HEALTH_SENSOR_MAX_CHANNELS) {
        count = HEALTH_SENSOR_MAX_CHANNELS;
    }

    for (i = 0U; i < count; i++) {
        uint32_t lastSampleTick = SensorTask_GetChannelLastSampleTick(i);

        if ((now - lastSampleTick) >= HEALTH_SENSOR_STALE_MS) {
            if (healthSensorLatch[i].reported == 0U) {
                healthSensorLatch[i].reported = 1U;
                healthStats.sensorStaleEvents++;
                health_report_event(HEALTH_ISSUE_SENSOR_STALE, (uint8_t)SensorTask_GetChannelProcess(i));
            }
        } else {
            healthSensorLatch[i].reported = 0U;
        }
    }
}

/*
 * 치명(FATAL)으로 승격된 조건이 하나도 없을 때만 갱신한다(어느 태스크의 hang이든
 * -SafetyTask 자신 포함- health_trigger_fatal을 거치므로 healthAnyFatalLatched
 * 하나로 전부 걸러진다). 비치명(UART 채널 단절/센서 지연/일시적 Queue drop)은
 * 갱신을 막지 않는다 - 시스템은 정상 동작 중이므로.
 */
static uint8_t health_watchdog_gate_ok(void) {
    return (healthAnyFatalLatched == 0U) ? 1U : 0U;
}

void HealthTask_Init(void) {
    memset(healthLastSeenCounters, 0, sizeof(healthLastSeenCounters));
    memset(healthLastChangeTick, 0, sizeof(healthLastChangeTick));
    memset(healthTaskHangLatched, 0, sizeof(healthTaskHangLatched));
    memset(healthStackMarginLatched, 0, sizeof(healthStackMarginLatched));
    memset(healthQueueDropSnapshot, 0, sizeof(healthQueueDropSnapshot));
    memset(healthQueueOverflowStreak, 0, sizeof(healthQueueOverflowStreak));
    memset(healthUartLatch, 0, sizeof(healthUartLatch));
    memset(healthSensorLatch, 0, sizeof(healthSensorLatch));
    memset(&healthStats, 0, sizeof(healthStats));
    healthQueueOverflowFatalLatched = 0U;
    healthAnyFatalLatched = 0U;

    {
        health_task_id_t id;
        for (id = (health_task_id_t)0; id < HEALTH_TASK_MONITORED_COUNT; id++) {
            atomic_store_explicit(&healthAliveCounters[id], 0U, memory_order_relaxed);
        }
    }

    healthResetCause = HealthHw_CaptureResetCause();
    HealthHw_IwdgStart(HEALTH_IWDG_PRESCALER_REG, HEALTH_IWDG_RELOAD);
}

void HealthTask_RunCycle(void) {
    uint32_t now = HAL_GetTick();

    health_check_task_alive(now);
    health_check_stack_margin();
    health_check_queue_overflow();
    health_check_uart_channels(now);
    health_check_sensor_staleness(now);

    if (health_watchdog_gate_ok() != 0U) {
        HealthHw_IwdgRefresh();
        healthStats.watchdogRefreshes++;
    } else {
        healthStats.watchdogRefreshSkips++;
    }
}

health_reset_cause_t Health_GetResetCause(void) {
    return healthResetCause;
}

void Health_GetLastPersistedRecord(health_persisted_record_t* record) {
    const health_persisted_record_t* persisted;
    uint32_t expectedChecksum;

    if (record == NULL) {
        return;
    }

    persisted = HealthHw_GetPersistedRecord();
    expectedChecksum = health_checksum(persisted->reason, persisted->offendingTask, persisted->tick);

    if ((persisted->magic == HEALTH_PERSISTED_MAGIC) && (persisted->checksum == expectedChecksum)) {
        *record = *persisted;
    } else {
        memset(record, 0, sizeof(*record));
        record->reason = (uint32_t)HEALTH_FATAL_NONE;
        record->offendingTask = (uint32_t)HEALTH_TASK_MONITORED_COUNT;
    }
}

void Health_GetStats(health_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = healthStats;
    }
}

/*
 * freertos.c의 __weak StartHealthTask를 덮는 실제 구현.
 */
void StartHealthTask(void* argument) {
    (void)argument;

    HealthTask_Init();

    for (;;) {
        HealthTask_RunCycle();
        osDelay(HEALTH_CYCLE_MS);
    }
}
