#ifndef HEALTH_LOGIC_H
#define HEALTH_LOGIC_H

#include <stdint.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HEALTH_STACK_WATERMARK_UNKNOWN UINT32_MAX

typedef enum {
    HEALTH_FAULT_NONE = 0,
    HEALTH_FAULT_TASK_STALLED,
    HEALTH_FAULT_STACK_LOW,
    HEALTH_FAULT_QUEUE_OVERFLOW,
    HEALTH_FAULT_UART_RX_TIMEOUT,
    HEALTH_FAULT_UART_TX_TIMEOUT,
    HEALTH_FAULT_INTERNAL_ERROR,
    HEALTH_FAULT_COUNT
} health_fault_reason_t;

typedef struct {
    health_fault_reason_t reason;
    app_task_id_t source_task;
    uint32_t occurred_at_ms;
    uint32_t detail;
    uint8_t error_code;
    uint8_t watchdog_blocking;
} health_fault_record_t;

typedef struct {
    uint32_t required_task_mask;
    uint32_t started_at_ms;
    uint32_t last_alive_ms[APP_TASK_COUNT];
    uint32_t last_alive_detail[APP_TASK_COUNT];
    uint32_t stack_high_water_words[APP_TASK_COUNT];
    uint32_t event_fault_latch[APP_TASK_COUNT];
    uint32_t last_event_ms[APP_TASK_COUNT][APP_HEALTH_EVENT_COUNT];
    uint32_t seen_alive_mask;
    uint32_t stalled_task_mask;
    uint32_t reported_stalled_mask;
    uint32_t stack_low_task_mask;
    uint32_t reported_stack_low_mask;
    uint8_t critical_fault_latched;
} health_logic_context_t;

void HealthLogic_Init(health_logic_context_t* context, uint32_t required_task_mask, uint32_t now_ms);

/*
 * Records TASK_ALIVE or translates an anomaly event into a fault. Repeated
 * anomaly types from the same source are reported once per boot.
 */
uint8_t HealthLogic_HandleEvent(health_logic_context_t* context, const app_health_event_t* event,
                                health_fault_record_t* fault);

void HealthLogic_UpdateStack(health_logic_context_t* context, app_task_id_t task, uint32_t high_water_words);

/*
 * Evaluates liveness and stack margins. One newly detected fault is returned
 * per call; call repeatedly to drain simultaneous faults.
 */
uint8_t HealthLogic_Evaluate(health_logic_context_t* context, uint32_t now_ms, uint32_t startup_grace_ms,
                             uint32_t alive_timeout_ms, uint32_t stack_min_words, health_fault_record_t* fault);

/*
 * Queue pressure and a TX failure are transient when they stop recurring.
 * RX timeout is cleared only by the explicit RX_RECOVERED event.
 */
uint32_t HealthLogic_ClearExpiredTransientFaults(health_logic_context_t* context, uint32_t now_ms,
                                                 uint32_t clear_timeout_ms);

uint8_t HealthLogic_HasActiveFaults(const health_logic_context_t* context);

uint8_t HealthLogic_WatchdogAllowed(const health_logic_context_t* context, uint32_t now_ms, uint32_t startup_grace_ms,
                                    uint32_t alive_timeout_ms, uint32_t stack_min_words);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_LOGIC_H */
