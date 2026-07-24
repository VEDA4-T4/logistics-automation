#include "health_task.h"

#include <stddef.h>
#include <string.h>

#include "app_queues.h"
#include "cmsis_os2.h"
#include "health_config.h"
#include "main.h"

static health_logic_context_t s_health_logic;
static health_task_stats_t s_health_stats;
static health_fault_record_t s_last_fault;
static health_persisted_record_t s_persisted_record;
static health_reset_cause_t s_reset_cause;
static volatile health_task_snapshot_t s_health_snapshot;
static volatile uint8_t s_health_snapshot_valid;
static uint8_t s_persisted_fault_report_pending;

static uint32_t HealthTask_EnterShortCriticalSection(void) {
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void HealthTask_ExitShortCriticalSection(uint32_t primask) {
    if (primask == 0U) {
        __enable_irq();
    }
}

static void HealthTask_UpdateSnapshot(uint32_t now_ms, uint8_t watchdog_allowed) {
    health_task_snapshot_t snapshot;
    uint32_t task;
    uint32_t primask;

    (void)memset(&snapshot, 0, sizeof(snapshot));
    snapshot.updated_at_ms = now_ms;
    snapshot.required_task_mask = s_health_logic.required_task_mask;
    snapshot.seen_alive_mask = s_health_logic.seen_alive_mask;
    snapshot.stalled_task_mask = s_health_logic.stalled_task_mask;
    snapshot.stack_low_task_mask = s_health_logic.stack_low_task_mask;
    for (task = 0U; task < (uint32_t)APP_TASK_COUNT; ++task) {
        snapshot.last_alive_ms[task] = s_health_logic.last_alive_ms[task];
        snapshot.stack_high_water_words[task] = s_health_logic.stack_high_water_words[task];
    }
    snapshot.last_fault = s_last_fault;
    snapshot.persisted_record = s_persisted_record;
    snapshot.reset_cause = s_reset_cause;
    snapshot.stats = s_health_stats;
    snapshot.watchdog_allowed = watchdog_allowed;

    primask = HealthTask_EnterShortCriticalSection();
    s_health_snapshot = snapshot;
    s_health_snapshot_valid = 1U;
    HealthTask_ExitShortCriticalSection(primask);
}

uint8_t HealthTask_GetLatest(health_task_snapshot_t* snapshot) {
    uint32_t primask;

    if (snapshot == NULL) {
        return 0U;
    }

    primask = HealthTask_EnterShortCriticalSection();
    if (s_health_snapshot_valid == 0U) {
        HealthTask_ExitShortCriticalSection(primask);
        return 0U;
    }
    *snapshot = s_health_snapshot;
    HealthTask_ExitShortCriticalSection(primask);
    return 1U;
}

static uint8_t HealthTask_PublishSafetyFault(const health_fault_record_t* fault) {
    app_safety_event_t event = { 0 };

    if ((fault == NULL) || (safetyEventQueue == NULL)) {
        return 0U;
    }

    event.type = APP_SAFETY_EVENT_HEALTH_FAULT;
    event.occurred_at_ms = fault->occurred_at_ms;
    event.reason = LINETRACER_STOP_REASON_HEALTH_FAULT;
    event.source_task = fault->source_task;
    event.error_code = fault->error_code;
    event.active = 1U;
    return (osMessageQueuePut(safetyEventQueue, &event, 0U, 0U) == osOK) ? 1U : 0U;
}

static void HealthTask_ReportFault(const health_fault_record_t* fault, uint8_t persist) {
    if (fault == NULL) {
        return;
    }

    s_last_fault = *fault;
    if (persist != 0U) {
        HealthHw_StoreFault(fault);
        s_persisted_record.fault = *fault;
        s_persisted_record.reset_cause = s_reset_cause;
        s_persisted_record.valid = 1U;
    }

    if (HealthTask_PublishSafetyFault(fault) != 0U) {
        ++s_health_stats.faults_reported;
    } else {
        ++s_health_stats.safety_report_drops;
    }
}

static void HealthTask_DrainEvents(void) {
    app_health_event_t event;
    uint32_t processed = 0U;

    while ((processed < HEALTH_MAX_EVENTS_PER_CYCLE) &&
           (osMessageQueueGet(healthEventQueue, &event, NULL, 0U) == osOK)) {
        health_fault_record_t fault;

        ++processed;
        ++s_health_stats.events_processed;
        if (HealthLogic_HandleEvent(&s_health_logic, &event, &fault) != 0U) {
            HealthTask_ReportFault(&fault, 1U);
        }
    }
}

static void HealthTask_SampleStacks(void) {
    uint32_t task;

    for (task = 0U; task < (uint32_t)APP_TASK_COUNT; ++task) {
        if ((HEALTH_REQUIRED_TASK_MASK & HEALTH_TASK_MASK(task)) == 0U) {
            continue;
        }

        HealthLogic_UpdateStack(&s_health_logic, (app_task_id_t)task,
                                HealthHw_GetStackHighWaterMark((app_task_id_t)task));
    }
}

static void HealthTask_Evaluate(uint32_t now_ms) {
    uint32_t report_count;

    for (report_count = 0U; report_count < ((uint32_t)APP_TASK_COUNT * 2U); ++report_count) {
        health_fault_record_t fault;

        if (HealthLogic_Evaluate(&s_health_logic, now_ms, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                 HEALTH_STACK_MIN_WORDS, &fault) == 0U) {
            break;
        }
        HealthTask_ReportFault(&fault, 1U);
    }
}

static void HealthTask_Initialize(uint32_t now_ms) {
    (void)memset(&s_health_stats, 0, sizeof(s_health_stats));
    (void)memset(&s_last_fault, 0, sizeof(s_last_fault));
    (void)memset(&s_persisted_record, 0, sizeof(s_persisted_record));
    (void)memset((void*)&s_health_snapshot, 0, sizeof(s_health_snapshot));
    s_health_snapshot_valid = 0U;

    HealthLogic_Init(&s_health_logic, HEALTH_REQUIRED_TASK_MASK, now_ms);
    s_reset_cause = HealthHw_CaptureResetCause();
    s_persisted_fault_report_pending = HealthHw_LoadPersistedRecord(&s_persisted_record);
    HealthHw_StartWatchdog(HEALTH_IWDG_PRESCALER_REG, HEALTH_IWDG_RELOAD);
}

void StartHealthTask(void* argument) {
    uint32_t next_wake_ms;

    (void)argument;
    next_wake_ms = osKernelGetTickCount();
    HealthTask_Initialize(next_wake_ms);

    for (;;) {
        uint32_t now_ms = osKernelGetTickCount();
        uint8_t watchdog_allowed;

        if ((s_persisted_fault_report_pending != 0U) && (s_persisted_record.valid != 0U)) {
            HealthTask_ReportFault(&s_persisted_record.fault, 0U);
            s_persisted_fault_report_pending = 0U;
        }

        HealthTask_DrainEvents();
        HealthTask_SampleStacks();
        HealthTask_Evaluate(now_ms);

        watchdog_allowed = HealthLogic_WatchdogAllowed(&s_health_logic, now_ms, HEALTH_STARTUP_GRACE_MS,
                                                       HEALTH_ALIVE_TIMEOUT_MS, HEALTH_STACK_MIN_WORDS);
        if (watchdog_allowed != 0U) {
            HealthHw_RefreshWatchdog();
            ++s_health_stats.watchdog_refreshes;
        } else {
            ++s_health_stats.watchdog_refresh_skips;
        }

        ++s_health_stats.cycles;
        HealthTask_UpdateSnapshot(now_ms, watchdog_allowed);

        next_wake_ms += HEALTH_MONITOR_PERIOD_MS;
        if (osDelayUntil(next_wake_ms) != osOK) {
            next_wake_ms = osKernelGetTickCount();
        }
    }
}
