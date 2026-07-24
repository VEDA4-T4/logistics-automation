#ifndef HEALTH_CONFIG_H
#define HEALTH_CONFIG_H

#include <stdint.h>

#include "app_messages.h"

/*
 * Producers publish APP_HEALTH_EVENT_TASK_ALIVE every 500 ms. HealthTask runs
 * faster so queue draining and fault propagation never block driving control.
 */
#define HEALTH_MONITOR_PERIOD_MS 100U
#define HEALTH_ALIVE_TIMEOUT_MS 1500U
#define HEALTH_STARTUP_GRACE_MS 2000U
#define HEALTH_STACK_MIN_WORDS 128U
#define HEALTH_MAX_EVENTS_PER_CYCLE 8U

/*
 * UnloadTask remains outside the required mask until its real implementation
 * replaces the generated weak loop. Add APP_TASK_UNLOAD here at that time.
 */
#define HEALTH_TASK_MASK(task_id) (1UL << (uint32_t)(task_id))
#define HEALTH_REQUIRED_TASK_MASK                                                                                  \
    (HEALTH_TASK_MASK(APP_TASK_SENSOR) | HEALTH_TASK_MASK(APP_TASK_COMM_RX) | HEALTH_TASK_MASK(APP_TASK_CONTROL) | \
     HEALTH_TASK_MASK(APP_TASK_SAFETY) | HEALTH_TASK_MASK(APP_TASK_COMM_TX))

/*
 * STM32F401 IWDG timeout is approximately 4 seconds:
 * LSI (~32 kHz) / 64 prescaler * (1999 + 1) counts.
 */
#define HEALTH_IWDG_PRESCALER_REG 4U
#define HEALTH_IWDG_RELOAD 1999U

#endif /* HEALTH_CONFIG_H */
