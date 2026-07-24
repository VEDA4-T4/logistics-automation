#include "health_logic.h"

#include <stddef.h>
#include <string.h>

#define HEALTH_TASK_BIT(task) (1UL << (uint32_t)(task))
#define HEALTH_EVENT_BIT(type) (1UL << (uint32_t)(type))

static uint8_t HealthLogic_TaskIsValid(app_task_id_t task) {
    return ((uint32_t)task < (uint32_t)APP_TASK_COUNT) ? 1U : 0U;
}

static void HealthLogic_FillFault(health_fault_record_t* fault, health_fault_reason_t reason, app_task_id_t source_task,
                                  uint32_t occurred_at_ms, uint32_t detail, uint8_t error_code,
                                  uint8_t watchdog_blocking) {
    (void)memset(fault, 0, sizeof(*fault));
    fault->reason = reason;
    fault->source_task = source_task;
    fault->occurred_at_ms = occurred_at_ms;
    fault->detail = detail;
    fault->error_code = error_code;
    fault->watchdog_blocking = watchdog_blocking;
}

void HealthLogic_Init(health_logic_context_t* context, uint32_t required_task_mask, uint32_t now_ms) {
    uint32_t task;

    if (context == NULL) {
        return;
    }

    (void)memset(context, 0, sizeof(*context));
    context->required_task_mask = required_task_mask;
    context->started_at_ms = now_ms;
    for (task = 0U; task < (uint32_t)APP_TASK_COUNT; ++task) {
        context->stack_high_water_words[task] = HEALTH_STACK_WATERMARK_UNKNOWN;
    }
}

uint8_t HealthLogic_HandleEvent(health_logic_context_t* context, const app_health_event_t* event,
                                health_fault_record_t* fault) {
    uint32_t task_bit;
    uint32_t event_bit;
    health_fault_reason_t reason;
    uint8_t error_code;
    uint8_t watchdog_blocking;

    if ((context == NULL) || (event == NULL) || (fault == NULL) ||
        (HealthLogic_TaskIsValid(event->source_task) == 0U)) {
        return 0U;
    }

    task_bit = HEALTH_TASK_BIT(event->source_task);
    if (event->type == APP_HEALTH_EVENT_TASK_ALIVE) {
        context->last_alive_ms[event->source_task] = event->occurred_at_ms;
        context->last_alive_detail[event->source_task] = event->detail;
        context->seen_alive_mask |= task_bit;
        return 0U;
    }

    reason = HEALTH_FAULT_NONE;
    error_code = (uint8_t)UART_ERROR_NONE;
    watchdog_blocking = 0U;

    switch (event->type) {
        case APP_HEALTH_EVENT_QUEUE_FULL:
            reason = HEALTH_FAULT_QUEUE_OVERFLOW;
            error_code = (uint8_t)UART_ERROR_BUSY;
            break;

        case APP_HEALTH_EVENT_UART_RX_TIMEOUT:
            reason = HEALTH_FAULT_UART_RX_TIMEOUT;
            error_code = (uint8_t)UART_ERROR_TIMEOUT;
            break;

        case APP_HEALTH_EVENT_UART_TX_TIMEOUT:
            reason = HEALTH_FAULT_UART_TX_TIMEOUT;
            error_code = (uint8_t)UART_ERROR_TIMEOUT;
            break;

        case APP_HEALTH_EVENT_INTERNAL_ERROR:
            reason = HEALTH_FAULT_INTERNAL_ERROR;
            error_code = (uint8_t)UART_ERROR_INTERNAL;
            watchdog_blocking = 1U;
            break;

        case APP_HEALTH_EVENT_NONE:
        case APP_HEALTH_EVENT_TASK_ALIVE:
        default:
            return 0U;
    }

    event_bit = HEALTH_EVENT_BIT(event->type);
    if ((context->event_fault_latch[event->source_task] & event_bit) != 0U) {
        return 0U;
    }
    context->event_fault_latch[event->source_task] |= event_bit;
    if (watchdog_blocking != 0U) {
        context->critical_fault_latched = 1U;
    }

    HealthLogic_FillFault(fault, reason, event->source_task, event->occurred_at_ms, event->detail, error_code,
                          watchdog_blocking);
    return 1U;
}

void HealthLogic_UpdateStack(health_logic_context_t* context, app_task_id_t task, uint32_t high_water_words) {
    if ((context == NULL) || (HealthLogic_TaskIsValid(task) == 0U)) {
        return;
    }

    context->stack_high_water_words[task] = high_water_words;
}

uint8_t HealthLogic_Evaluate(health_logic_context_t* context, uint32_t now_ms, uint32_t startup_grace_ms,
                             uint32_t alive_timeout_ms, uint32_t stack_min_words, health_fault_record_t* fault) {
    uint32_t task;

    if ((context == NULL) || (fault == NULL)) {
        return 0U;
    }

    if ((uint32_t)(now_ms - context->started_at_ms) < startup_grace_ms) {
        return 0U;
    }

    for (task = 0U; task < (uint32_t)APP_TASK_COUNT; ++task) {
        uint32_t task_bit = HEALTH_TASK_BIT(task);
        uint32_t elapsed_ms;

        if ((context->required_task_mask & task_bit) == 0U) {
            continue;
        }

        elapsed_ms = ((context->seen_alive_mask & task_bit) != 0U) ? (uint32_t)(now_ms - context->last_alive_ms[task])
                                                                   : (uint32_t)(now_ms - context->started_at_ms);

        if (((context->seen_alive_mask & task_bit) == 0U) || (elapsed_ms >= alive_timeout_ms)) {
            context->stalled_task_mask |= task_bit;
            context->critical_fault_latched = 1U;
            if ((context->reported_stalled_mask & task_bit) == 0U) {
                context->reported_stalled_mask |= task_bit;
                HealthLogic_FillFault(fault, HEALTH_FAULT_TASK_STALLED, (app_task_id_t)task, now_ms, elapsed_ms,
                                      (uint8_t)UART_ERROR_INTERNAL, 1U);
                return 1U;
            }
        }

        if ((context->stack_high_water_words[task] != HEALTH_STACK_WATERMARK_UNKNOWN) &&
            (context->stack_high_water_words[task] < stack_min_words)) {
            context->stack_low_task_mask |= task_bit;
            context->critical_fault_latched = 1U;
            if ((context->reported_stack_low_mask & task_bit) == 0U) {
                context->reported_stack_low_mask |= task_bit;
                HealthLogic_FillFault(fault, HEALTH_FAULT_STACK_LOW, (app_task_id_t)task, now_ms,
                                      context->stack_high_water_words[task], (uint8_t)UART_ERROR_INTERNAL, 1U);
                return 1U;
            }
        }
    }

    return 0U;
}

uint8_t HealthLogic_WatchdogAllowed(const health_logic_context_t* context, uint32_t now_ms, uint32_t startup_grace_ms,
                                    uint32_t alive_timeout_ms, uint32_t stack_min_words) {
    uint32_t task;

    if (context == NULL) {
        return 0U;
    }
    if (context->critical_fault_latched != 0U) {
        return 0U;
    }
    if ((uint32_t)(now_ms - context->started_at_ms) < startup_grace_ms) {
        return 1U;
    }

    for (task = 0U; task < (uint32_t)APP_TASK_COUNT; ++task) {
        uint32_t task_bit = HEALTH_TASK_BIT(task);

        if ((context->required_task_mask & task_bit) == 0U) {
            continue;
        }
        if ((context->seen_alive_mask & task_bit) == 0U) {
            return 0U;
        }
        if ((uint32_t)(now_ms - context->last_alive_ms[task]) >= alive_timeout_ms) {
            return 0U;
        }
        if ((context->stack_high_water_words[task] != HEALTH_STACK_WATERMARK_UNKNOWN) &&
            (context->stack_high_water_words[task] < stack_min_words)) {
            return 0U;
        }
    }

    return 1U;
}
