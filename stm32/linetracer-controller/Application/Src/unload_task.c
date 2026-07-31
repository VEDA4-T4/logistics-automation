#include "unload_task.h"

#include <stdbool.h>

#include "app_queues.h"
#include "app_timing.h"
#include "cmsis_os2.h"
#include "sensor_task.h"
#include "unload_config.h"
#include "unload_hw.h"
#include "unload_logic.h"

extern osThreadId_t UnloadTaskHandle;

static unload_logic_context_t unloadTaskContext;
static volatile uint8_t unloadTaskSafetyStopRequested;
static volatile uint8_t unloadTaskAbortStopRequested;
static uint8_t unloadTaskResultQueueFaultReported;

static void UnloadTask_PublishHealthEvent(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_UNLOAD;
    (void)AppQueues_TryPutHealth(&event);
}

static void UnloadTask_PublishResult(void) {
    app_unload_result_t result;
    uint32_t now_ms;

    if (UnloadLogic_GetPendingResult(&unloadTaskContext, &result) == 0U) {
        return;
    }

    now_ms = osKernelGetTickCount();
    if (unloadResultQueue == NULL || osMessageQueuePut(unloadResultQueue, &result, 0U, 0U) != osOK) {
        if (unloadTaskResultQueueFaultReported == 0U) {
            UnloadTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)result.type, now_ms);
            unloadTaskResultQueueFaultReported = 1U;
        }
        return;
    }

    unloadTaskResultQueueFaultReported = 0U;
    UnloadLogic_AcknowledgeResult(&unloadTaskContext);
}

static void UnloadTask_HandleCommand(const app_unload_command_t* command, uint32_t now_ms) {
    if (command == NULL) {
        return;
    }

    switch (command->type) {
        case APP_UNLOAD_COMMAND_START:
            if (unloadTaskSafetyStopRequested != 0U || unloadTaskAbortStopRequested != 0U ||
                UnloadHw_IsSafetyInhibited() != 0U) {
                return;
            }
            if (UnloadLogic_Start(&unloadTaskContext, command, now_ms) == 0U) {
                UnloadTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR, (uint32_t)command->type, now_ms);
            }
            break;

        case APP_UNLOAD_COMMAND_ABORT:
            unloadTaskAbortStopRequested = 1U;
            if (UnloadHw_IsSafetyInhibited() == 0U) {
                UnloadHw_SetSafetyInhibit(1U);
            }
            UnloadLogic_Abort(&unloadTaskContext, now_ms, UART_ERROR_BUSY);
            break;

        case APP_UNLOAD_COMMAND_RESET:
            /*
             * Clear task-local requests before releasing the generation gate.
             * A newer stop racing with RESET will set a new latch/generation,
             * so this release fails or leaves that new latch asserted.
             */
            unloadTaskSafetyStopRequested = 0U;
            unloadTaskAbortStopRequested = 0U;
            if (UnloadHw_ReleaseSafetyInhibit(command->inhibit_generation) == 0U) {
                unloadTaskSafetyStopRequested = 1U;
                break;
            }
            unloadTaskResultQueueFaultReported = 0U;
            UnloadLogic_Reset(&unloadTaskContext, now_ms);
            break;

        case APP_UNLOAD_COMMAND_NONE:
        default:
            break;
    }
}

static void UnloadTask_ProcessCommands(uint32_t now_ms) {
    app_unload_command_t command;
    uint32_t processed = 0U;

    if (unloadCommandQueue == NULL) {
        return;
    }

    while (processed < APP_UNLOAD_COMMAND_QUEUE_DEPTH &&
           osMessageQueueGet(unloadCommandQueue, &command, NULL, 0U) == osOK) {
        UnloadTask_HandleCommand(&command, now_ms);
        ++processed;
    }
}

static void UnloadTask_ProcessStopRequests(uint32_t now_ms) {
    uint32_t flags;
    uint8_t abort_requested;

    flags = osThreadFlagsWait(APP_UNLOAD_NOTIFY_SAFETY_STOP | APP_UNLOAD_NOTIFY_ABORT, osFlagsWaitAny, 0U);
    abort_requested =
        ((flags & osFlagsError) == 0U && (flags & APP_UNLOAD_NOTIFY_ABORT) != 0U) ? 1U : 0U;
    if ((flags & osFlagsError) == 0U && (flags & APP_UNLOAD_NOTIFY_SAFETY_STOP) != 0U) {
        unloadTaskSafetyStopRequested = 1U;
    }

    if (unloadTaskSafetyStopRequested != 0U) {
        if (UnloadHw_IsSafetyInhibited() == 0U) {
            UnloadHw_SetSafetyInhibit(1U);
        }
        UnloadLogic_Abort(&unloadTaskContext, now_ms, UART_ERROR_EMERGENCY_STOP);
    } else if (abort_requested != 0U) {
        unloadTaskAbortStopRequested = 1U;
        if (UnloadHw_IsSafetyInhibited() == 0U) {
            UnloadHw_SetSafetyInhibit(1U);
        }
        UnloadLogic_Abort(&unloadTaskContext, now_ms, UART_ERROR_BUSY);
    }
}

uint8_t UnloadTask_RequestSafetyStop(void) {
    uint32_t flags;

    unloadTaskSafetyStopRequested = 1U;
    UnloadHw_SetSafetyInhibit(1U);
    if (UnloadTaskHandle == NULL || osKernelGetState() != osKernelRunning) {
        return 0U;
    }

    flags = osThreadFlagsSet(UnloadTaskHandle, APP_UNLOAD_NOTIFY_SAFETY_STOP);
    return ((flags & osFlagsError) == 0U) ? 1U : 0U;
}

uint8_t UnloadTask_RequestAbort(void) {
    uint32_t flags;

    unloadTaskAbortStopRequested = 1U;
    UnloadHw_SetSafetyInhibit(1U);
    if (UnloadTaskHandle == NULL || osKernelGetState() != osKernelRunning) {
        return 0U;
    }

    flags = osThreadFlagsSet(UnloadTaskHandle, APP_UNLOAD_NOTIFY_ABORT);
    return ((flags & osFlagsError) == 0U) ? 1U : 0U;
}

void StartUnloadTask(void* argument) {
    uint32_t next_wake_tick;
    uint32_t last_alive_tick;

    (void)argument;
    unloadTaskResultQueueFaultReported = 0U;
    next_wake_tick = osKernelGetTickCount();
    last_alive_tick = next_wake_tick;
    UnloadLogic_Init(&unloadTaskContext, next_wake_tick);
    if (UnloadHw_Init() == 0U) {
        UnloadTask_PublishHealthEvent(APP_HEALTH_EVENT_INTERNAL_ERROR, 1U, next_wake_tick);
    }
    (void)osThreadFlagsClear(APP_UNLOAD_NOTIFY_SAFETY_STOP | APP_UNLOAD_NOTIFY_ABORT);

    for (;;) {
        app_sensor_snapshot_t snapshot;
        uart_linetracer_load_state_t load_state = UART_LINETRACER_LOAD_UNLOADING;
        uint32_t now_ms = osKernelGetTickCount();

        UnloadTask_ProcessStopRequests(now_ms);
        UnloadTask_ProcessCommands(now_ms);
        if (SensorTask_GetLatest(&snapshot) &&
            uart_linetracer_load_state_is_valid(snapshot.load_state) != 0U &&
            (uint32_t)(now_ms - snapshot.sampled_at_ms) <= UNLOAD_SENSOR_SNAPSHOT_MAX_AGE_MS) {
            load_state = snapshot.load_state;
        }

        if (unloadTaskSafetyStopRequested == 0U && unloadTaskAbortStopRequested == 0U &&
            UnloadHw_IsSafetyInhibited() == 0U) {
            UnloadLogic_Update(&unloadTaskContext, load_state, now_ms);
            if (UnloadHw_Apply(UnloadLogic_GetServoOutput(&unloadTaskContext)) == 0U) {
                UnloadLogic_Fail(&unloadTaskContext, now_ms, UART_ERROR_SERVO);
            }
        }
        UnloadTask_PublishResult();

        if ((uint32_t)(now_ms - last_alive_tick) >= APP_TIMING_HEALTH_PERIOD_MS) {
            UnloadTask_PublishHealthEvent(APP_HEALTH_EVENT_TASK_ALIVE, (uint32_t)unloadTaskContext.state, now_ms);
            last_alive_tick = now_ms;
        }

        next_wake_tick += APP_TIMING_UNLOAD_STEP_MS;
        if (osDelayUntil(next_wake_tick) != osOK) {
            next_wake_tick = osKernelGetTickCount();
        }
    }
}
