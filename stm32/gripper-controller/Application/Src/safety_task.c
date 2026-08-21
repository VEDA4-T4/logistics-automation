#include "safety_task.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_messages.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "gripper_control_task.h"
#include "main.h"

#define SAFETY_QUEUE_WAIT_MS 10U

typedef enum { SAFETY_REQUEST_NONE = 0U, SAFETY_REQUEST_STOP, SAFETY_REQUEST_RELEASE } safety_request_t;

static atomic_uint_fast32_t safetyEpoch = ATOMIC_VAR_INIT(0U);
static atomic_uint_fast8_t safetyLatched = ATOMIC_VAR_INIT(0U);
static atomic_uint_fast8_t safetyPendingRequest = ATOMIC_VAR_INIT(SAFETY_REQUEST_NONE);
static safety_task_stats_t safetyStats;
static uint8_t safetyReleasePending;
static uint8_t safetyReleaseCause;
static uint8_t safetyEventPending;
static uint8_t safetyEventLatched;
static uint8_t safetyEventCause;

uint32_t safety_task_get_epoch(void) {
    return (uint32_t)atomic_load_explicit(&safetyEpoch, memory_order_acquire);
}

uint8_t safety_task_is_latched(void) {
    return (uint8_t)atomic_load_explicit(&safetyLatched, memory_order_acquire);
}

static void safety_task_service_event(void) {
    uint8_t payload[3];

    if (safetyEventPending == 0U) {
        return;
    }

    payload[0] = APP_EVENT_SAFETY;
    payload[1] = safetyEventLatched;
    payload[2] = safetyEventCause;
    if (CommTx_SendUrgent(UART_CMD_EVENT, payload, sizeof(payload)) == 0) {
        safetyEventPending = 0U;
    }
}

static void safety_task_emit_state(uint8_t latched, uint8_t cause) {
    CommTx_SetDeviceStatus((latched != 0U) ? UART_DEVICE_EMERGENCY_STOP : UART_DEVICE_STOPPED,
                           (latched != 0U) ? UART_ERROR_EMERGENCY_STOP : UART_ERROR_NONE);
    safetyEventLatched = latched;
    safetyEventCause = cause;
    safetyEventPending = 1U;
    safety_task_service_event();
}

static void safety_task_enter_estop(uint8_t cause) {
    const uint8_t already_latched = (uint8_t)atomic_exchange_explicit(&safetyLatched, 1U, memory_order_acq_rel);

    if ((already_latched != 0U) && (safetyReleasePending == 0U)) {
        safety_task_emit_state(1U, cause);
        return;
    }

    safetyReleasePending = 0U;
    (void)atomic_fetch_add_explicit(&safetyEpoch, 1U, memory_order_acq_rel);
    safetyStats.emergency_stops++;
    if (gripper_control_task_notify_safety_stop() == 0U) {
        safetyStats.notifications_failed++;
    }
    safety_task_emit_state(1U, cause);
}

static void safety_task_release(uint8_t cause) {
    if (safetyReleasePending != 0U) {
        return;
    }

    if (atomic_load_explicit(&safetyLatched, memory_order_acquire) == 0U) {
        safety_task_emit_state(0U, cause);
        return;
    }

    (void)atomic_fetch_add_explicit(&safetyEpoch, 1U, memory_order_acq_rel);
    safetyReleasePending = 1U;
    safetyReleaseCause = cause;
    if (gripper_control_task_notify_safety_release() == 0U) {
        safetyStats.notifications_failed++;
    }
}

static void safety_task_service_release(void) {
    gripper_control_snapshot_t snapshot;

    if (safetyReleasePending == 0U) {
        return;
    }

    gripper_control_task_get_snapshot(&snapshot);
    if ((snapshot.safety_latched != 0U) || (snapshot.state != UART_GRIPPER_STATE_STOPPED)) {
        return;
    }

    safetyReleasePending = 0U;
    atomic_store_explicit(&safetyLatched, 0U, memory_order_release);
    safetyStats.releases++;
    safety_task_emit_state(0U, safetyReleaseCause);
}

uint8_t safety_task_request_local_estop(void) {
    control_command_t message;

    atomic_store_explicit(&safetyPendingRequest, SAFETY_REQUEST_STOP, memory_order_release);
    memset(&message, 0, sizeof(message));
    message.kind = APP_CONTROL_MESSAGE_SAFETY_STOP;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = UART_CMD_EMERGENCY_STOP;
    message.safety_epoch = safety_task_get_epoch();

    if (safetyCommandQueueHandle == NULL || osMessageQueuePut(safetyCommandQueueHandle, &message, 3U, 0U) != osOK) {
        safetyStats.queue_drops++;
        return 0U;
    }
    return 1U;
}

void safety_task_get_stats(safety_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = safetyStats;
    }
}

void SafetyTask_Init(void) {
    memset(&safetyStats, 0, sizeof(safetyStats));
    atomic_store_explicit(&safetyEpoch, 0U, memory_order_release);
    atomic_store_explicit(&safetyLatched, 0U, memory_order_release);
    safetyReleasePending = 0U;
    safetyReleaseCause = 0U;
    safetyEventPending = 0U;
    safetyEventLatched = 0U;
    safetyEventCause = 0U;
}

void SafetyTask_HandleSafetyCommand(const control_command_t* message) {
    if (message == NULL) {
        return;
    }

    if (message->kind == APP_CONTROL_MESSAGE_SAFETY_STOP) {
        safety_task_enter_estop(message->frame.command);
    } else if (message->kind == APP_CONTROL_MESSAGE_SAFETY_RELEASE) {
        safety_task_release(message->frame.command);
    }
}

void SafetyTask_ServicePending(void) {
    const uint_fast8_t pending =
        atomic_exchange_explicit(&safetyPendingRequest, SAFETY_REQUEST_NONE, memory_order_acq_rel);

    if (pending == SAFETY_REQUEST_STOP) {
        safety_task_enter_estop(UART_CMD_EMERGENCY_STOP);
    } else if (pending == SAFETY_REQUEST_RELEASE) {
        safety_task_release(UART_CMD_RESET_DEVICE);
    }

    safety_task_service_release();
    safety_task_service_event();
}

void StartSafetyTask(void* argument) {
    control_command_t message;

    (void)argument;
    SafetyTask_Init();
    for (;;) {
        if ((safetyCommandQueueHandle != NULL) &&
            (osMessageQueueGet(safetyCommandQueueHandle, &message, NULL, SAFETY_QUEUE_WAIT_MS) == osOK)) {
            SafetyTask_HandleSafetyCommand(&message);
        }

        SafetyTask_ServicePending();
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == ESTOP_TEST_BUTTON_Pin) {
        (void)safety_task_request_local_estop();
    }
}
