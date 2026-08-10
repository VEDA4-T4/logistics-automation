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

typedef enum {
    SAFETY_REQUEST_NONE = 0U,
    SAFETY_REQUEST_STOP,
    SAFETY_REQUEST_RELEASE
} safety_request_t;

static atomic_uint_fast32_t safetyEpoch = ATOMIC_VAR_INIT(0U);
static atomic_uint_fast8_t safetyLatched = ATOMIC_VAR_INIT(0U);
static atomic_uint_fast8_t safetyPendingRequest = ATOMIC_VAR_INIT(SAFETY_REQUEST_NONE);
static safety_task_stats_t safetyStats;

uint32_t safety_task_get_epoch(void) {
    return (uint32_t)atomic_load_explicit(&safetyEpoch, memory_order_acquire);
}

uint8_t safety_task_is_latched(void) {
    return (uint8_t)atomic_load_explicit(&safetyLatched, memory_order_acquire);
}

static void safety_task_emit_state(uint8_t latched, uint8_t cause) {
    uint8_t payload[3];

    payload[0] = APP_EVENT_SAFETY;
    payload[1] = latched;
    payload[2] = cause;
    (void)CommTx_SendUrgent(UART_CMD_EVENT, payload, sizeof(payload));
    CommTx_SetDeviceStatus((latched != 0U) ? UART_DEVICE_EMERGENCY_STOP : UART_DEVICE_STOPPED,
                           (latched != 0U) ? UART_ERROR_EMERGENCY_STOP : UART_ERROR_NONE);
}

static void safety_task_enter_estop(uint8_t cause) {
    if (atomic_exchange_explicit(&safetyLatched, 1U, memory_order_acq_rel) != 0U) {
        return;
    }
    (void)atomic_fetch_add_explicit(&safetyEpoch, 1U, memory_order_acq_rel);
    safetyStats.emergency_stops++;
    if (gripper_control_task_notify_safety_stop() == 0U) {
        safetyStats.notifications_failed++;
    }
    safety_task_emit_state(1U, cause);
}

static void safety_task_release(uint8_t cause) {
    if (atomic_exchange_explicit(&safetyLatched, 0U, memory_order_acq_rel) == 0U) {
        return;
    }
    (void)atomic_fetch_add_explicit(&safetyEpoch, 1U, memory_order_acq_rel);
    safetyStats.releases++;
    if (gripper_control_task_notify_safety_release() == 0U) {
        safetyStats.notifications_failed++;
    }
    safety_task_emit_state(0U, cause);
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

void StartSafetyTask(void* argument) {
    control_command_t message;

    (void)argument;
    memset(&safetyStats, 0, sizeof(safetyStats));
    for (;;) {
        const uint_fast8_t pending = atomic_exchange_explicit(&safetyPendingRequest, SAFETY_REQUEST_NONE,
                                                              memory_order_acq_rel);

        if (pending == SAFETY_REQUEST_STOP) {
            safety_task_enter_estop(UART_CMD_EMERGENCY_STOP);
        } else if (pending == SAFETY_REQUEST_RELEASE) {
            safety_task_release(UART_CMD_RESET_DEVICE);
        }

        if (safetyCommandQueueHandle == NULL ||
            osMessageQueueGet(safetyCommandQueueHandle, &message, NULL, SAFETY_QUEUE_WAIT_MS) != osOK) {
            continue;
        }
        if (message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP) {
            safety_task_enter_estop(message.frame.command);
        } else if (message.kind == APP_CONTROL_MESSAGE_SAFETY_RELEASE) {
            safety_task_release(message.frame.command);
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin) {
    if (gpio_pin == ESTOP_TEST_BUTTON_Pin) {
        (void)safety_task_request_local_estop();
    }
}
