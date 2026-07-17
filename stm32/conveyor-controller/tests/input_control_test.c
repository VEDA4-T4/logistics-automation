#ifdef NDEBUG
#undef NDEBUG
#endif

#include "input_control.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
    uint8_t failInitialize;
    uint8_t failNextApply;
    uint8_t running;
    uint8_t speed;
} fake_motor_t;

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

static control_command_t command_message(app_uart_channel_t source, uint8_t command, const uint8_t* payload,
                                         uint8_t length) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = source;
    message.frame.sequence = 0xA5U;
    message.frame.command = command;
    message.frame.length = length;

    if ((payload != NULL) && (length != 0U)) {
        memcpy(message.frame.payload, payload, length);
    }

    return message;
}

static void initialize_controller(input_control_t* controller, fake_motor_t* motor) {
    input_motor_port_t port;

    memset(motor, 0, sizeof(*motor));
    port = fake_motor_port(motor);

    assert(input_control_init(controller, &port) == INPUT_CONTROL_OK);
    assert(motor->initializeCalls == 1U);
    assert(motor->applyCalls == 1U);
    assert(motor->running == 0U);
    assert(motor->speed == 0U);
    assert(controller->state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(controller->state.speed == 0U);
    assert(controller->state.lastError == UART_ERROR_NONE);
}

static void test_speed_start_stop(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uint32_t applyCalls;
    const uint8_t speed[] = { 60U };

    initialize_controller(&controller, &motor);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.speed == 60U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(motor.running == 0U);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_RUNNING);
    assert(motor.running == 1U);
    assert(motor.speed == 60U);

    applyCalls = motor.applyCalls;
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(motor.applyCalls == (applyCalls + 1U));

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_STOP, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(motor.running == 0U);
}

static void test_zero_speed_never_reports_running(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uint32_t applyCalls;
    const uint8_t speed[] = { 50U };
    const uint8_t zeroSpeed[] = { 0U };

    initialize_controller(&controller, &motor);
    applyCalls = motor.applyCalls;

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_SPEED_NOT_CONFIGURED);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(motor.applyCalls == applyCalls);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, zeroSpeed, sizeof(zeroSpeed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.speed == 0U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(motor.running == 0U);
}

static void test_invalid_commands_do_not_mutate_state(void) {
    input_control_t controller;
    input_control_state_t previous;
    fake_motor_t motor;
    control_command_t message;
    uint32_t applyCalls;
    const uint8_t invalidSpeed[] = { 101U };
    const uint8_t unexpectedPayload[] = { 1U };

    initialize_controller(&controller, &motor);
    previous = controller.state;
    applyCalls = motor.applyCalls;

    message =
        command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, invalidSpeed, sizeof(invalidSpeed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_INVALID_PAYLOAD);
    assert(memcmp(&controller.state, &previous, sizeof(previous)) == 0);
    assert(motor.applyCalls == applyCalls);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, unexpectedPayload,
                              sizeof(unexpectedPayload));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_INVALID_PAYLOAD);
    assert(memcmp(&controller.state, &previous, sizeof(previous)) == 0);

    message = command_message(APP_UART_CHANNEL_6, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_INVALID_SOURCE);
    assert(memcmp(&controller.state, &previous, sizeof(previous)) == 0);

    message = command_message(APP_UART_CHANNEL_1, 0x15U, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_UNSUPPORTED_COMMAND);
    assert(memcmp(&controller.state, &previous, sizeof(previous)) == 0);

    message = command_message(APP_UART_CHANNEL_1, 0x7FU, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_UNSUPPORTED_COMMAND);
    assert(memcmp(&controller.state, &previous, sizeof(previous)) == 0);
}

static void test_status_payload(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uint32_t applyCalls;
    uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE];

    initialize_controller(&controller, &motor);
    controller.state.speed = 42U;
    applyCalls = motor.applyCalls;

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_GET_STATUS, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(motor.applyCalls == applyCalls);

    input_control_build_status_payload(&controller.state, payload);
    assert(payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_SUCCESS);
    assert(payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_INPUT_CONVEYOR_GET_STATUS);
    assert(payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_NONE);
    assert(payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] == UART_INPUT_CONVEYOR_STOPPED);
    assert(payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] == 42U);
}

static void test_running_reset_stops_and_preserves_speed(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    const uint8_t speed[] = { 75U };

    initialize_controller(&controller, &motor);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(motor.running == 1U);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONTROL_RESET, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(controller.state.speed == 75U);
    assert(controller.state.lastError == UART_ERROR_NONE);
    assert(motor.running == 0U);
    assert(motor.speed == 75U);
}

static void test_motor_failure_status_and_recovery(void) {
    input_control_t controller;
    fake_motor_t motor;
    control_command_t message;
    uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE];
    const uint8_t speed[] = { 50U };

    initialize_controller(&controller, &motor);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed, sizeof(speed));
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);

    motor.failNextApply = 1U;
    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_MOTOR_ERROR);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_FAULT);
    assert(controller.state.lastError == UART_ERROR_MOTOR);
    assert(controller.motorInitialized == 0U);
    assert(motor.running == 0U);
    assert(motor.speed == 0U);

    input_control_build_status_payload(&controller.state, payload);
    assert(payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_ERROR);
    assert(payload[UART_RESPONSE_ERROR_INDEX] == UART_ERROR_MOTOR);
    assert(payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] == UART_INPUT_CONVEYOR_FAULT);

    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_FAULT_LATCHED);

    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONTROL_RESET, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(motor.initializeCalls == 2U);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
    assert(controller.state.speed == 50U);
    assert(controller.state.lastError == UART_ERROR_NONE);
}

static void test_initialization_failure_can_recover(void) {
    input_control_t controller;
    fake_motor_t motor;
    input_motor_port_t port;
    control_command_t message;

    memset(&motor, 0, sizeof(motor));
    motor.failInitialize = 1U;
    port = fake_motor_port(&motor);

    assert(input_control_init(&controller, &port) == INPUT_CONTROL_MOTOR_ERROR);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_FAULT);
    assert(controller.state.lastError == UART_ERROR_MOTOR);

    motor.failInitialize = 0U;
    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONTROL_RESET, NULL, 0U);
    assert(input_control_process_command(&controller, &message) == INPUT_CONTROL_OK);
    assert(controller.state.conveyorState == UART_INPUT_CONVEYOR_STOPPED);
}

static void test_invalid_arguments(void) {
    input_control_t controller;
    fake_motor_t motor;
    input_motor_port_t port;
    control_command_t message;

    memset(&motor, 0, sizeof(motor));
    port = fake_motor_port(&motor);
    message = command_message(APP_UART_CHANNEL_1, UART_CMD_INPUT_CONVEYOR_START, NULL, 0U);

    assert(input_control_init(NULL, &port) == INPUT_CONTROL_INVALID_ARGUMENT);
    assert(input_control_init(&controller, NULL) == INPUT_CONTROL_INVALID_ARGUMENT);
    assert(input_control_process_command(NULL, &message) == INPUT_CONTROL_INVALID_ARGUMENT);
    assert(input_control_process_command(&controller, NULL) == INPUT_CONTROL_INVALID_ARGUMENT);
}

int main(void) {
    test_speed_start_stop();
    test_zero_speed_never_reports_running();
    test_invalid_commands_do_not_mutate_state();
    test_status_payload();
    test_running_reset_stops_and_preserves_speed();
    test_motor_failure_status_and_recovery();
    test_initialization_failure_can_recover();
    test_invalid_arguments();
    return 0;
}
