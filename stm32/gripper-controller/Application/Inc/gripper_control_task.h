#ifndef GRIPPER_CONTROL_TASK_H
#define GRIPPER_CONTROL_TASK_H

#include <stdint.h>

#include "gripper_control.h"

typedef struct {
    uint32_t commands;
    uint32_t duplicate_commands;
    uint32_t sequence_conflicts;
    uint32_t stale_commands;
    uint32_t busy_commands;
    uint32_t response_retries;
    uint32_t response_drops;
    uint32_t motion_completions;
    uint32_t servo_faults;
    uint32_t safety_queue_drops;
} gripper_control_task_stats_t;

void StartGripperControlTask(void* argument);
uint8_t gripper_control_task_notify_safety_stop(void);
uint8_t gripper_control_task_notify_safety_release(void);
void gripper_control_task_get_snapshot(gripper_control_snapshot_t* snapshot);
void gripper_control_task_get_stats(gripper_control_task_stats_t* stats);

#endif /* GRIPPER_CONTROL_TASK_H */
