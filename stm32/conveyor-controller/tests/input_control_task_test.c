#ifdef NDEBUG
#undef NDEBUG
#endif

#include "input_control_task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "health_task.h"
#include "input_motor_tb6612.h"

void Health_TaskAlive(health_task_id_t id) {
    (void)id;
}

#define FAKE_QUEUE_MAX_ITEMS 16U

typedef struct {
    size_t itemSize;
    uint32_t count;
    uint32_t capacity;
    uint8_t failPut;
    uint8_t lastPriority;
    uint32_t lastTimeout;
    uint8_t items[FAKE_QUEUE_MAX_ITEMS][sizeof(control_command_t)];
} fake_queue_t;

typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
    uint8_t failInitialize;
    uint8_t failNextApply;
    uint8_t requestSafetyStopOnApply;
    uint8_t running;
    uint8_t speed;
} fake_motor_t;

typedef struct {
    app_uart_channel_t destination;
    uart_frame_t frame;
} uart_tx_request_t;

_Static_assert(sizeof(uart_tx_request_t) <= sizeof(control_command_t), "fake queue slot is too small");

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle;
osMessageQueueId_t sortingControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;

static fake_queue_t inputQueue;
static fake_queue_t txQueue;
static input_motor_port_t productionMotorPort;
static uint8_t lastDeviceState;
static uint8_t lastDeviceError;

static input_motor_result_t fake_motor_initialize(void* context) {
    fake_motor_t* motor;

    motor = (fake_motor_t*)context;
    motor->initializeCalls++;
    return (motor->failInitialize != 0U) ? INPUT_MOTOR_ERROR : INPUT_MOTOR_OK;
}

static input_motor_result_t fake_motor_apply(void* context, uint8_t running, uint8_t speed) {
    fake_motor_t* motor;

    motor = (fake_motor_t*)context;
    motor->applyCalls++;

    if (motor->failNextApply != 0U) {
        motor->failNextApply = 0U;
        return INPUT_MOTOR_ERROR;
    }

    if (motor->requestSafetyStopOnApply != 0U) {
        motor->requestSafetyStopOnApply = 0U;
        assert(input_control_task_notify_safety_stop() == 1U);
    }

    motor->running = running;
    motor->speed = speed;
    return INPUT_MOTOR_OK;
}

static input_motor_port_t fake_motor_port(fake_motor_t* motor) {
    input_motor_port_t port;

    port.context = motor;
    port.initialize = fake_motor_initialize;
    port.apply = fake_motor_apply;
    return port;
}

const input_motor_port_t* input_motor_tb6612_port(void) {
    return &productionMotorPort;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    fake_queue_t* queue;

    if ((mq_id == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }

    queue = (fake_queue_t*)mq_id;
    queue->lastPriority = msg_prio;
    queue->lastTimeout = timeout;

    if ((queue->failPut != 0U) || (queue->count >= queue->capacity)) {
        return osErrorResource;
    }

    memcpy(queue->items[queue->count], msg_ptr, queue->itemSize);
    queue->count++;
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void* msg_ptr, uint8_t* msg_prio, uint32_t timeout) {
    fake_queue_t* queue;

    (void)msg_prio;
    (void)timeout;

    if ((mq_id == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }

    queue = (fake_queue_t*)mq_id;

    if (queue->count == 0U) {
        return osErrorResource;
    }

    memcpy(msg_ptr, queue->items[0], queue->itemSize);
    queue->count--;

    if (queue->count != 0U) {
        memmove(queue->items[0], queue->items[1], queue->count * sizeof(queue->items[0]));
    }

    return osOK;
}

osStatus_t osDelay(uint32_t ticks) {
    (void)ticks;
    return osOK;
}

static int32_t fake_comm_tx_send_with_sequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                               const uint8_t* payload, uint8_t length) {
    uart_tx_request_t request;

    if ((channel >= COMM_TX_CH_COUNT) || (length > UART_MAX_PAYLOAD_SIZE) || ((length != 0U) && (payload == NULL))) {
        return -2;
    }

    memset(&request, 0, sizeof(request));
    request.destination = (channel == COMM_TX_CH_INPUT) ? APP_UART_CHANNEL_1 : APP_UART_CHANNEL_6;
    request.frame.version = UART_PROTOCOL_VERSION;
    request.frame.sequence = sequence;
    request.frame.command = command;
    request.frame.length = length;

    if (length != 0U) {
        memcpy(request.frame.payload, payload, length);
    }

    return (osMessageQueuePut(&txQueue, &request, 0U, 0U) == osOK) ? 0 : -3;
}

int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command, const uint8_t* payload,
                                uint8_t length) {
    return fake_comm_tx_send_with_sequence(channel, sequence, command, payload, length);
}

int32_t CommTx_SendUrgentWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                      const uint8_t* payload, uint8_t length) {
    return fake_comm_tx_send_with_sequence(channel, sequence, command, payload, length);
}

void CommTx_SetChannelDeviceStatus(comm_tx_channel_t channel, uint8_t device_state, uint8_t error_code) {
    assert(channel == COMM_TX_CH_INPUT);
    lastDeviceState = device_state;
    lastDeviceError = error_code;
}

static void fake_queue_reset(fake_queue_t* queue, size_t itemSize) {
    memset(queue, 0, sizeof(*queue));
    queue->itemSize = itemSize;
    queue->capacity = FAKE_QUEUE_MAX_ITEMS;
}

static void reset_queues(void) {
    fake_queue_reset(&inputQueue, sizeof(control_command_t));
    fake_queue_reset(&txQueue, sizeof(uart_tx_request_t));

    uartRxQueueHandle = NULL;
    inputControlQueueHandle = &inputQueue;
    sortingControlQueueHandle = NULL;
    safetyCommandQueueHandle = NULL;
    lastDeviceState = UART_DEVICE_READY;
    lastDeviceError = UART_ERROR_NONE;
}

static control_command_t command_message(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_1;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.sequence = sequence;
    message.frame.command = command;
    message.frame.length = length;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    message.safetyEpoch = input_control_task_capture_command_epoch();

    if ((payload != NULL) && (length != 0U)) {
        memcpy(message.frame.payload, payload, length);
    }

    return message;
}

static void initialize_controller(input_control_t* controller, fake_motor_t* motor) {
    input_motor_port_t port;

    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
    reset_queues();
    memset(motor, 0, sizeof(*motor));
    port = fake_motor_port(motor);

    assert(input_control_task_initialize_controller(controller, &port) == INPUT_CONTROL_OK);
    assert(controller->state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
}

static uart_tx_request_t pop_tx(void) {
    uart_tx_request_t request;

    assert(osMessageQueueGet(&txQueue, &request, NULL, 0U) == osOK);
    return request;
}

static control_command_t pop_input(void) {
    control_command_t message;

    assert(osMessageQueueGet(&inputQueue, &message, NULL, 0U) == osOK);
    return message;
}

static void test_invalid_motor_port_is_rejected(void) {
    input_control_t controller;
    input_motor_port_t invalidPort;

    reset_queues();
    memset(&controller, 0xA5, sizeof(controller));
    memset(&invalidPort, 0, sizeof(invalidPort));

    assert(input_control_task_initialize_controller(&controller, &invalidPort) == INPUT_CONTROL_INVALID_ARGUMENT);
}

static void test_prestart_safety_request_applies_final_state(void) {
    input_control_t controller;
    fake_motor_t motor;
    input_motor_port_t port;
    control_command_t message;

    reset_queues();
    memset(&motor, 0, sizeof(motor));
    port = fake_motor_port(&motor);

    assert(input_control_task_notify_safety_stop() == 1U);
    assert(input_control_task_initialize_controller(&controller, &port) == INPUT_CONTROL_OK);
    assert(controller.safetyStopLatched == 1U);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_RELEASE);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(controller.state.lastError == UART_ERROR_NONE);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
}

static void test_status_response_preserves_sequence(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;

    initialize_controller(&controller, &motor);
    controller.state.speed = 42U;
    message = command_message(0x37U, UART_CMD_INPUT_CONVEYOR_GET_STATUS, NULL, 0U);

    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();

    assert(request.destination == APP_UART_CHANNEL_1);
    assert(request.frame.version == UART_PROTOCOL_VERSION);
    assert(request.frame.sequence == 0x37U);
    assert(request.frame.command == UART_CMD_RESPONSE);
    assert(request.frame.length == UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE);
    assert(request.frame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(request.frame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_INPUT_CONVEYOR_GET_STATUS);
    assert(request.frame.payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_NONE);
    assert(request.frame.payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] == UART_INPUT_CONVEYOR_STOPPED);
    assert(request.frame.payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] == 42U);
    assert(request.frame.crc == 0U);
    assert(txQueue.lastPriority == 0U);
    assert(txQueue.lastTimeout == 0U);
}

static void test_operation_results(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    const uint8_t speed[] = { 65U };

    initialize_controller(&controller, &motor);

    message = command_message(0x40U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_SPEED_NOT_CONFIGURED);
    request = pop_tx();
    assert(request.frame.sequence == 0x40U);
    assert(request.frame.command == UART_CMD_OPERATION_RESULT);
    assert(request.frame.length == UART_OPERATION_RESULT_PAYLOAD_SIZE);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_INVALID_PAYLOAD);

    message = command_message(0x41U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x41U);
    assert(request.frame.command == UART_CMD_OPERATION_RESULT);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_NONE);

    message = command_message(0x42U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x42U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_RUNNING);
    assert(lastDeviceState == UART_DEVICE_RUNNING);
    assert(lastDeviceError == UART_ERROR_NONE);

    message = command_message(0x43U, UART_CMD_INPUT_CONVEYOR_STOP, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x43U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(lastDeviceState == UART_DEVICE_STOPPED);
}

static void test_motor_error_reports_result(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    const uint8_t speed[] = { 50U };

    initialize_controller(&controller, &motor);

    message = command_message(0x50U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();

    motor.failNextApply = 1U;
    message = command_message(0x51U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_MOTOR_ERROR);
    request = pop_tx();

    assert(request.frame.sequence == 0x51U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_MOTOR);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_FAULT);
    assert(controller.state.lastError == UART_ERROR_MOTOR);
    assert(lastDeviceState == UART_DEVICE_ERROR);
    assert(lastDeviceError == UART_ERROR_MOTOR);
}

static void test_failed_tx_is_retried_before_next_command(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    input_control_task_stats_t before;
    input_control_task_stats_t after;
    const uint8_t speed[] = { 45U };
    uint32_t applyCallsAfterSpeed;

    initialize_controller(&controller, &motor);
    input_control_task_get_stats(&before);

    txQueue.failPut = 1U;
    message = command_message(0x60U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.speed == 45U);
    assert(txQueue.count == 0U);
    applyCallsAfterSpeed = motor.applyCalls;

    message = command_message(0x61U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_TX_BUSY);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(motor.applyCalls == applyCallsAfterSpeed);

    txQueue.failPut = 0U;
    assert(input_control_task_service_tx() == 1U);
    request = pop_tx();
    assert(request.frame.sequence == 0x60U);
    assert(request.frame.command == UART_CMD_OPERATION_RESULT);

    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x61U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_RUNNING);

    input_control_task_get_stats(&after);
    assert(after.txQueueDrops == (before.txQueueDrops + 2U));
    assert(after.txRetryAttempts == (before.txRetryAttempts + 2U));
}

static void test_duplicate_sequence_replays_response_without_reexecution(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    input_control_task_stats_t before;
    input_control_task_stats_t after;
    const uint8_t firstSpeed[] = { 45U };
    const uint8_t conflictingSpeed[] = { 80U };
    uint32_t applyCallsAfterFirstExecution;

    initialize_controller(&controller, &motor);
    input_control_task_get_stats(&before);

    message = command_message(0x63U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, firstSpeed, sizeof(firstSpeed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x63U);
    applyCallsAfterFirstExecution = motor.applyCalls;

    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x63U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(motor.applyCalls == applyCallsAfterFirstExecution);
    assert(controller.state.speed == 45U);

    message = command_message(0x63U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, conflictingSpeed, sizeof(conflictingSpeed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_SEQUENCE_CONFLICT);
    request = pop_tx();
    assert(request.frame.sequence == 0x63U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_SEQUENCE);
    assert(motor.applyCalls == applyCallsAfterFirstExecution);
    assert(controller.state.speed == 45U);

    input_control_task_get_stats(&after);
    assert(after.duplicateCommands == (before.duplicateCommands + 1U));
    assert(after.sequenceConflicts == (before.sequenceConflicts + 1U));
}

static void test_safety_stop_precedes_stale_commands_and_release_is_fifo(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    const uint8_t speed[] = { 70U };

    initialize_controller(&controller, &motor);
    message = command_message(0x70U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();
    message = command_message(0x71U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();

    assert(input_control_task_notify_safety_stop() == 1U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED);
    assert(inputQueue.lastPriority == 0U);
    assert(inputQueue.lastTimeout == 0U);

    message = command_message(0x72U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_STALE_COMMAND);
    request = pop_tx();
    assert(request.frame.sequence == 0x72U);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(controller.safetyStopLatched == 1U);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(controller.state.speed == 70U);
    assert(motor.running == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    message = command_message(0x73U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(osMessageQueuePut(&inputQueue, &message, 0U, 0U) == osOK);
    assert(input_control_task_notify_safety_release() == 1U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASE_REQUESTED);
    message = command_message(0x74U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(osMessageQueuePut(&inputQueue, &message, 0U, 0U) == osOK);
    assert(inputQueue.count == 4U);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(txQueue.count == 0U);

    message = pop_input();
    assert(message.frame.command == UART_CMD_INPUT_CONVEYOR_START);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_STALE_COMMAND);
    request = pop_tx();
    assert(request.frame.sequence == 0x73U);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(motor.running == 0U);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_RELEASE);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
    assert(controller.safetyStopLatched == 0U);
    assert(controller.state.lastError == UART_ERROR_NONE);
    assert(controller.state.speed == 70U);
    assert(motor.running == 0U);

    message = pop_input();
    assert(message.frame.command == UART_CMD_INPUT_CONVEYOR_START);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_STALE_COMMAND);
    request = pop_tx();
    assert(request.frame.sequence == 0x74U);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_SEQUENCE);
    assert(motor.running == 0U);

    message = command_message(0x75U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    request = pop_tx();
    assert(request.frame.sequence == 0x75U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(motor.running == 1U);
    assert(motor.speed == 70U);
}

static void test_repeated_stop_wins_release_race(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;

    initialize_controller(&controller, &motor);

    assert(input_control_task_notify_safety_stop() == 1U);
    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_RELEASE);
    motor.requestSafetyStopOnApply = 1U;
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);

    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);
    assert(controller.safetyStopLatched == 1U);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(motor.running == 0U);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
}

static void test_stop_during_start_cannot_publish_running_success(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    const uint8_t speed[] = { 80U };

    initialize_controller(&controller, &motor);
    message = command_message(0x76U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();

    motor.requestSafetyStopOnApply = 1U;
    message = command_message(0x77U, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_STALE_COMMAND);
    request = pop_tx();

    assert(request.frame.sequence == 0x77U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_FAULT);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(controller.safetyStopLatched == 1U);
    assert(motor.running == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
}

static void test_stop_during_set_speed_discards_stale_speed(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uart_tx_request_t request;
    const uint8_t initialSpeed[] = { 35U };
    const uint8_t staleSpeed[] = { 90U };

    initialize_controller(&controller, &motor);

    message = command_message(0x78U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, initialSpeed, sizeof(initialSpeed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();

    motor.requestSafetyStopOnApply = 1U;
    message = command_message(0x79U, UART_CMD_INPUT_CONVEYOR_SET_SPEED, staleSpeed, sizeof(staleSpeed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_STALE_COMMAND);
    request = pop_tx();

    assert(request.frame.sequence == 0x79U);
    assert(request.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(request.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(controller.state.speed == 35U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_FAULT);
    assert(motor.running == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    message = pop_input();
    assert(message.kind == APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
    assert(controller.state.speed == 35U);

    message = command_message(0x7AU, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();
    assert(motor.running == 1U);
    assert(motor.speed == 35U);
}

static void test_stop_output_failure_blocks_release_and_reports_motor_error(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    const uint8_t speed[] = { 60U };

    initialize_controller(&controller, &motor);

    message = command_message(0x7BU, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();
    message = command_message(0x7CU, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    (void)pop_tx();
    assert(motor.running == 1U);

    assert(input_control_task_notify_safety_stop() == 1U);
    message = pop_input();
    motor.failNextApply = 1U;
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_MOTOR_ERROR);

    assert(controller.safetyStopLatched == 1U);
    assert(controller.state.lastError == UART_ERROR_MOTOR);
    assert(controller.motorInitialized == 0U);
    assert(motor.running == 1U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED);
    assert(input_control_task_notify_safety_release() == 0U);

    memset(&message, 0, sizeof(message));
    message.kind = APP_CONTROL_MESSAGE_SAFETY_STOP;
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);

    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(controller.motorInitialized == 1U);
    assert(motor.running == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);

    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(controller.state.lastError == UART_ERROR_NONE);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
}

static void test_safety_queue_failure_remains_fail_safe(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    input_control_task_stats_t before;
    input_control_task_stats_t after;

    initialize_controller(&controller, &motor);
    input_control_task_get_stats(&before);

    inputQueue.failPut = 1U;
    assert(input_control_task_notify_safety_stop() == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED);

    inputQueue.failPut = 0U;
    message = command_message(0x80U, UART_CMD_INPUT_CONVEYOR_GET_STATUS, NULL, 0U);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.safetyStopLatched == 1U);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);
    (void)pop_tx();

    inputQueue.failPut = 1U;
    assert(input_control_task_notify_safety_release() == 0U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);
    assert(controller.safetyStopLatched == 1U);

    inputQueue.failPut = 0U;
    assert(input_control_task_notify_safety_release() == 1U);
    message = pop_input();
    assert(message.frame.command == UART_CMD_RESET_DEVICE);
    assert(input_control_task_process_message(&controller, &message) == INPUT_CONTROL_OK);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
    assert(controller.safetyStopLatched == 0U);
    assert(motor.running == 0U);

    input_control_task_get_stats(&after);
    assert(after.safetyQueueDrops == (before.safetyQueueDrops + 2U));
}

int main(void) {
    test_invalid_motor_port_is_rejected();
    test_prestart_safety_request_applies_final_state();
    test_status_response_preserves_sequence();
    test_operation_results();
    test_motor_error_reports_result();
    test_failed_tx_is_retried_before_next_command();
    test_duplicate_sequence_replays_response_without_reexecution();
    test_safety_stop_precedes_stale_commands_and_release_is_fifo();
    test_repeated_stop_wins_release_race();
    test_stop_during_start_cannot_publish_running_success();
    test_stop_during_set_speed_discards_stale_speed();
    test_stop_output_failure_blocks_release_and_reports_motor_error();
    test_safety_queue_failure_remains_fail_safe();
    return 0;
}
