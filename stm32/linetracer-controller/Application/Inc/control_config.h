#ifndef CONTROL_CONFIG_H
#define CONTROL_CONFIG_H

#include "app_timing.h"

#define CONTROL_TASK_MAX_COMMANDS_PER_CYCLE 4U
#define CONTROL_TASK_ALIVE_INTERVAL_MS APP_TIMING_HEALTH_PERIOD_MS

#if CONTROL_TASK_MAX_COMMANDS_PER_CYCLE == 0U
#error "ControlTask must process at least one command per cycle"
#endif

#endif /* CONTROL_CONFIG_H */
