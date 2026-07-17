#include "input_control.h"

#include <string.h>

static void input_control_set_motor_fault(input_control_t* controller) {
    controller->state.conveyorState = UART_INPUT_CONVEYOR_FAULT;
    controller->state.lastError = UART_ERROR_MOTOR;
    controller->motorInitialized = 0U;
}

static input_control_result_t input_control_apply_state(input_control_t* controller,
                                                        const input_control_state_t* candidate) {
    uint8_t running;

    running = (candidate->conveyorState == UART_INPUT_CONVEYOR_RUNNING) ? 1U : 0U;

    if ((controller->motorInitialized == 0U) ||
        (controller->motor.apply(controller->motor.context, running, candidate->speed) != INPUT_MOTOR_OK)) {
        (void)controller->motor.apply(controller->motor.context, 0U, 0U);
        input_control_set_motor_fault(controller);
        return INPUT_CONTROL_MOTOR_ERROR;
    }

    controller->state = *candidate;
    return INPUT_CONTROL_OK;
}

static input_control_result_t input_control_reset(input_control_t* controller) {
    input_control_state_t candidate;

    if (controller->motorInitialized == 0U) {
        if (controller->motor.initialize(controller->motor.context) != INPUT_MOTOR_OK) {
            input_control_set_motor_fault(controller);
            return INPUT_CONTROL_MOTOR_ERROR;
        }

        controller->motorInitialized = 1U;
    }

    candidate = controller->state;
    candidate.conveyorState = UART_INPUT_CONVEYOR_STOPPED;
    candidate.lastError = UART_ERROR_NONE;

    return input_control_apply_state(controller, &candidate);
}

input_control_result_t input_control_init(input_control_t* controller, const input_motor_port_t* motor) {
    if ((controller == NULL) || (motor == NULL) || (motor->initialize == NULL) || (motor->apply == NULL)) {
        return INPUT_CONTROL_INVALID_ARGUMENT;
    }

    memset(controller, 0, sizeof(*controller));
    controller->motor = *motor;
    controller->state.conveyorState = UART_INPUT_CONVEYOR_STOPPED;
    controller->state.lastError = UART_ERROR_NONE;

    if (controller->motor.initialize(controller->motor.context) != INPUT_MOTOR_OK) {
        input_control_set_motor_fault(controller);
        return INPUT_CONTROL_MOTOR_ERROR;
    }

    controller->motorInitialized = 1U;

    if (controller->motor.apply(controller->motor.context, 0U, 0U) != INPUT_MOTOR_OK) {
        input_control_set_motor_fault(controller);
        return INPUT_CONTROL_MOTOR_ERROR;
    }

    return INPUT_CONTROL_OK;
}

input_control_result_t input_control_process_command(input_control_t* controller, const control_command_t* message) {
    const uart_frame_t* frame;
    input_control_state_t candidate;

    if ((controller == NULL) || (message == NULL)) {
        return INPUT_CONTROL_INVALID_ARGUMENT;
    }

    if (message->source != APP_UART_CHANNEL_1) {
        return INPUT_CONTROL_INVALID_SOURCE;
    }

    frame = &message->frame;

    if (UART_IS_VALID_INPUT_COMMAND(frame->command) == 0U) {
        return INPUT_CONTROL_UNSUPPORTED_COMMAND;
    }

    if (UART_IS_VALID_INPUT_PAYLOAD(frame->command, frame->payload, frame->length) == 0U) {
        return INPUT_CONTROL_INVALID_PAYLOAD;
    }

    candidate = controller->state;

    switch (frame->command) {
        case UART_CMD_INPUT_CONVEYOR_START:
            if (candidate.conveyorState == UART_INPUT_CONVEYOR_FAULT) {
                return INPUT_CONTROL_FAULT_LATCHED;
            }

            if (candidate.speed == 0U) {
                return INPUT_CONTROL_SPEED_NOT_CONFIGURED;
            }

            candidate.conveyorState = UART_INPUT_CONVEYOR_RUNNING;
            return input_control_apply_state(controller, &candidate);

        case UART_CMD_INPUT_CONVEYOR_STOP:
            if (candidate.conveyorState == UART_INPUT_CONVEYOR_FAULT) {
                (void)controller->motor.apply(controller->motor.context, 0U, 0U);
                return INPUT_CONTROL_FAULT_LATCHED;
            }

            candidate.conveyorState = UART_INPUT_CONVEYOR_STOPPED;
            return input_control_apply_state(controller, &candidate);

        case UART_CMD_INPUT_CONVEYOR_SET_SPEED:
            if (candidate.conveyorState == UART_INPUT_CONVEYOR_FAULT) {
                return INPUT_CONTROL_FAULT_LATCHED;
            }

            candidate.speed = frame->payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX];

            if ((candidate.speed == 0U) && (candidate.conveyorState == UART_INPUT_CONVEYOR_RUNNING)) {
                candidate.conveyorState = UART_INPUT_CONVEYOR_STOPPED;
            }

            return input_control_apply_state(controller, &candidate);

        case UART_CMD_INPUT_CONVEYOR_GET_STATUS:
            return INPUT_CONTROL_OK;

        case UART_CMD_INPUT_CONTROL_RESET:
            return input_control_reset(controller);

        default:
            return INPUT_CONTROL_UNSUPPORTED_COMMAND;
    }
}

void input_control_build_status_payload(const input_control_state_t* state,
                                        uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE]) {
    if ((state == NULL) || (payload == NULL)) {
        return;
    }

    payload[UART_RESPONSE_STATUS_INDEX] =
        ((state->conveyorState == UART_INPUT_CONVEYOR_FAULT) || (state->lastError != UART_ERROR_NONE))
            ? UART_STATUS_ERROR
            : UART_STATUS_SUCCESS;
    payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_INPUT_CONVEYOR_GET_STATUS;
    payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)state->lastError;
    payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] = (uint8_t)state->conveyorState;
    payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] = state->speed;
}
