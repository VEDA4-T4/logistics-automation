#include "input_control_task.h"

#include <stdatomic.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "health_task.h"
#include "input_motor_tb6612.h"

#define INPUT_CONTROL_QUEUE_RETRY_TICKS 100U
#define INPUT_CONTROL_QUEUE_WAIT_TICKS 10U
#define INPUT_CONTROL_QUEUE_PUT_TIMEOUT 0U
#define INPUT_CONTROL_QUEUE_PRIORITY 0U
#define INPUT_CONTROL_INVALID_SAFETY_EPOCH UINT32_MAX

static input_control_t inputController;
static input_control_task_stats_t inputControlTaskStats;
static _Atomic input_control_safety_sync_state_t inputSafetySyncState = INPUT_CONTROL_SAFETY_RELEASED;
static _Atomic uint32_t inputCommandSafetyEpoch;

typedef struct {
    app_uart_channel_t destination;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} input_control_task_response_t;

typedef struct {
    uint8_t valid;
    app_uart_channel_t source;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
    uint32_t safetyEpoch;
    input_control_result_t result;
    input_control_task_response_t response;
} input_control_task_transaction_t;

static input_control_task_response_t inputPendingResponse;
static input_control_task_transaction_t inputTransactionCache;
static uint8_t inputPendingResponseValid;
static uint8_t inputPendingResponseAttempted;

static input_control_safety_sync_state_t input_control_task_safety_state(void) {
    return atomic_load_explicit(&inputSafetySyncState, memory_order_acquire);
}

static void input_control_task_set_safety_state(input_control_safety_sync_state_t state) {
    atomic_store_explicit(&inputSafetySyncState, state, memory_order_release);
}

static uint8_t input_control_task_transition_safety_state(input_control_safety_sync_state_t expected,
                                                          input_control_safety_sync_state_t desired) {
    return atomic_compare_exchange_strong_explicit(&inputSafetySyncState, &expected, desired, memory_order_acq_rel,
                                                   memory_order_acquire)
               ? 1U
               : 0U;
}

static uart_error_t input_control_task_result_error(input_control_result_t result, const input_control_t* controller) {
    const input_control_state_t* state;

    state = &controller->state;

    switch (result) {
        case INPUT_CONTROL_OK:
            return UART_ERROR_NONE;

        case INPUT_CONTROL_INVALID_SOURCE:
        case INPUT_CONTROL_UNSUPPORTED_COMMAND:
            return UART_ERROR_UNSUPPORTED_COMMAND;

        case INPUT_CONTROL_INVALID_PAYLOAD:
            return UART_ERROR_INVALID_PAYLOAD;

        case INPUT_CONTROL_SPEED_NOT_CONFIGURED:
            return UART_ERROR_SPEED_NOT_CONFIGURED;

        case INPUT_CONTROL_FAULT_LATCHED:
            return (state->lastError != UART_ERROR_NONE) ? state->lastError : UART_ERROR_INTERNAL;

        case INPUT_CONTROL_MOTOR_ERROR:
            return UART_ERROR_MOTOR;

        case INPUT_CONTROL_STALE_COMMAND:
            return (controller->safetyStopLatched != 0U) ? UART_ERROR_EMERGENCY_STOP : UART_ERROR_SEQUENCE;

        case INPUT_CONTROL_SEQUENCE_CONFLICT:
            return UART_ERROR_SEQUENCE;

        case INPUT_CONTROL_TX_BUSY:
            return UART_ERROR_BUSY;

        case INPUT_CONTROL_INVALID_ARGUMENT:
        default:
            return UART_ERROR_INTERNAL;
    }
}

static void input_control_task_update_device_status(uint8_t command, input_control_result_t result,
                                                    const input_control_t* controller) {
    uint8_t deviceState;

    if (controller == NULL) {
        return;
    }

    if (result == INPUT_CONTROL_MOTOR_ERROR) {
        CommTx_SetChannelDeviceStatus(COMM_TX_CH_INPUT, UART_DEVICE_ERROR, UART_ERROR_MOTOR);
        return;
    }

    if (result != INPUT_CONTROL_OK) {
        return;
    }

    if ((command != UART_CMD_INPUT_CONVEYOR_START) && (command != UART_CMD_INPUT_CONVEYOR_STOP) &&
        (command != UART_CMD_INPUT_CONVEYOR_SET_SPEED) && (command != UART_CMD_INPUT_CONTROL_RESET)) {
        return;
    }

    deviceState =
        (controller->state.conveyorState == UART_INPUT_CONVEYOR_RUNNING) ? UART_DEVICE_RUNNING : UART_DEVICE_STOPPED;
    CommTx_SetChannelDeviceStatus(COMM_TX_CH_INPUT, deviceState, UART_ERROR_NONE);
}

static uint8_t input_control_task_send_tx(const input_control_task_response_t* response) {
    comm_tx_channel_t channel;

    if (response == NULL) {
        inputControlTaskStats.txQueueDrops++;
        return 0U;
    }

    if (response->destination == APP_UART_CHANNEL_1) {
        channel = COMM_TX_CH_INPUT;
    } else if (response->destination == APP_UART_CHANNEL_6) {
        channel = COMM_TX_CH_SORTING;
    } else {
        inputControlTaskStats.txQueueDrops++;
        return 0U;
    }

    /*
     * urgent 큐를 쓴다. 이 응답은 Pi가 ~400ms 재시도 예산 안에 받아야 하는데,
     * 일반 큐는 센서 텔레메트리(SENSOR_STATUS, 상태 무관 매 폴링 전송)와
     * heartbeat가 상시 점유하고 있어 링버퍼에 밀리면 그 예산을 넘길 수 있다.
     */
    if (CommTx_SendUrgentWithSequence(channel, response->sequence, response->command, response->payload,
                                      response->length) != 0) {
        inputControlTaskStats.txQueueDrops++;
        return 0U;
    }

    return 1U;
}

uint8_t input_control_task_service_tx(void) {
    if (inputPendingResponseValid == 0U) {
        return 1U;
    }

    if (inputPendingResponseAttempted != 0U) {
        inputControlTaskStats.txRetryAttempts++;
    }

    inputPendingResponseAttempted = 1U;

    if (input_control_task_send_tx(&inputPendingResponse) == 0U) {
        return 0U;
    }

    inputPendingResponseValid = 0U;
    inputPendingResponseAttempted = 0U;
    return 1U;
}

static uint8_t input_control_task_schedule_response(const input_control_task_response_t* response) {
    if ((response == NULL) || (response->length > UART_MAX_PAYLOAD_SIZE)) {
        inputControlTaskStats.txPendingOverruns++;
        return 0U;
    }

    if (inputPendingResponseValid != 0U) {
        inputControlTaskStats.txPendingOverruns++;
        return 0U;
    }

    inputPendingResponse = *response;
    inputPendingResponseValid = 1U;
    inputPendingResponseAttempted = 0U;
    return input_control_task_service_tx();
}

static input_control_task_response_t input_control_task_build_response(const control_command_t* message,
                                                                       input_control_result_t result,
                                                                       const input_control_t* controller) {
    input_control_task_response_t response;

    memset(&response, 0, sizeof(response));
    response.destination = message->source;
    response.sequence = message->frame.sequence;

    if (message->frame.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS) {
        response.command = UART_CMD_RESPONSE;
        response.length = UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE;
        input_control_build_status_payload(&controller->state, response.payload);

        if (result != INPUT_CONTROL_OK) {
            response.payload[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_ERROR;
            response.payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)input_control_task_result_error(result, controller);
        }
    } else {
        response.command = UART_CMD_OPERATION_RESULT;
        response.length = UART_OPERATION_RESULT_PAYLOAD_SIZE;
        response.payload[UART_OPERATION_RESULT_STATUS_INDEX] =
            (result == INPUT_CONTROL_OK) ? UART_STATUS_SUCCESS : UART_STATUS_ERROR;
        response.payload[UART_OPERATION_RESULT_ERROR_INDEX] =
            (uint8_t)input_control_task_result_error(result, controller);
    }

    return response;
}

static uint8_t input_control_task_same_transaction_identity(const control_command_t* message) {
    return ((inputTransactionCache.valid != 0U) && (inputTransactionCache.source == message->source) &&
            (inputTransactionCache.sequence == message->frame.sequence))
               ? 1U
               : 0U;
}

static uint8_t input_control_task_same_wire_request(const control_command_t* message) {
    if ((inputTransactionCache.command != message->frame.command) ||
        (inputTransactionCache.length != message->frame.length)) {
        return 0U;
    }

    if (message->frame.length == 0U) {
        return 1U;
    }

    return (memcmp(inputTransactionCache.payload, message->frame.payload, message->frame.length) == 0) ? 1U : 0U;
}

static void input_control_task_cache_transaction(const control_command_t* message, input_control_result_t result,
                                                 const input_control_task_response_t* response) {
    memset(&inputTransactionCache, 0, sizeof(inputTransactionCache));
    inputTransactionCache.valid = 1U;
    inputTransactionCache.source = message->source;
    inputTransactionCache.sequence = message->frame.sequence;
    inputTransactionCache.command = message->frame.command;
    inputTransactionCache.length = message->frame.length;
    inputTransactionCache.safetyEpoch = message->safetyEpoch;
    inputTransactionCache.result = result;
    inputTransactionCache.response = *response;

    if (message->frame.length != 0U) {
        memcpy(inputTransactionCache.payload, message->frame.payload, message->frame.length);
    }
}

static input_control_result_t input_control_task_apply_requested_stop(input_control_t* controller) {
    input_control_result_t result;

    if (input_control_task_safety_state() != INPUT_CONTROL_SAFETY_STOP_REQUESTED) {
        return INPUT_CONTROL_OK;
    }

    inputTransactionCache.valid = 0U;
    inputPendingResponseValid = 0U;
    inputPendingResponseAttempted = 0U;

    result = input_control_handle_safety_stop(controller);

    if (result == INPUT_CONTROL_OK) {
        (void)input_control_task_transition_safety_state(INPUT_CONTROL_SAFETY_STOP_REQUESTED,
                                                         INPUT_CONTROL_SAFETY_STOPPED);
    }

    return result;
}

static uint8_t input_control_task_is_safety_marker(const control_command_t* message, app_control_message_kind_t kind) {
    return (message->kind == kind) ? 1U : 0U;
}

static control_command_t input_control_task_safety_marker(app_control_message_kind_t kind, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_1;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    message.kind = kind;
    return message;
}

input_control_result_t input_control_task_initialize_controller(input_control_t* controller,
                                                                const input_motor_port_t* motor) {
    input_control_result_t result;

    if ((controller == NULL) || (motor == NULL) || (motor->initialize == NULL) || (motor->apply == NULL)) {
        return INPUT_CONTROL_INVALID_ARGUMENT;
    }

    memset(&inputTransactionCache, 0, sizeof(inputTransactionCache));
    memset(&inputPendingResponse, 0, sizeof(inputPendingResponse));
    inputPendingResponseValid = 0U;
    inputPendingResponseAttempted = 0U;

    result = input_control_init(controller, motor);
    (void)input_control_task_apply_requested_stop(controller);

    return result;
}

input_control_result_t input_control_task_process_message(input_control_t* controller,
                                                          const control_command_t* message) {
    input_control_result_t result;
    input_control_result_t safetyResult;
    input_control_state_t previous;
    input_control_task_response_t response;

    if ((controller == NULL) || (message == NULL)) {
        return INPUT_CONTROL_INVALID_ARGUMENT;
    }

    previous = controller->state;
    safetyResult = input_control_task_apply_requested_stop(controller);

    if (input_control_task_is_safety_marker(message, APP_CONTROL_MESSAGE_SAFETY_STOP) != 0U) {
        result = safetyResult;
    } else if (input_control_task_is_safety_marker(message, APP_CONTROL_MESSAGE_SAFETY_RELEASE) != 0U) {
        if (input_control_task_safety_state() != INPUT_CONTROL_SAFETY_RELEASE_REQUESTED) {
            result = INPUT_CONTROL_OK;
        } else {
            result = input_control_handle_safety_release(controller);

            if (result == INPUT_CONTROL_OK) {
                (void)atomic_fetch_add_explicit(&inputCommandSafetyEpoch, 1U, memory_order_acq_rel);
                inputTransactionCache.valid = 0U;
                (void)input_control_task_transition_safety_state(INPUT_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                                 INPUT_CONTROL_SAFETY_RELEASED);
            } else {
                (void)input_control_task_transition_safety_state(INPUT_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                                 INPUT_CONTROL_SAFETY_STOPPED);
            }

            if (input_control_task_safety_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED) {
                safetyResult = input_control_task_apply_requested_stop(controller);
            }
        }
    } else {
        if (input_control_task_same_transaction_identity(message) != 0U) {
            if ((input_control_task_same_wire_request(message) != 0U) &&
                (inputTransactionCache.safetyEpoch == message->safetyEpoch)) {
                inputControlTaskStats.duplicateCommands++;

                if (inputPendingResponseValid == 0U) {
                    (void)input_control_task_schedule_response(&inputTransactionCache.response);
                } else {
                    (void)input_control_task_service_tx();
                }

                return inputTransactionCache.result;
            }

            if (input_control_task_service_tx() == 0U) {
                return INPUT_CONTROL_TX_BUSY;
            }

            if (input_control_task_same_wire_request(message) != 0U) {
                result = INPUT_CONTROL_STALE_COMMAND;
            } else {
                inputControlTaskStats.sequenceConflicts++;
                result = INPUT_CONTROL_SEQUENCE_CONFLICT;
            }

            response = input_control_task_build_response(message, result, controller);
            (void)input_control_task_schedule_response(&response);
            return result;
        }

        if (input_control_task_service_tx() == 0U) {
            return INPUT_CONTROL_TX_BUSY;
        }

        if ((message->frame.command != UART_CMD_INPUT_CONVEYOR_GET_STATUS) &&
            ((message->safetyEpoch == INPUT_CONTROL_INVALID_SAFETY_EPOCH) ||
             (message->safetyEpoch != atomic_load_explicit(&inputCommandSafetyEpoch, memory_order_acquire)))) {
            result = INPUT_CONTROL_STALE_COMMAND;
        } else {
            result = input_control_process_command(controller, message);
        }

        if (input_control_task_safety_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED) {
            if (message->frame.command != UART_CMD_INPUT_CONVEYOR_GET_STATUS) {
                controller->state.speed = previous.speed;
            }

            if (safetyResult != INPUT_CONTROL_MOTOR_ERROR) {
                safetyResult = input_control_task_apply_requested_stop(controller);
            }

            if (message->frame.command != UART_CMD_INPUT_CONVEYOR_GET_STATUS) {
                result = INPUT_CONTROL_STALE_COMMAND;
            }
        } else if ((message->frame.command != UART_CMD_INPUT_CONVEYOR_GET_STATUS) &&
                   (message->safetyEpoch != atomic_load_explicit(&inputCommandSafetyEpoch, memory_order_acquire))) {
            controller->state.speed = previous.speed;

            if (safetyResult != INPUT_CONTROL_MOTOR_ERROR) {
                safetyResult = input_control_task_apply_requested_stop(controller);
            }

            result = INPUT_CONTROL_STALE_COMMAND;
        }

        input_control_task_update_device_status(message->frame.command, result, controller);

        response = input_control_task_build_response(message, result, controller);
        input_control_task_cache_transaction(message, result, &response);
        (void)input_control_task_schedule_response(&response);
    }

    return result;
}

uint8_t input_control_task_notify_safety_stop(void) {
    control_command_t message;

    input_control_task_set_safety_state(INPUT_CONTROL_SAFETY_STOP_REQUESTED);
    (void)atomic_fetch_add_explicit(&inputCommandSafetyEpoch, 1U, memory_order_acq_rel);
    message = input_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);

    if ((inputControlQueueHandle == NULL) ||
        (osMessageQueuePut(inputControlQueueHandle, &message, INPUT_CONTROL_QUEUE_PRIORITY,
                           INPUT_CONTROL_QUEUE_PUT_TIMEOUT) != osOK)) {
        inputControlTaskStats.safetyQueueDrops++;
        return 0U;
    }

    return 1U;
}

uint8_t input_control_task_notify_safety_release(void) {
    control_command_t message;

    if (input_control_task_transition_safety_state(INPUT_CONTROL_SAFETY_STOPPED,
                                                   INPUT_CONTROL_SAFETY_RELEASE_REQUESTED) == 0U) {
        return 0U;
    }

    message = input_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);

    if ((inputControlQueueHandle == NULL) ||
        (osMessageQueuePut(inputControlQueueHandle, &message, INPUT_CONTROL_QUEUE_PRIORITY,
                           INPUT_CONTROL_QUEUE_PUT_TIMEOUT) != osOK)) {
        (void)input_control_task_transition_safety_state(INPUT_CONTROL_SAFETY_RELEASE_REQUESTED,
                                                         INPUT_CONTROL_SAFETY_STOPPED);

        inputControlTaskStats.safetyQueueDrops++;
        return 0U;
    }

    return (input_control_task_safety_state() == INPUT_CONTROL_SAFETY_RELEASE_REQUESTED) ? 1U : 0U;
}

input_control_safety_sync_state_t input_control_task_get_safety_sync_state(void) {
    return input_control_task_safety_state();
}

uint32_t input_control_task_capture_command_epoch(void) {
    if (input_control_task_safety_state() != INPUT_CONTROL_SAFETY_RELEASED) {
        return INPUT_CONTROL_INVALID_SAFETY_EPOCH;
    }

    return atomic_load_explicit(&inputCommandSafetyEpoch, memory_order_acquire);
}

void input_control_task_get_stats(input_control_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = inputControlTaskStats;
    }
}

void StartInputControlTask(void* argument) {
    control_command_t message;

    (void)argument;
    (void)input_control_task_initialize_controller(&inputController, input_motor_tb6612_port());

    for (;;) {
        Health_TaskAlive(HEALTH_TASK_INPUT_CONTROL);

        if (input_control_task_safety_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED) {
            message = input_control_task_safety_marker(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);
            (void)input_control_task_process_message(&inputController, &message);
        }

        if (input_control_task_service_tx() == 0U) {
            osDelay(1U);
            continue;
        }

        if (inputControlQueueHandle == NULL) {
            osDelay(INPUT_CONTROL_QUEUE_RETRY_TICKS);
            continue;
        }

        if (osMessageQueueGet(inputControlQueueHandle, &message, NULL, INPUT_CONTROL_QUEUE_WAIT_TICKS) != osOK) {
            continue;
        }

        (void)input_control_task_process_message(&inputController, &message);
    }
}
