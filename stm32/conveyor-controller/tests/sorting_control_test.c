#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sorting_control.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
    uint8_t running;
    uint8_t speed;
    uint8_t failApply;
} fake_motor_t;

typedef struct {
    uint32_t initializeCalls;
    uint32_t moveCalls;
    uart_sorting_destination_t destination;
    uint8_t motionComplete;
    uint8_t failMove;
} fake_gate_t;

static sorting_motor_result_t fake_motor_initialize(void* context) {
    fake_motor_t* motor = (fake_motor_t*)context;
    motor->initializeCalls++;
    return SORTING_MOTOR_OK;
}

static sorting_motor_result_t fake_motor_apply(void* context, uint8_t running, uint8_t speed) {
    fake_motor_t* motor = (fake_motor_t*)context;
    motor->applyCalls++;

    if (motor->failApply != 0U) {
        motor->failApply = 0U;
        return SORTING_MOTOR_ERROR;
    }

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

    if (gate->failMove != 0U) {
        gate->failMove = 0U;
        return SORTING_GATE_ERROR;
    }

    gate->destination = destination;
    gate->motionComplete = 0U;
    return SORTING_GATE_OK;
}

static sorting_gate_result_t fake_gate_motion_complete(void* context, uint8_t* complete) {
    fake_gate_t* gate = (fake_gate_t*)context;
    *complete = gate->motionComplete;
    return SORTING_GATE_OK;
}

static control_command_t command(uint8_t sequence, uint8_t commandId, const uint8_t* payload, uint8_t length) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_6;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.sequence = sequence;
    message.frame.command = commandId;
    message.frame.length = length;

    if ((payload != NULL) && (length != 0U)) {
        memcpy(message.frame.payload, payload, length);
    }

    return message;
}

static void initialize(sorting_control_t* controller, fake_motor_t* motor, fake_gate_t* gate) {
    sorting_motor_port_t motorPort;
    sorting_gate_port_t gatePort;
    sorting_cycle_complete_t completion;

    memset(motor, 0, sizeof(*motor));
    memset(gate, 0, sizeof(*gate));
    motorPort =
        (sorting_motor_port_t){ .context = motor, .initialize = fake_motor_initialize, .apply = fake_motor_apply };
    gatePort = (sorting_gate_port_t){ .context = gate,
                                      .initialize = fake_gate_initialize,
                                      .move = fake_gate_move,
                                      .motion_complete = fake_gate_motion_complete };

    assert(sorting_control_init(controller, &motorPort, &gatePort) == SORTING_CONTROL_OK);
    assert(controller->state.gateState == UART_SORTING_GATE_RETURNING);
    gate->motionComplete = 1U;
    assert(sorting_control_service_motion(controller, &completion) == SORTING_CONTROL_OK);
    assert(completion.valid == 0U);
    assert(controller->state.gateState == UART_SORTING_GATE_HOME);
}

static void test_conveyor_commands(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    control_command_t message;
    const uint8_t speed[] = { 60U };
    uint8_t status[UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE];

    initialize(&controller, &motor, &gate);
    message = command(1U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_SPEED_NOT_CONFIGURED);

    message = command(2U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    assert(motor.speed == 60U);
    assert(motor.running == 0U);

    message = command(3U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    assert(motor.running == 1U);

    sorting_control_build_conveyor_status_payload(&controller.state, status);
    assert(status[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(status[UART_SORTING_CONVEYOR_STATUS_STATE_INDEX] == UART_SORTING_CONVEYOR_RUNNING);
    assert(status[UART_SORTING_CONVEYOR_STATUS_SPEED_INDEX] == 60U);
}

static void test_route_return_home_and_completion(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    sorting_cycle_complete_t completion;
    control_command_t message;
    const uint8_t route[] = { 0x34U, 0x12U, UART_SORTING_DESTINATION_3 };
    const uint8_t wrongCycle[] = { 0x35U, 0x12U };
    const uint8_t cycle[] = { 0x34U, 0x12U };
    uint8_t status[UART_SORTING_STATUS_PAYLOAD_SIZE];

    initialize(&controller, &motor, &gate);
    message = command(4U, UART_CMD_SORTING_ROUTE_ITEM, route, sizeof(route));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    assert(controller.state.gateState == UART_SORTING_GATE_MOVING);
    assert(controller.state.activeCycleId == 0x1234U);
    assert(gate.destination == UART_SORTING_DESTINATION_3);

    message = command(5U, UART_CMD_SORTING_RETURN_HOME, cycle, sizeof(cycle));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_BUSY);
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_MOTION_PENDING);
    gate.motionComplete = 1U;
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_OK);
    assert(controller.state.gateState == UART_SORTING_GATE_WAIT_ITEM);

    message = command(6U, UART_CMD_SORTING_RETURN_HOME, wrongCycle, sizeof(wrongCycle));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_CYCLE_MISMATCH);

    message = command(7U, UART_CMD_SORTING_RETURN_HOME, cycle, sizeof(cycle));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    assert(controller.state.gateState == UART_SORTING_GATE_RETURNING);
    assert(gate.destination == UART_SORTING_DESTINATION_NONE);

    gate.motionComplete = 1U;
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_OK);
    assert(completion.valid == 1U);
    assert(completion.cycleId == 0x1234U);
    assert(completion.destination == UART_SORTING_DESTINATION_3);
    assert(controller.state.gateState == UART_SORTING_GATE_HOME);
    assert(controller.state.activeCycleId == 0U);

    sorting_control_build_status_payload(&controller.state, status);
    assert(status[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(status[UART_SORTING_STATUS_GATE_STATE_INDEX] == UART_SORTING_GATE_HOME);
    assert(status[UART_SORTING_STATUS_CYCLE_ID_LOW_INDEX] == 0U);
    assert(status[UART_SORTING_STATUS_DESTINATION_INDEX] == UART_SORTING_DESTINATION_NONE);
}

static void test_cancel_does_not_complete_cycle(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    sorting_cycle_complete_t completion;
    control_command_t message;
    const uint8_t route[] = { 0x78U, 0x56U, UART_SORTING_DESTINATION_1 };
    const uint8_t cycle[] = { 0x78U, 0x56U };

    initialize(&controller, &motor, &gate);
    message = command(7U, UART_CMD_SORTING_ROUTE_ITEM, route, sizeof(route));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);

    message = command(8U, UART_CMD_SORTING_CANCEL, cycle, sizeof(cycle));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    gate.motionComplete = 1U;
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_OK);
    assert(completion.valid == 0U);
    assert(controller.state.activeCycleId == 0U);
}

static void test_safety_stop_waits_for_home(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    control_command_t message;
    sorting_cycle_complete_t completion;
    const uint8_t speed[] = { 50U };
    const uint8_t route[] = { 1U, 0U, UART_SORTING_DESTINATION_2 };

    initialize(&controller, &motor, &gate);
    message = command(9U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    message = command(10U, UART_CMD_SORTING_CONVEYOR_START, NULL, 0U);
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    message = command(11U, UART_CMD_SORTING_ROUTE_ITEM, route, sizeof(route));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);

    assert(sorting_control_handle_safety_stop(&controller) == SORTING_CONTROL_MOTION_PENDING);
    assert(motor.running == 0U);
    assert(controller.state.activeCycleId == 0U);
    assert(controller.safetyStopLatched == 1U);
    gate.motionComplete = 1U;
    assert(sorting_control_handle_safety_stop(&controller) == SORTING_CONTROL_OK);
    assert(controller.state.gateState == UART_SORTING_GATE_HOME);
    assert(controller.state.lastError == UART_ERROR_EMERGENCY_STOP);
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_OK);

    assert(sorting_control_handle_safety_release(&controller) == SORTING_CONTROL_OK);
    assert(controller.safetyStopLatched == 0U);
    assert(controller.state.conveyorState == UART_SORTING_CONVEYOR_STOPPED);
    assert(controller.state.lastError == UART_ERROR_NONE);
}

static void test_reset_recovers_driver_failures(void) {
    sorting_control_t controller;
    fake_motor_t motor;
    fake_gate_t gate;
    sorting_cycle_complete_t completion;
    control_command_t message;
    const uint8_t speed[] = { 50U };
    const uint8_t route[] = { 1U, 0U, UART_SORTING_DESTINATION_1 };

    initialize(&controller, &motor, &gate);
    motor.failApply = 1U;
    message = command(12U, UART_CMD_SORTING_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_MOTOR_ERROR);
    assert(controller.state.lastError == UART_ERROR_MOTOR);

    message = command(13U, UART_CMD_SORTING_RESET, NULL, 0U);
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    gate.motionComplete = 1U;
    assert(sorting_control_service_motion(&controller, &completion) == SORTING_CONTROL_OK);
    assert(controller.state.lastError == UART_ERROR_NONE);

    gate.failMove = 1U;
    message = command(14U, UART_CMD_SORTING_ROUTE_ITEM, route, sizeof(route));
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_GATE_ERROR);
    assert(controller.state.lastError == UART_ERROR_SERVO);

    message = command(15U, UART_CMD_SORTING_RESET, NULL, 0U);
    assert(sorting_control_process_command(&controller, &message) == SORTING_CONTROL_OK);
    assert(controller.state.lastError == UART_ERROR_NONE);
}

int main(void) {
    test_conveyor_commands();
    test_route_return_home_and_completion();
    test_cancel_does_not_complete_cycle();
    test_safety_stop_waits_for_home();
    test_reset_recovers_driver_failures();
    return 0;
}
