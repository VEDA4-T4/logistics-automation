#include "gripper_control_task.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_messages.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "gripper_calibration.h"
#include "logistics/contracts/uart/gripper_commands.h"
#include "safety_task.h"
#include "stm32f4xx_hal.h"
#include "task.h"

#define GRIPPER_QUEUE_WAIT_MS 5U

#define GRIPPER_TASK_SAFETY_STOP_PENDING (1U << 0U)
#define GRIPPER_TASK_SAFETY_RELEASE_PENDING (1U << 1U)

typedef struct {
    uint8_t valid;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
    uint32_t safety_epoch;
    gripper_control_result_t result;
    uint8_t response_length;
    uint8_t response[UART_MAX_PAYLOAD_SIZE];
} gripper_transaction_cache_t;

static gripper_control_t gripperController;
static gripper_control_snapshot_t gripperSnapshot;
static gripper_control_task_stats_t gripperTaskStats;
static gripper_transaction_cache_t gripperCache;
static atomic_uint_fast8_t gripperSafetyPendingFlags = ATOMIC_VAR_INIT(0U);
static uint8_t gripperPendingResponseValid;
static uint8_t gripperPendingSequence;
static uint8_t gripperPendingLength;
static uint8_t gripperPendingResponse[UART_MAX_PAYLOAD_SIZE];
static uint8_t gripperPendingEventValid;
static uint8_t gripperPendingEventLength;
static uint8_t gripperPendingEvent[UART_MAX_PAYLOAD_SIZE];

static void gripper_control_task_publish_snapshot(void) {
    gripper_control_snapshot_t snapshot;

    gripper_control_get_snapshot(&gripperController, &snapshot);
    taskENTER_CRITICAL();
    gripperSnapshot = snapshot;
    taskEXIT_CRITICAL();
}

static control_command_t gripper_control_task_safety_marker(app_control_message_kind_t kind, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.kind = kind;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    message.safety_epoch = safety_task_get_epoch();
    return message;
}

static uint8_t gripper_control_task_enqueue_safety(app_control_message_kind_t kind, uint8_t command) {
    const control_command_t message = gripper_control_task_safety_marker(kind, command);

    if (gripperControlQueueHandle == NULL || osMessageQueuePut(gripperControlQueueHandle, &message, 3U, 0U) != osOK) {
        gripperTaskStats.safety_queue_drops++;
        return 0U;
    }
    return 1U;
}

uint8_t gripper_control_task_notify_safety_stop(void) {
    (void)atomic_fetch_or_explicit(&gripperSafetyPendingFlags, GRIPPER_TASK_SAFETY_STOP_PENDING,
                                   memory_order_release);
    return gripper_control_task_enqueue_safety(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);
}

uint8_t gripper_control_task_notify_safety_release(void) {
    (void)atomic_fetch_or_explicit(&gripperSafetyPendingFlags, GRIPPER_TASK_SAFETY_RELEASE_PENDING,
                                   memory_order_release);
    return gripper_control_task_enqueue_safety(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);
}

static uint8_t gripper_control_task_service_tx(void) {
    if (gripperPendingResponseValid != 0U) {
        if (CommTx_SendWithSequence(gripperPendingSequence, UART_CMD_RESPONSE, gripperPendingResponse,
                                    gripperPendingLength) != 0) {
            gripperTaskStats.response_retries++;
            return 0U;
        }

        gripperPendingResponseValid = 0U;
    }

    if (gripperPendingEventValid != 0U) {
        if (CommTx_Send(UART_CMD_EVENT, gripperPendingEvent, gripperPendingEventLength) != 0) {
            gripperTaskStats.event_retries++;
            return 0U;
        }

        gripperPendingEventValid = 0U;
    }

    return 1U;
}

static void gripper_control_task_schedule_response(uint8_t sequence, const uint8_t* payload, uint8_t length) {
    if (CommTx_SendWithSequence(sequence, UART_CMD_RESPONSE, payload, length) == 0) {
        return;
    }

    gripperPendingSequence = sequence;
    gripperPendingLength = length;
    memcpy(gripperPendingResponse, payload, length);
    gripperPendingResponseValid = 1U;
    gripperTaskStats.response_drops++;
}

static uint8_t gripper_control_task_result_error(gripper_control_result_t result) {
    switch (result) {
        case GRIPPER_CONTROL_OK:
            return UART_ERROR_NONE;
        case GRIPPER_CONTROL_INVALID_ARGUMENT:
        case GRIPPER_CONTROL_INVALID_PAYLOAD:
            return UART_ERROR_INVALID_PAYLOAD;
        case GRIPPER_CONTROL_BUSY:
            return UART_ERROR_BUSY;
        case GRIPPER_CONTROL_NOT_HOMED:
            return UART_GRIPPER_ERROR_NOT_HOMED;
        case GRIPPER_CONTROL_SAFETY_STOP:
            return UART_ERROR_EMERGENCY_STOP;
        case GRIPPER_CONTROL_SERVO_ERROR:
            return UART_ERROR_SERVO;
        default:
            return UART_ERROR_INTERNAL;
    }
}

static uint8_t gripper_control_task_result_status(uint8_t command, gripper_control_result_t result) {
    if (result == GRIPPER_CONTROL_BUSY) {
        return UART_STATUS_BUSY;
    }
    if (result != GRIPPER_CONTROL_OK) {
        return UART_STATUS_ERROR;
    }
    if (command == UART_CMD_GRIPPER_MOVE_ARM || command == UART_CMD_GRIPPER_SET_GRIPPER ||
        command == UART_CMD_GRIPPER_HOME) {
        return UART_STATUS_ACK;
    }
    return UART_STATUS_SUCCESS;
}

static uint8_t gripper_control_task_build_response(const control_command_t* message, gripper_control_result_t result,
                                                   uint8_t* payload) {
    gripper_control_snapshot_t snapshot;
    uint8_t length = UART_RESPONSE_HEADER_SIZE;

    payload[UART_RESPONSE_STATUS_INDEX] = gripper_control_task_result_status(message->frame.command, result);
    payload[UART_RESPONSE_COMMAND_INDEX] = message->frame.command;
    payload[UART_RESPONSE_ERROR_INDEX] = gripper_control_task_result_error(result);

    if (result == GRIPPER_CONTROL_OK &&
        (message->frame.command == UART_CMD_GRIPPER_GET_STATUS || message->frame.command == UART_CMD_GET_STATUS)) {
        gripper_control_get_snapshot(&gripperController, &snapshot);
        payload[UART_GRIPPER_STATUS_STATE_INDEX] = (uint8_t)snapshot.state;
        payload[UART_GRIPPER_STATUS_MOTION_ID_LOW_INDEX] = (uint8_t)(snapshot.active_motion_id & 0xFFU);
        payload[UART_GRIPPER_STATUS_MOTION_ID_HIGH_INDEX] = (uint8_t)(snapshot.active_motion_id >> 8U);
        payload[UART_GRIPPER_STATUS_BASE_ANGLE_LOW_INDEX] = (uint8_t)(snapshot.base_angle & 0xFFU);
        payload[UART_GRIPPER_STATUS_BASE_ANGLE_HIGH_INDEX] = (uint8_t)(snapshot.base_angle >> 8U);
        payload[UART_GRIPPER_STATUS_SHOULDER_ANGLE_LOW_INDEX] = (uint8_t)(snapshot.shoulder_angle & 0xFFU);
        payload[UART_GRIPPER_STATUS_SHOULDER_ANGLE_HIGH_INDEX] = (uint8_t)(snapshot.shoulder_angle >> 8U);
        payload[UART_GRIPPER_STATUS_ELBOW_ANGLE_LOW_INDEX] = (uint8_t)(snapshot.elbow_angle & 0xFFU);
        payload[UART_GRIPPER_STATUS_ELBOW_ANGLE_HIGH_INDEX] = (uint8_t)(snapshot.elbow_angle >> 8U);
        payload[UART_GRIPPER_STATUS_POSITION_INDEX] = snapshot.gripper_position;
        payload[UART_GRIPPER_STATUS_HOMED_INDEX] = snapshot.homed;
        length = UART_GRIPPER_STATUS_PAYLOAD_SIZE;
    }
    return length;
}

static uint8_t gripper_control_task_same_request(const control_command_t* message) {
    return (gripperCache.valid != 0U && gripperCache.sequence == message->frame.sequence &&
            gripperCache.command == message->frame.command && gripperCache.length == message->frame.length &&
            (message->frame.length == 0U ||
             memcmp(gripperCache.payload, message->frame.payload, message->frame.length) == 0))
               ? 1U
               : 0U;
}

static void gripper_control_task_cache(const control_command_t* message, gripper_control_result_t result,
                                       const uint8_t* response, uint8_t response_length) {
    gripperCache.valid = 1U;
    gripperCache.sequence = message->frame.sequence;
    gripperCache.command = message->frame.command;
    gripperCache.length = message->frame.length;
    gripperCache.safety_epoch = message->safety_epoch;
    gripperCache.result = result;
    gripperCache.response_length = response_length;
    if (message->frame.length > 0U) {
        memcpy(gripperCache.payload, message->frame.payload, message->frame.length);
    }
    memcpy(gripperCache.response, response, response_length);
}

static void gripper_control_task_apply_pending_safety(void) {
    const uint_fast8_t pending =
        atomic_exchange_explicit(&gripperSafetyPendingFlags, 0U, memory_order_acq_rel);

    if ((pending & GRIPPER_TASK_SAFETY_STOP_PENDING) != 0U) {
        gripper_control_apply_safety_stop(&gripperController);
        gripperCache.valid = 0U;
    }

    if ((pending & GRIPPER_TASK_SAFETY_RELEASE_PENDING) != 0U) {
        gripper_control_release_safety(&gripperController);
        gripperCache.valid = 0U;
    }
}

static gripper_control_result_t gripper_control_task_process_message(const control_command_t* message) {
    gripper_control_result_t result;
    uint8_t response[UART_MAX_PAYLOAD_SIZE];
    uint8_t response_length;

    if (message == NULL) {
        return GRIPPER_CONTROL_INVALID_ARGUMENT;
    }

    gripper_control_task_apply_pending_safety();
    if (message->kind != APP_CONTROL_MESSAGE_UART_COMMAND) {
        return GRIPPER_CONTROL_OK;
    }

    if (gripperCache.valid != 0U && gripperCache.sequence == message->frame.sequence) {
        if (gripper_control_task_same_request(message) != 0U && gripperCache.safety_epoch == message->safety_epoch) {
            gripperTaskStats.duplicate_commands++;
            gripper_control_task_schedule_response(message->frame.sequence, gripperCache.response,
                                                   gripperCache.response_length);
            return gripperCache.result;
        }

        gripperTaskStats.sequence_conflicts++;
        response[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_NACK;
        response[UART_RESPONSE_COMMAND_INDEX] = message->frame.command;
        response[UART_RESPONSE_ERROR_INDEX] = UART_ERROR_SEQUENCE;
        gripper_control_task_schedule_response(message->frame.sequence, response, UART_RESPONSE_HEADER_SIZE);
        return GRIPPER_CONTROL_INVALID_PAYLOAD;
    }

    if (message->frame.command != UART_CMD_GRIPPER_GET_STATUS && message->frame.command != UART_CMD_GET_STATUS &&
        message->safety_epoch != safety_task_get_epoch()) {
        gripperTaskStats.stale_commands++;
        result = GRIPPER_CONTROL_SAFETY_STOP;
    } else if (message->frame.command == UART_CMD_GET_STATUS) {
        result = GRIPPER_CONTROL_OK;
    } else {
        result = gripper_control_process_command(&gripperController, message->frame.command, message->frame.payload,
                                                 message->frame.length, HAL_GetTick());
    }

    gripperTaskStats.commands++;
    if (result == GRIPPER_CONTROL_BUSY) {
        gripperTaskStats.busy_commands++;
    } else if (result == GRIPPER_CONTROL_SERVO_ERROR) {
        gripperTaskStats.servo_faults++;
    }

    response_length = gripper_control_task_build_response(message, result, response);
    gripper_control_task_cache(message, result, response, response_length);
    gripper_control_task_schedule_response(message->frame.sequence, response, response_length);
    return result;
}

static void gripper_control_task_emit_completion(void) {
    gripper_control_completion_t completion;
    uint8_t payload[UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE];
    uint8_t length;

    if (gripper_control_take_completion(&gripperController, &completion) == 0U) {
        return;
    }

    payload[UART_EVENT_ID_INDEX] = (completion.fault != 0U) ? UART_GRIPPER_EVENT_FAULT
                                                            : UART_GRIPPER_EVENT_MOTION_COMPLETE;
    payload[UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX] = (uint8_t)(completion.motion_id & 0xFFU);
    payload[UART_GRIPPER_EVENT_MOTION_ID_HIGH_INDEX] = (uint8_t)(completion.motion_id >> 8U);
    payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX] = (uint8_t)completion.motion_type;
    length = UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE;
    if (completion.fault != 0U) {
        payload[UART_GRIPPER_FAULT_EVENT_ERROR_INDEX] = (uint8_t)completion.error;
        length = UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE;
        gripperTaskStats.servo_faults++;
    } else {
        gripperTaskStats.motion_completions++;
    }

    if (CommTx_Send(UART_CMD_EVENT, payload, length) != 0) {
        memcpy(gripperPendingEvent, payload, length);
        gripperPendingEventLength = length;
        gripperPendingEventValid = 1U;
    }
}

void gripper_control_task_get_snapshot(gripper_control_snapshot_t* snapshot) {
    if (snapshot != NULL) {
        taskENTER_CRITICAL();
        *snapshot = gripperSnapshot;
        taskEXIT_CRITICAL();
    }
}

void gripper_control_task_get_stats(gripper_control_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = gripperTaskStats;
    }
}

void StartGripperControlTask(void* argument) {
    control_command_t message;
    uint32_t last_update;

    (void)argument;
    memset(&gripperTaskStats, 0, sizeof(gripperTaskStats));
    memset(&gripperCache, 0, sizeof(gripperCache));
    (void)gripper_control_init(&gripperController, gripper_servo_mg90s_port());
    gripper_control_task_publish_snapshot();
    last_update = HAL_GetTick();

    for (;;) {
        gripper_control_task_apply_pending_safety();
        if (gripper_control_task_service_tx() == 0U) {
            osDelay(1U);
            continue;
        }

        if ((HAL_GetTick() - last_update) >= GRIPPER_CONTROL_UPDATE_PERIOD_MS) {
            gripper_control_tick(&gripperController, HAL_GetTick());
            gripper_control_task_emit_completion();
            gripper_control_task_publish_snapshot();
            last_update = HAL_GetTick();
        }

        if (gripperControlQueueHandle != NULL &&
            osMessageQueueGet(gripperControlQueueHandle, &message, NULL, GRIPPER_QUEUE_WAIT_MS) == osOK) {
            (void)gripper_control_task_process_message(&message);
            gripper_control_task_publish_snapshot();
        }
    }
}
