#ifndef SORTING_CONTROL_TASK_H
#define SORTING_CONTROL_TASK_H

#include <stdint.h>

#include "app_messages.h"
#include "sorting_control.h"

/* 1 ms task backoff 기준 약 100 ms 동안 CommTx urgent queue 재등록을 시도한다. */
#define SORTING_CONTROL_TX_RETRY_LIMIT 100U

typedef enum {
    SORTING_CONTROL_SAFETY_RELEASED = 0,
    SORTING_CONTROL_SAFETY_STOP_REQUESTED,
    SORTING_CONTROL_SAFETY_STOPPED,
    SORTING_CONTROL_SAFETY_RELEASE_REQUESTED
} sorting_control_safety_sync_state_t;

typedef struct {
    uint32_t txQueueDrops;
    uint32_t txRetryAttempts;
    uint32_t txRetryExhausted;
    uint32_t txPendingOverruns;
    uint32_t duplicateCommands;
    uint32_t sequenceConflicts;
    uint32_t safetyQueueDrops;
    uint32_t cycleCompleteEvents;
} sorting_control_task_stats_t;

void StartSortingControlTask(void* argument);

sorting_control_result_t sorting_control_task_initialize_controller(sorting_control_t* controller,
                                                                    const sorting_motor_port_t* motor,
                                                                    const sorting_gate_port_t* gate);

sorting_control_result_t sorting_control_task_process_message(sorting_control_t* controller,
                                                              const control_command_t* message);

sorting_control_result_t sorting_control_task_service_motion(sorting_control_t* controller);

uint8_t sorting_control_task_service_tx(void);

/* SafetyTask latches the shared STBY low before requesting this stop. */
uint8_t sorting_control_task_notify_safety_stop(void);

/* Release is accepted only after the motor is stopped and the gate is Home. */
uint8_t sorting_control_task_notify_safety_release(void);

sorting_control_safety_sync_state_t sorting_control_task_get_safety_sync_state(void);

/* CommRxTask stamps each sorting command with this ingress safety epoch. */
uint32_t sorting_control_task_capture_command_epoch(void);

void sorting_control_task_get_stats(sorting_control_task_stats_t* stats);

#endif /* SORTING_CONTROL_TASK_H */
