#ifndef SAFETY_TASK_H
#define SAFETY_TASK_H

#include <stdint.h>

#include "app_messages.h"

typedef struct {
    uint32_t emergency_stops;
    uint32_t releases;
    uint32_t queue_drops;
    uint32_t notifications_failed;
} safety_task_stats_t;

void StartSafetyTask(void* argument);
void SafetyTask_Init(void);
void SafetyTask_HandleSafetyCommand(const control_command_t* message);
void SafetyTask_ServicePending(void);
uint32_t safety_task_get_epoch(void);
uint8_t safety_task_is_latched(void);
uint8_t safety_task_request_local_estop(void);
void safety_task_get_stats(safety_task_stats_t* stats);

#endif /* SAFETY_TASK_H */
