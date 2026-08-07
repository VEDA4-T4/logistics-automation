#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorting_control_task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "health_task.h"
#include "sorting_gate_mg90s.h"
#include "sorting_motor_tb6612.h"

void Health_TaskAlive(health_task_id_t id) {
    (void)id;
}

#define FAKE_QUEUE_MAX_ITEMS 16U

typedef struct {
    size_t itemSize;
    uint32_t count;
    uint32_t capacity;
    uint8_t failPut;
    uint8_t items[FAKE_QUEUE_MAX_ITEMS][sizeof(control_command_t)];
} fake_queue_t;

typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
    uint8_t running;
    uint8_t speed;
} fake_motor_t;

typedef struct {
    uint32_t initializeCalls;
    uint32_t moveCalls;
    uart_sorting_destination_t destination;
    uint8_t motionComplete;
} fake_gate_t;

typedef struct {
    uint8_t useProvidedSequence;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} tx_request_t;

_Static_assert(sizeof(tx_request_t) <= sizeof(control_command_t), "fake queue slot is too small");

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle;
osMessageQueueId_t sortingControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;

static fake_queue_t sortingQueue;
static fake_queue_t txQueue;
static sorting_motor_port_t productionMotorPort;
static sorting_gate_port_t productionGatePort;
static uint8_t lastDeviceState;
static uint8_t lastDeviceError;

static sorting_motor_result_t fake_motor_initialize(void* context) {
    fake_motor_t* motor = (fake_motor_t*)context;
    motor->initializeCalls++;
    return SORTING_MOTOR_OK;
}

static sorting_motor_result_t fake_motor_apply(void* context, uint8_t running, uint8_t speed) {
    fake_motor_t* motor = (fake_motor_t*)context;
    motor->applyCalls++;
    motor->running = running;
    motor->speed = speed;
    return SORTING_MOTOR_OK;
}

static sorting_gate_result_t fake_gate_initialize(void* context) {
    fake_gate_t* gate = (fake_gate_t*)context;
    gate->initializeCalls++;
    gate->destination = UART_SORTING_DESTINATION_NONE;
    gate->motionComplete = 0U;
    return SORTING_GATE_OK;
}

static sorting_gate_result_t fake_gate_move(void* context, uart_sorting_destination_t destination) {
    fake_gate_t* gate = (fake_gate_t*)context;
    gate->moveCalls++;
    gate->destination = destination;
    gate->motionComplete = 0U;
    return SORTING_GATE_OK;
}

static sorting_gate_result_t fake_gate_motion_complete(void* context, uint8_t* complete) {
    fake_gate_t* gate = (fake_gate_t*)context;
    *complete = gate->motionComplete;
    return SORTING_GATE_OK;
}

const sorting_motor_port_t* sorting_motor_tb6612_port(void) {
    return &productionMotorPort;
}

const sorting_gate_port_t* sorting_gate_mg90s_port(void) {
    return &productionGatePort;
}

static void fake_queue_reset(fake_queue_t* queue, size_t itemSize) {
    memset(queue, 0, sizeof(*queue));
    queue->itemSize = itemSize;
    queue->capacity = FAKE_QUEUE_MAX_ITEMS;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    fake_queue_t* queue;

    (void)msg_prio;
    (void)timeout;

    if ((mq_id == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }

    queue = (fake_queue_t*)mq_id;

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

static int32_t enqueue_tx(uint8_t useProvidedSequence, uint8_t sequence, uint8_t command, const uint8_t* payload,
                          uint8_t length) {
    tx_request_t request;

    memset(&request, 0, sizeof(request));
    request.useProvidedSequence = useProvidedSequence;
    request.sequence = sequence;
    request.command = command;
    request.length = length;

    if (length != 0U) {
        memcpy(request.payload, payload, length);
    }

    return (osMessageQueuePut(&txQueue, &request, 0U, 0U) == osOK) ? 0 : -1;
}

int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    assert(channel == COMM_TX_CH_SORTING);
    return enqueue_tx(0U, 0U, command, payload, length);
}

int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command, const uint8_t* payload,
                                uint8_t length) {
    assert(channel == COMM_TX_CH_SORTING);
    return enqueue_tx(1U, sequence, command, payload, length);
}

int32_t CommTx_SendUrgentWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                      const uint8_t* payload, uint8_t length) {
    assert(channel == COMM_TX_CH_SORTING);
    return enqueue_tx(1U, sequence, command, payload, length);
}

void CommTx_SetChannelDeviceStatus(comm_tx_channel_t channel, uint8_t device_state, uint8_t error_code) {
    assert(channel == COMM_TX_CH_SORTING);
    lastDeviceState = device_state;
    lastDeviceError = error_code;
}

static control_command_t command(uint8_t sequence, uint8_t commandId, const uint8_t* payload, uint8_t length) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_6;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    message.safetyEpoch = sorting_control_task_capture_command_epoch();
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.sequence = sequence;
    message.frame.command = commandId;
    message.frame.length = length;

    if ((payload != NULL) && (length != 0U)) {
        memcpy(message.frame.payload, payload, length);
    }

    return message;
}

static tx_request_t pop_tx(void) {
    tx_request_t request;
    assert(osMessageQueueGet(&txQueue, &request, NULL, 0U) == osOK);
    return request;
}

static control_command_t pop_sorting_message(void) {
    control_command_t message;
    assert(osMessageQueueGet(&sortingQueue, &message, NULL, 0U) == osOK);
    return message;
}

static void initialize(sorting_control_t* controller, fake_motor_t* motor, fake_gate_t* gate) {
    sorting_motor_port_t motorPort;
    sorting_gate_port_t gatePort;

    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_RELEASED);
    fake_queue_reset(&sortingQueue, sizeof(control_command_t));
    fake_queue_reset(&txQueue, sizeof(tx_request_t));
    lastDeviceState = UART_DEVICE_READY;
    lastDeviceError = UART_ERROR_NONE;
    sortingControlQueueHandle = &sortingQueue;
    memset(motor, 0, sizeof(*motor));
    memset(gate, 0, sizeof(*gate));
    motorPort =
        (sorting_motor_port_t){ .context = motor, .initialize = fake_motor_initialize, .apply = fake_motor_apply };
    gatePort = (sorting_gate_port_t){ .context = gate,
                                      .initialize = fake_gate_initialize,
                                      .move = fake_gate_move,
                                      .motion_complete = fake_gate_motion_complete };

    assert(sorting_control_task_initialize_controller(controller, &motorPort, &gatePort) == SORTING_CONTROL_OK);
    gate->motionComplete = 1U;
    assert(sorting_control_task_service_motion(controller) == SORTING_CONTROL_OK);
    assert(controller->state.gateState == UART_SORTING_GATE_HOME);
}

static void test_response_cache_and_retry(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    control_command_t message;
    tx_request_t response;
    const uint8_t speed[] = { 55U };
    const uint8_t conflictingSpeed[] = { 56U };
    uint32_t applyCalls;

    initialize(&controller, &motor, &gate);
    message = command(10U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_OK);
    response = pop_tx();
    assert(response.useProvidedSequence == 1U);
    assert(response.sequence == 10U);
    assert(response.command == UART_CMD_OPERATION_RESULT);
    assert(response.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(lastDeviceState == UART_DEVICE_STOPPED);
    assert(lastDeviceError == UART_ERROR_NONE);

    applyCalls = motor.applyCalls;
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_OK);
    response = pop_tx();
    assert(response.sequence == 10U);
    assert(motor.applyCalls == applyCalls);

    message = command(10U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, conflictingSpeed, sizeof(conflictingSpeed));
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_SEQUENCE_CONFLICT);
    response = pop_tx();
    assert(response.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(response.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_SEQUENCE);
    assert(motor.applyCalls == applyCalls);

    txQueue.failPut = 1U;
    message = command(11U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_OK);
    assert(txQueue.count == 0U);
    txQueue.failPut = 0U;
    assert(sorting_control_task_service_tx() == 1U);
    response = pop_tx();
    assert(response.sequence == 11U);
    assert(lastDeviceState == UART_DEVICE_RUNNING);
}

static void test_route_return_home_emits_event(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    control_command_t message;
    tx_request_t event;
    const uint8_t route[] = { 0x34U, 0x12U, UART_SORTING_DESTINATION_2 };
    const uint8_t cycle[] = { 0x34U, 0x12U };

    initialize(&controller, &motor, &gate);
    message = command(20U, UART_CMD_SORTING_ROUTE_ITEM, route, sizeof(route));
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_OK);
    (void)pop_tx();

    gate.motionComplete = 1U;
    assert(sorting_control_task_service_motion(&controller) == SORTING_CONTROL_OK);
    assert(txQueue.count == 0U);

    message = command(21U, UART_CMD_SORTING_RETURN_HOME, cycle, sizeof(cycle));
    assert(sorting_control_task_process_message(&controller, &message) == SORTING_CONTROL_OK);
    (void)pop_tx();

    gate.motionComplete = 1U;
    assert(sorting_control_task_service_motion(&controller) == SORTING_CONTROL_OK);
    event = pop_tx();
    assert(event.useProvidedSequence == 0U);
    assert(event.command == UART_CMD_EVENT);
    assert(event.length == UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE);
    assert(event.payload[UART_EVENT_ID_INDEX] == UART_SORTING_EVENT_CYCLE_COMPLETE);
    assert(event.payload[UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX] == 0x34U);
    assert(event.payload[UART_SORTING_EVENT_CYCLE_ID_HIGH_INDEX] == 0x12U);
    assert(event.payload[UART_SORTING_EVENT_DESTINATION_INDEX] == UART_SORTING_DESTINATION_2);
}

static void test_safety_epoch_rejects_old_command(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    control_command_t marker;
    control_command_t oldCommand;
    tx_request_t response;
    const uint8_t speed[] = { 40U };

    initialize(&controller, &motor, &gate);
    oldCommand = command(30U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(sorting_control_task_notify_safety_stop() == 1U);
    assert(sorting_control_task_capture_command_epoch() == UINT32_MAX);

    marker = pop_sorting_message();
    assert(sorting_control_task_process_message(&controller, &marker) == SORTING_CONTROL_OK);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_STOPPED);

    assert(sorting_control_task_notify_safety_release() == 1U);
    marker = pop_sorting_message();
    assert(sorting_control_task_process_message(&controller, &marker) == SORTING_CONTROL_OK);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_RELEASED);

    assert(sorting_control_task_process_message(&controller, &oldCommand) == SORTING_CONTROL_STALE_COMMAND);
    response = pop_tx();
    assert(response.command == UART_CMD_OPERATION_RESULT);
    assert(response.payload[UART_OPERATION_RESULT_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(response.payload[UART_OPERATION_RESULT_ERROR_INDEX] == UART_ERROR_SEQUENCE);
    assert(controller.state.speed == 0U);
}

int main(void) {
    test_response_cache_and_retry();
    test_route_return_home_emits_event();
    test_safety_epoch_rejects_old_command();
    return 0;
}
