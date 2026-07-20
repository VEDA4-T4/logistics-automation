#ifndef INPUT_CONTROL_TASK_H
#define INPUT_CONTROL_TASK_H

#include <stdint.h>

#include "app_messages.h"
#include "input_control.h"

typedef enum {
    INPUT_CONTROL_SAFETY_RELEASED = 0,
    INPUT_CONTROL_SAFETY_STOP_REQUESTED,
    INPUT_CONTROL_SAFETY_STOPPED,
    INPUT_CONTROL_SAFETY_RELEASE_REQUESTED
} input_control_safety_sync_state_t;

typedef struct {
    uint32_t txQueueDrops;
    uint32_t txRetryAttempts;
    uint32_t txPendingOverruns;
    uint32_t duplicateCommands;
    uint32_t sequenceConflicts;
    uint32_t safetyQueueDrops;
} input_control_task_stats_t;

void StartInputControlTask(void* argument);

input_control_result_t input_control_task_initialize_controller(input_control_t* controller,
                                                                const input_motor_port_t* motor);

input_control_result_t input_control_task_process_message(input_control_t* controller,
                                                          const control_command_t* message);

/* Pending 응답을 CommTxTask에 다시 등록한다. 1이면 pending 응답이 없다. */
uint8_t input_control_task_service_tx(void);

/*
 * SafetyTask must latch the shared motor STBY low before requesting this stop.
 * STOPPED is published only after this task confirms the channel stop output;
 * failures remain STOP_REQUESTED, reject release, and are retried by the task.
 */
uint8_t input_control_task_notify_safety_stop(void);

/*
 * Release is accepted only after STOPPED synchronization. SafetyTask requests
 * release from every control task while the shared STBY latch remains LOW,
 * waits until every task reports RELEASED, and only then releases the latch.
 */
uint8_t input_control_task_notify_safety_release(void);

input_control_safety_sync_state_t input_control_task_get_safety_sync_state(void);

/* CommRxTask stamps each input command with this ingress safety epoch. */
uint32_t input_control_task_capture_command_epoch(void);

void input_control_task_get_stats(input_control_task_stats_t* stats);

#endif /* INPUT_CONTROL_TASK_H */
