#include "sorting_control_task.h"

#include <stdatomic.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "health_task.h"
#include "sorting_gate_mg90s.h"
#include "sorting_motor_tb6612.h"

#define SORTING_CONTROL_QUEUE_RETRY_TICKS 100U
#define SORTING_CONTROL_QUEUE_WAIT_TICKS 10U
#define SORTING_CONTROL_QUEUE_PUT_TIMEOUT 0U
#define SORTING_CONTROL_QUEUE_PRIORITY 0U
#define SORTING_CONTROL_INVALID_SAFETY_EPOCH UINT32_MAX

static sorting_control_t sortingController;
static sorting_control_task_stats_t sortingControlTaskStats;
static _Atomic sorting_control_safety_sync_state_t sortingSafetySyncState = SORTING_CONTROL_SAFETY_RELEASED;
static _Atomic uint32_t sortingCommandSafetyEpoch;

typedef struct {
    uint8_t useProvidedSequence;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} sorting_control_task_response_t;

typedef struct {
    uint8_t valid;
    app_uart_channel_t source;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
    uint32_t safetyEpoch;
    sorting_control_result_t result;
    sorting_control_task_response_t response;
} sorting_control_task_transaction_t;

static sorting_control_task_response_t sortingPendingResponse;
static sorting_control_task_transaction_t sortingTransactionCache;
static uint8_t sortingPendingResponseValid;
static uint8_t sortingPendingResponseAttempted;

static sorting_control_safety_sync_state_t sorting_control_task_safety_state(void) {
    return atomic_load_explicit(&sortingSafetySyncState, memory_order_acquire);
}

static void sorting_control_task_set_safety_state(sorting_control_safety_sync_state_t state) {
    atomic_store_explicit(&sortingSafetySyncState, state, memory_order_release);
}

static uint8_t sorting_control_task_transition_safety_state(sorting_control_safety_sync_state_t expected,
                                                            sorting_control_safety_sync_state_t desired) {
    return atomic_compare_exchange_strong_explicit(&sortingSafetySyncState, &expected, desired, memory_order_acq_rel,
                                                   memory_order_acquire)
               ? 1U
               : 0U;
}

static uint8_t sorting_control_task_is_status_command(uint8_t command) {
    return ((command == UART_CMD_SORTING_GET_STATUS) || (command == UART_CMD_SORTING_CONVEYOR_GET_STATUS)) ? 1U : 0U;
}

static uart_error_t sorting_control_task_result_error(sorting_control_result_t result,
                                                      const sorting_control_t* controller) {
    switch (result) {
        case SORTING_CONTROL_OK:
            return UART_ERROR_NONE;

        case SORTING_CONTROL_INVALID_SOURCE:
        case SORTING_CONTROL_UNSUPPORTED_COMMAND:
            return UART_ERROR_UNSUPPORTED_COMMAND;

        case SORTING_CONTROL_INVALID_PAYLOAD:
        case SORTING_CONTROL_SPEED_NOT_CONFIGURED:
            return UART_ERROR_INVALID_PAYLOAD;

        case SORTING_CONTROL_FAULT_LATCHED:
            return (controller->state.lastError != UART_ERROR_NONE) ? controller->state.lastError : UART_ERROR_INTERNAL;

        case SORTING_CONTROL_MOTOR_ERROR:
            return UART_ERROR_MOTOR;

        case SORTING_CONTROL_GATE_ERROR:
            return UART_ERROR_SERVO;

        case SORTING_CONTROL_BUSY:
        case SORTING_CONTROL_MOTION_PENDING:
        case SORTING_CONTROL_TX_BUSY:
            return UART_ERROR_BUSY;

        case SORTING_CONTROL_NO_ACTIVE_CYCLE:
        case SORTING_CONTROL_CYCLE_MISMATCH:
        case SORTING_CONTROL_SEQUENCE_CONFLICT:
            return UART_ERROR_SEQUENCE;

        case SORTING_CONTROL_STALE_COMMAND:
            return (controller->safetyStopLatched != 0U) ? UART_ERROR_EMERGENCY_STOP : UART_ERROR_SEQUENCE;

        case SORTING_CONTROL_INVALID_ARGUMENT:
        default:
            return UART_ERROR_INTERNAL;
    }
}

static uint8_t sorting_control_task_send_tx(const sorting_control_task_response_t* response) {
    int32_t sendResult;

    if ((response == NULL) || (response->length > UART_MAX_PAYLOAD_SIZE)) {
        sortingControlTaskStats.txQueueDrops++;
        return 0U;
    }

    if (response->useProvidedSequence != 0U) {
        sendResult = CommTx_SendWithSequence(COMM_TX_CH_SORTING, response->sequence, response->command,
                                             response->payload, response->length);
    } else {
        sendResult = CommTx_Send(COMM_TX_CH_SORTING, response->command, response->payload, response->length);
    }

    if (sendResult != 0) {
        sortingControlTaskStats.txQueueDrops++;
        return 0U;
    }

    return 1U;
}

uint8_t sorting_control_task_service_tx(void) {
    if (sortingPendingResponseValid == 0U) {
        return 1U;
    }

    if (sortingPendingResponseAttempted != 0U) {
        sortingControlTaskStats.txRetryAttempts++;
    }

    sortingPendingResponseAttempted = 1U;

    if (sorting_control_task_send_tx(&sortingPendingResponse) == 0U) {
        return 0U;
    }

    sortingPendingResponseValid = 0U;
    sortingPendingResponseAttempted = 0U;
    return 1U;
}

static uint8_t sorting_control_task_schedule_response(const sorting_control_task_response_t* response) {
    if ((response == NULL) || (response->length > UART_MAX_PAYLOAD_SIZE)) {
        sortingControlTaskStats.txPendingOverruns++;
        return 0U;
    }

    if (sortingPendingResponseValid != 0U) {
        sortingControlTaskStats.txPendingOverruns++;
        return 0U;
    }

    sortingPendingResponse = *response;
    sortingPendingResponseValid = 1U;
    sortingPendingResponseAttempted = 0U;
    return sorting_control_task_service_tx();
}

static sorting_control_task_response_t sorting_control_task_build_response(const control_command_t* message,
                                                                           sorting_control_result_t result,
                                                                           const sorting_control_t* controller) {
    sorting_control_task_response_t response;

    memset(&response, 0, sizeof(response));
    response.useProvidedSequence = 1U;
    response.sequence = message->frame.sequence;

    if (message->frame.command == UART_CMD_SORTING_GET_STATUS) {
        response.command = UART_CMD_RESPONSE;
        response.length = UART_SORTING_STATUS_PAYLOAD_SIZE;
        sorting_control_build_status_payload(&controller->state, response.payload);
    } else if (message->frame.command == UART_CMD_SORTING_CONVEYOR_GET_STATUS) {
        response.command = UART_CMD_RESPONSE;
        response.length = UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE;
        sorting_control_build_conveyor_status_payload(&controller->state, response.payload);
    } else {
        response.command = UART_CMD_OPERATION_RESULT;
        response.length = UART_OPERATION_RESULT_PAYLOAD_SIZE;
        response.payload[UART_OPERATION_RESULT_STATUS_INDEX] =
            (result == SORTING_CONTROL_OK) ? UART_STATUS_SUCCESS : UART_STATUS_ERROR;
        response.payload[UART_OPERATION_RESULT_ERROR_INDEX] =
            (uint8_t)sorting_control_task_result_error(result, controller);
        return response;
    }

    if (result != SORTING_CONTROL_OK) {
        response.payload[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_ERROR;
        response.payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)sorting_control_task_result_error(result, controller);
    }

    return response;
}

static sorting_control_task_response_t sorting_control_task_build_cycle_event(
    const sorting_cycle_complete_t* completion) {
    sorting_control_task_response_t response;

    memset(&response, 0, sizeof(response));
    response.command = UART_CMD_EVENT;
    response.length = UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE;
    response.payload[UART_EVENT_ID_INDEX] = UART_SORTING_EVENT_CYCLE_COMPLETE;
    response.payload[UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX] = (uint8_t)(completion->cycleId & 0xFFU);
    response.payload[UART_SORTING_EVENT_CYCLE_ID_HIGH_INDEX] = (uint8_t)((completion->cycleId >> 8U) & 0xFFU);
    response.payload[UART_SORTING_EVENT_DESTINATION_INDEX] = (uint8_t)completion->destination;
    return response;
}

static uint8_t sorting_control_task_same_transaction_identity(const control_command_t* message) {
    return ((sortingTransactionCache.valid != 0U) && (sortingTransactionCache.source == message->source) &&
            (sortingTransactionCache.sequence == message->frame.sequence))
               ? 1U
               : 0U;
}

static uint8_t sorting_control_task_same_wire_request(const control_command_t* message) {
    if ((sortingTransactionCache.command != message->frame.command) ||
        (sortingTransactionCache.length != message->frame.length)) {
        return 0U;
    }

    if (message->frame.length == 0U) {
        return 1U;
    }

    return (memcmp(sortingTransactionCache.payload, message->frame.payload, message->frame.length) == 0) ? 1U : 0U;
}

static void sorting_control_task_cache_transaction(const control_command_t* message, sorting_control_result_t result,
                                                   const sorting_control_task_response_t* response) {
    memset(&sortingTransactionCache, 0, sizeof(sortingTransactionCache));
    sortingTransactionCache.valid = 1U;
    sortingTransactionCache.source = message->source;
    sortingTransactionCache.sequence = message->frame.sequence;
    sortingTransactionCache.command = message->frame.command;
    sortingTransactionCache.length = message->frame.length;
    sortingTransactionCache.safetyEpoch = message->safetyEpoch;
    sortingTransactionCache.result = result;
    sortingTransactionCache.response = *response;

    if (message->frame.length != 0U) {
        memcpy(sortingTransactionCache.payload, message->frame.payload, message->frame.length);
    }
}

static sorting_control_result_t sorting_control_task_apply_requested_stop(sorting_control_t* controller) {
    sorting_control_result_t result;

    if (sorting_control_task_safety_state() != SORTING_CONTROL_SAFETY_STOP_REQUESTED) {
        return SORTING_CONTROL_OK;
    }

    sortingTransactionCache.valid = 0U;
    sortingPendingResponseValid = 0U;
    sortingPendingResponseAttempted = 0U;
    result = sorting_control_handle_safety_stop(controller);

    if (result == SORTING_CONTROL_OK) {
        (void)sorting_control_task_transition_safety_state(SORTING_CONTROL_SAFETY_STOP_REQUESTED,
                                                           SORTING_CONTROL_SAFETY_STOPPED);
    }

    return result;
}

static uint8_t sorting_control_task_is_safety_marker(const control_command_t* message,
                                                     app_control_message_kind_t kind) {
    return (message->kind == kind) ? 1U : 0U;
}

static control_command_t sorting_control_task_safety_marker(app_control_message_kind_t kind, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_6;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    message.kind = kind;
    return message;
}

sorting_control_result_t sorting_control_task_initialize_controller(sorting_control_t* controller,
                                                                    const sorting_motor_port_t* motor,
                                                                    const sorting_gate_port_t* gate) {
    sorting_control_result_t result;

    if ((controller == NULL) || (motor == NULL) || (gate == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    memset(&sortingTransactionCache, 0, sizeof(sortingTransactionCache));
    memset(&sortingPendingResponse, 0, sizeof(sortingPendingResponse));
    sortingPendingResponseValid = 0U;
    sortingPendingResponseAttempted = 0U;
    result = sorting_control_init(controller, motor, gate);
    (void)sorting_control_task_apply_requested_stop(controller);
    return result;
}

sorting_control_result_t sorting_control_task_process_message(sorting_control_t* controller,
                                                              const control_command_t* message) {
    sorting_control_result_t result;
    sorting_control_result_t safetyResult;
    sorting_control_task_response_t response;

    if ((controller == NULL) || (message == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    safetyResult = sorting_control_task_apply_requested_stop(controller);

    if (sorting_control_task_is_safety_marker(message, APP_CONTROL_MESSAGE_SAFETY_STOP) != 0U) {
        return safetyResult;
    }

    if (sorting_control_task_is_safety_marker(message, APP_CONTROL_MESSAGE_SAFETY_RELEASE) != 0U) {
        if (sorting_control_task_safety_state() != SORTING_CONTROL_SAFETY_RELEASE_REQUESTED) {
            return SORTING_CONTROL_OK;
        }

        result = sorting_control_handle_safety_release(controller);

        if (result == SORTING_CONTROL_OK) {
            (void)atomic_fetch_add_explicit(&sortingCommandSafetyEpoch, 1U, memory_order_acq_rel);
            sortingTransactionCache.valid = 0U;
            (void)sorting_control_task_transition_safety_state(SORTING_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                               SORTING_CONTROL_SAFETY_RELEASED);
        } else {
            (void)sorting_control_task_transition_safety_state(SORTING_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                               SORTING_CONTROL_SAFETY_STOPPED);
        }

        if (sorting_control_task_safety_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED) {
            (void)sorting_control_task_apply_requested_stop(controller);
        }

        return result;
    }

    if (sorting_control_task_same_transaction_identity(message) != 0U) {
        if ((sorting_control_task_same_wire_request(message) != 0U) &&
            (sortingTransactionCache.safetyEpoch == message->safetyEpoch)) {
            sortingControlTaskStats.duplicateCommands++;

            if (sortingPendingResponseValid == 0U) {
                (void)sorting_control_task_schedule_response(&sortingTransactionCache.response);
            } else {
                (void)sorting_control_task_service_tx();
            }

            return sortingTransactionCache.result;
        }

        if (sorting_control_task_service_tx() == 0U) {
            return SORTING_CONTROL_TX_BUSY;
        }

        if (sorting_control_task_same_wire_request(message) != 0U) {
            result = SORTING_CONTROL_STALE_COMMAND;
        } else {
            sortingControlTaskStats.sequenceConflicts++;
            result = SORTING_CONTROL_SEQUENCE_CONFLICT;
        }

        response = sorting_control_task_build_response(message, result, controller);
        (void)sorting_control_task_schedule_response(&response);
        return result;
    }

    if (sorting_control_task_service_tx() == 0U) {
        return SORTING_CONTROL_TX_BUSY;
    }

    if ((sorting_control_task_is_status_command(message->frame.command) == 0U) &&
        ((message->safetyEpoch == SORTING_CONTROL_INVALID_SAFETY_EPOCH) ||
         (message->safetyEpoch != atomic_load_explicit(&sortingCommandSafetyEpoch, memory_order_acquire)))) {
        result = SORTING_CONTROL_STALE_COMMAND;
    } else {
        result = sorting_control_process_command(controller, message);
    }

    if (sorting_control_task_safety_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED) {
        if (safetyResult != SORTING_CONTROL_MOTOR_ERROR) {
            (void)sorting_control_task_apply_requested_stop(controller);
        }

        if (sorting_control_task_is_status_command(message->frame.command) == 0U) {
            result = SORTING_CONTROL_STALE_COMMAND;
        }
    } else if ((sorting_control_task_is_status_command(message->frame.command) == 0U) &&
               (message->safetyEpoch != atomic_load_explicit(&sortingCommandSafetyEpoch, memory_order_acquire))) {
        (void)sorting_control_task_apply_requested_stop(controller);
        result = SORTING_CONTROL_STALE_COMMAND;
    }

    response = sorting_control_task_build_response(message, result, controller);
    sorting_control_task_cache_transaction(message, result, &response);
    (void)sorting_control_task_schedule_response(&response);
    return result;
}

sorting_control_result_t sorting_control_task_service_motion(sorting_control_t* controller) {
    sorting_cycle_complete_t completion;
    sorting_control_result_t result;
    sorting_control_task_response_t response;

    if (controller == NULL) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    if (sorting_control_task_service_tx() == 0U) {
        return SORTING_CONTROL_TX_BUSY;
    }

    result = sorting_control_service_motion(controller, &completion);

    if ((result == SORTING_CONTROL_OK) && (completion.valid != 0U)) {
        response = sorting_control_task_build_cycle_event(&completion);
        sortingControlTaskStats.cycleCompleteEvents++;
        (void)sorting_control_task_schedule_response(&response);
    }

    return result;
}

uint8_t sorting_control_task_notify_safety_stop(void) {
    control_command_t message;

    sorting_control_task_set_safety_state(SORTING_CONTROL_SAFETY_STOP_REQUESTED);
    (void)atomic_fetch_add_explicit(&sortingCommandSafetyEpoch, 1U, memory_order_acq_rel);
    message = sorting_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);

    if ((sortingControlQueueHandle == NULL) ||
        (osMessageQueuePut(sortingControlQueueHandle, &message, SORTING_CONTROL_QUEUE_PRIORITY,
                           SORTING_CONTROL_QUEUE_PUT_TIMEOUT) != osOK)) {
        sortingControlTaskStats.safetyQueueDrops++;
        return 0U;
    }

    return 1U;
}

uint8_t sorting_control_task_notify_safety_release(void) {
    control_command_t message;

    if (sorting_control_task_transition_safety_state(SORTING_CONTROL_SAFETY_STOPPED,
                                                     SORTING_CONTROL_SAFETY_RELEASE_REQUESTED) == 0U) {
        return 0U;
    }

    message = sorting_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);

    if ((sortingControlQueueHandle == NULL) ||
        (osMessageQueuePut(sortingControlQueueHandle, &message, SORTING_CONTROL_QUEUE_PRIORITY,
                           SORTING_CONTROL_QUEUE_PUT_TIMEOUT) != osOK)) {
        (void)sorting_control_task_transition_safety_state(SORTING_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                           SORTING_CONTROL_SAFETY_STOPPED);
        sortingControlTaskStats.safetyQueueDrops++;
        return 0U;
    }

    return (sorting_control_task_safety_state() == SORTING_CONTROL_SAFETY_RELEASE_REQUESTED) ? 1U : 0U;
}

sorting_control_safety_sync_state_t sorting_control_task_get_safety_sync_state(void) {
    return sorting_control_task_safety_state();
}

uint32_t sorting_control_task_capture_command_epoch(void) {
    if (sorting_control_task_safety_state() != SORTING_CONTROL_SAFETY_RELEASED) {
        return SORTING_CONTROL_INVALID_SAFETY_EPOCH;
    }

    return atomic_load_explicit(&sortingCommandSafetyEpoch, memory_order_acquire);
}

void sorting_control_task_get_stats(sorting_control_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = sortingControlTaskStats;
    }
}

void StartSortingControlTask(void* argument) {
    control_command_t message;

    (void)argument;
    (void)sorting_control_task_initialize_controller(&sortingController, sorting_motor_tb6612_port(),
                                                     sorting_gate_mg90s_port());

    for (;;) {
        Health_TaskAlive(HEALTH_TASK_SORTING_CONTROL);

        if (sorting_control_task_safety_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED) {
            message = sorting_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);
            (void)sorting_control_task_process_message(&sortingController, &message);
        }

        if (sorting_control_task_service_tx() == 0U) {
            osDelay(1U);
            continue;
        }

        if (sorting_control_task_service_motion(&sortingController) == SORTING_CONTROL_TX_BUSY) {
            osDelay(1U);
            continue;
        }

        if (sortingControlQueueHandle == NULL) {
            osDelay(SORTING_CONTROL_QUEUE_RETRY_TICKS);
            continue;
        }

        if (osMessageQueueGet(sortingControlQueueHandle, &message, NULL, SORTING_CONTROL_QUEUE_WAIT_TICKS) != osOK) {
            continue;
        }

        (void)sorting_control_task_process_message(&sortingController, &message);
    }
}
