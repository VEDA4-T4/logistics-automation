#ifndef HEALTH_TASK_H
#define HEALTH_TASK_H

#include <stdint.h>

#include "health_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t cycles;
    uint32_t events_processed;
    uint32_t faults_reported;
    uint32_t fault_clears_reported;
    uint32_t safety_report_drops;
    uint32_t persisted_fault_skips;
    uint32_t watchdog_refreshes;
    uint32_t watchdog_refresh_skips;
} health_task_stats_t;

typedef struct {
    uint32_t updated_at_ms;
    uint32_t required_task_mask;
    uint32_t seen_alive_mask;
    uint32_t stalled_task_mask;
    uint32_t stack_low_task_mask;
    uint32_t last_alive_ms[APP_TASK_COUNT];
    uint32_t stack_high_water_words[APP_TASK_COUNT];
    uint32_t active_event_fault_mask[APP_TASK_COUNT];
    health_fault_record_t last_fault;
    health_persisted_record_t persisted_record;
    health_reset_cause_t reset_cause;
    health_task_stats_t stats;
    uint8_t watchdog_allowed;
} health_task_snapshot_t;

void StartHealthTask(void* argument);
uint8_t HealthTask_GetLatest(health_task_snapshot_t* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_TASK_H */
