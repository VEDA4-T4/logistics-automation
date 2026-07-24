#include "sorting_control.h"

#include <string.h>

static uint8_t sorting_control_is_status_command(uint8_t command) {
    return ((command == UART_CMD_SORTING_GET_STATUS) || (command == UART_CMD_SORTING_CONVEYOR_GET_STATUS)) ? 1U : 0U;
}

static void sorting_control_clear_cycle(sorting_control_t* controller) {
    controller->state.activeCycleId = 0U;
    controller->state.activeDestination = UART_SORTING_DESTINATION_NONE;
    controller->completeCycleOnHome = 0U;
}

static void sorting_control_set_motor_fault(sorting_control_t* controller) {
    controller->state.conveyorState = UART_SORTING_CONVEYOR_FAULT;
    controller->state.lastError = UART_ERROR_MOTOR;
    controller->motorInitialized = 0U;
}

static void sorting_control_set_gate_fault(sorting_control_t* controller) {
    controller->state.gateState = UART_SORTING_GATE_FAULT;
    controller->state.lastError = UART_ERROR_SERVO;
    controller->gateInitialized = 0U;
}

static sorting_control_result_t sorting_control_apply_motor(sorting_control_t* controller,
                                                            const sorting_control_state_t* candidate) {
    uint8_t running;

    running = (candidate->conveyorState == UART_SORTING_CONVEYOR_RUNNING) ? 1U : 0U;

    if ((controller->motorInitialized == 0U) ||
        (controller->motor.apply(controller->motor.context, running, candidate->speed) != SORTING_MOTOR_OK)) {
        (void)controller->motor.apply(controller->motor.context, 0U, 0U);
        sorting_control_set_motor_fault(controller);
        return SORTING_CONTROL_MOTOR_ERROR;
    }

    controller->state.conveyorState = candidate->conveyorState;
    controller->state.speed = candidate->speed;
    return SORTING_CONTROL_OK;
}

static sorting_control_result_t sorting_control_move_gate(sorting_control_t* controller,
                                                          uart_sorting_destination_t destination,
                                                          uart_sorting_gate_state_t movingState) {
    if ((controller->gateInitialized == 0U) ||
        (controller->gate.move(controller->gate.context, destination) != SORTING_GATE_OK)) {
        sorting_control_set_gate_fault(controller);
        return SORTING_CONTROL_GATE_ERROR;
    }

    controller->state.gateState = movingState;
    return SORTING_CONTROL_OK;
}

static sorting_control_result_t sorting_control_reset(sorting_control_t* controller) {
    sorting_control_state_t candidate;

    if (controller->safetyStopLatched != 0U) {
        return SORTING_CONTROL_FAULT_LATCHED;
    }

    if (controller->motorInitialized == 0U) {
        if (controller->motor.initialize(controller->motor.context) != SORTING_MOTOR_OK) {
            sorting_control_set_motor_fault(controller);
            return SORTING_CONTROL_MOTOR_ERROR;
        }

        controller->motorInitialized = 1U;
    }

    if (controller->gateInitialized == 0U) {
        if (controller->gate.initialize(controller->gate.context) != SORTING_GATE_OK) {
            sorting_control_set_gate_fault(controller);
            return SORTING_CONTROL_GATE_ERROR;
        }

        controller->gateInitialized = 1U;
        controller->state.gateState = UART_SORTING_GATE_RETURNING;
    } else if (sorting_control_move_gate(controller, UART_SORTING_DESTINATION_NONE, UART_SORTING_GATE_RETURNING) !=
               SORTING_CONTROL_OK) {
        return SORTING_CONTROL_GATE_ERROR;
    }

    candidate = controller->state;
    candidate.conveyorState = UART_SORTING_CONVEYOR_STOPPED;

    if (sorting_control_apply_motor(controller, &candidate) != SORTING_CONTROL_OK) {
        return SORTING_CONTROL_MOTOR_ERROR;
    }

    sorting_control_clear_cycle(controller);
    controller->state.lastError = UART_ERROR_NONE;
    return SORTING_CONTROL_OK;
}

sorting_control_result_t sorting_control_init(sorting_control_t* controller, const sorting_motor_port_t* motor,
                                              const sorting_gate_port_t* gate) {
    sorting_control_result_t result;

    if ((controller == NULL) || (motor == NULL) || (gate == NULL) || (motor->initialize == NULL) ||
        (motor->apply == NULL) || (gate->initialize == NULL) || (gate->move == NULL) ||
        (gate->motion_complete == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    memset(controller, 0, sizeof(*controller));
    controller->motor = *motor;
    controller->gate = *gate;
    controller->state.conveyorState = UART_SORTING_CONVEYOR_STOPPED;
    controller->state.gateState = UART_SORTING_GATE_RETURNING;
    controller->state.activeDestination = UART_SORTING_DESTINATION_NONE;
    controller->state.lastError = UART_ERROR_NONE;
    result = SORTING_CONTROL_OK;

    if (controller->motor.initialize(controller->motor.context) == SORTING_MOTOR_OK) {
        controller->motorInitialized = 1U;

        if (controller->motor.apply(controller->motor.context, 0U, 0U) != SORTING_MOTOR_OK) {
            sorting_control_set_motor_fault(controller);
            result = SORTING_CONTROL_MOTOR_ERROR;
        }
    } else {
        sorting_control_set_motor_fault(controller);
        result = SORTING_CONTROL_MOTOR_ERROR;
    }

    if (controller->gate.initialize(controller->gate.context) == SORTING_GATE_OK) {
        controller->gateInitialized = 1U;
        controller->state.gateState = UART_SORTING_GATE_RETURNING;
    } else {
        sorting_control_set_gate_fault(controller);
        result = SORTING_CONTROL_GATE_ERROR;
    }

    return result;
}

sorting_control_result_t sorting_control_service_motion(sorting_control_t* controller,
                                                        sorting_cycle_complete_t* completion) {
    uint8_t complete;

    if ((controller == NULL) || (completion == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    memset(completion, 0, sizeof(*completion));

    if ((controller->state.gateState != UART_SORTING_GATE_MOVING) &&
        (controller->state.gateState != UART_SORTING_GATE_RETURNING)) {
        return SORTING_CONTROL_OK;
    }

    if ((controller->gateInitialized == 0U) ||
        (controller->gate.motion_complete(controller->gate.context, &complete) != SORTING_GATE_OK)) {
        sorting_control_set_gate_fault(controller);
        return SORTING_CONTROL_GATE_ERROR;
    }

    if (complete == 0U) {
        return SORTING_CONTROL_MOTION_PENDING;
    }

    if (controller->state.gateState == UART_SORTING_GATE_MOVING) {
        controller->state.gateState = UART_SORTING_GATE_WAIT_ITEM;
        return SORTING_CONTROL_OK;
    }

    if ((controller->completeCycleOnHome != 0U) && (controller->state.activeCycleId != 0U) &&
        (uart_sorting_destination_is_valid(controller->state.activeDestination) != 0U)) {
        completion->valid = 1U;
        completion->cycleId = controller->state.activeCycleId;
        completion->destination = controller->state.activeDestination;
    }

    controller->state.gateState = UART_SORTING_GATE_HOME;
    sorting_control_clear_cycle(controller);
    return SORTING_CONTROL_OK;
}

sorting_control_result_t sorting_control_process_command(sorting_control_t* controller,
                                                         const control_command_t* message) {
    const uart_frame_t* frame;
    sorting_control_state_t candidate;
    uint16_t cycleId;

    if ((controller == NULL) || (message == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    if (message->source != APP_UART_CHANNEL_6) {
        return SORTING_CONTROL_INVALID_SOURCE;
    }

    frame = &message->frame;

    if (UART_IS_VALID_SORTING_COMMAND(frame->command) == 0U) {
        return SORTING_CONTROL_UNSUPPORTED_COMMAND;
    }

    if (UART_IS_VALID_SORTING_PAYLOAD(frame->command, frame->payload, frame->length) == 0U) {
        return SORTING_CONTROL_INVALID_PAYLOAD;
    }

    if ((controller->safetyStopLatched != 0U) && (sorting_control_is_status_command(frame->command) == 0U)) {
        return SORTING_CONTROL_FAULT_LATCHED;
    }

    candidate = controller->state;

    switch (frame->command) {
        case UART_CMD_SORTING_CONVEYOR_START:
            if ((candidate.conveyorState == UART_SORTING_CONVEYOR_FAULT) ||
                (candidate.gateState == UART_SORTING_GATE_FAULT) || (candidate.lastError != UART_ERROR_NONE)) {
                return SORTING_CONTROL_FAULT_LATCHED;
            }

            if (candidate.speed == 0U) {
                return SORTING_CONTROL_SPEED_NOT_CONFIGURED;
            }

            candidate.conveyorState = UART_SORTING_CONVEYOR_RUNNING;
            return sorting_control_apply_motor(controller, &candidate);

        case UART_CMD_SORTING_CONVEYOR_STOP:
            candidate.conveyorState = UART_SORTING_CONVEYOR_STOPPED;

            if (sorting_control_apply_motor(controller, &candidate) != SORTING_CONTROL_OK) {
                return SORTING_CONTROL_MOTOR_ERROR;
            }

            return (candidate.lastError == UART_ERROR_NONE) ? SORTING_CONTROL_OK : SORTING_CONTROL_FAULT_LATCHED;

        case UART_CMD_SORTING_CONVEYOR_SET_SPEED:
            if ((candidate.conveyorState == UART_SORTING_CONVEYOR_FAULT) ||
                (candidate.gateState == UART_SORTING_GATE_FAULT) || (candidate.lastError != UART_ERROR_NONE)) {
                return SORTING_CONTROL_FAULT_LATCHED;
            }

            candidate.speed = frame->payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX];

            if ((candidate.speed == 0U) && (candidate.conveyorState == UART_SORTING_CONVEYOR_RUNNING)) {
                candidate.conveyorState = UART_SORTING_CONVEYOR_STOPPED;
            }

            return sorting_control_apply_motor(controller, &candidate);

        case UART_CMD_SORTING_CONVEYOR_GET_STATUS:
        case UART_CMD_SORTING_GET_STATUS:
            return SORTING_CONTROL_OK;

        case UART_CMD_SORTING_ROUTE_ITEM:
            if ((candidate.gateState == UART_SORTING_GATE_FAULT) || (candidate.lastError != UART_ERROR_NONE)) {
                return SORTING_CONTROL_FAULT_LATCHED;
            }

            if ((candidate.activeCycleId != 0U) || (candidate.gateState != UART_SORTING_GATE_HOME)) {
                return SORTING_CONTROL_BUSY;
            }

            cycleId = uart_sorting_route_cycle_id(frame->payload);
            candidate.activeDestination =
                (uart_sorting_destination_t)frame->payload[UART_SORTING_ROUTE_DESTINATION_INDEX];

            if (sorting_control_move_gate(controller, candidate.activeDestination, UART_SORTING_GATE_MOVING) !=
                SORTING_CONTROL_OK) {
                return SORTING_CONTROL_GATE_ERROR;
            }

            controller->state.activeCycleId = cycleId;
            controller->state.activeDestination = candidate.activeDestination;
            controller->completeCycleOnHome = 0U;
            return SORTING_CONTROL_OK;

        case UART_CMD_SORTING_RETURN_HOME:
            cycleId = uart_sorting_return_home_cycle_id(frame->payload);

            if (candidate.activeCycleId == 0U) {
                return SORTING_CONTROL_NO_ACTIVE_CYCLE;
            }

            if (cycleId != candidate.activeCycleId) {
                return SORTING_CONTROL_CYCLE_MISMATCH;
            }

            if (candidate.gateState != UART_SORTING_GATE_WAIT_ITEM) {
                return SORTING_CONTROL_BUSY;
            }

            if (sorting_control_move_gate(controller, UART_SORTING_DESTINATION_NONE, UART_SORTING_GATE_RETURNING) !=
                SORTING_CONTROL_OK) {
                return SORTING_CONTROL_GATE_ERROR;
            }

            controller->completeCycleOnHome = 1U;
            return SORTING_CONTROL_OK;

        case UART_CMD_SORTING_CANCEL:
            cycleId = uart_sorting_cancel_cycle_id(frame->payload);

            if (candidate.activeCycleId == 0U) {
                return SORTING_CONTROL_NO_ACTIVE_CYCLE;
            }

            if (cycleId != candidate.activeCycleId) {
                return SORTING_CONTROL_CYCLE_MISMATCH;
            }

            if (sorting_control_move_gate(controller, UART_SORTING_DESTINATION_NONE, UART_SORTING_GATE_RETURNING) !=
                SORTING_CONTROL_OK) {
                return SORTING_CONTROL_GATE_ERROR;
            }

            controller->completeCycleOnHome = 0U;
            return SORTING_CONTROL_OK;

        case UART_CMD_SORTING_RESET:
            return sorting_control_reset(controller);

        default:
            return SORTING_CONTROL_UNSUPPORTED_COMMAND;
    }
}

sorting_control_result_t sorting_control_handle_safety_stop(sorting_control_t* controller) {
    sorting_cycle_complete_t ignoredCompletion;
    sorting_control_result_t motionResult;
    sorting_control_result_t result;

    if ((controller == NULL) || (controller->motor.initialize == NULL) || (controller->motor.apply == NULL) ||
        (controller->gate.initialize == NULL) || (controller->gate.move == NULL) ||
        (controller->gate.motion_complete == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    result = SORTING_CONTROL_OK;

    if (controller->motorInitialized == 0U) {
        if (controller->motor.initialize(controller->motor.context) != SORTING_MOTOR_OK) {
            sorting_control_set_motor_fault(controller);
            result = SORTING_CONTROL_MOTOR_ERROR;
        } else {
            controller->motorInitialized = 1U;
        }
    }

    if ((controller->motorInitialized != 0U) &&
        (controller->motor.apply(controller->motor.context, 0U, 0U) != SORTING_MOTOR_OK)) {
        sorting_control_set_motor_fault(controller);
        result = SORTING_CONTROL_MOTOR_ERROR;
    }

    if (controller->safetyStopLatched == 0U) {
        controller->safetyStopLatched = 1U;
        controller->completeCycleOnHome = 0U;
        sorting_control_clear_cycle(controller);
    }

    if (controller->gateInitialized == 0U) {
        if (controller->gate.initialize(controller->gate.context) != SORTING_GATE_OK) {
            sorting_control_set_gate_fault(controller);
            result = SORTING_CONTROL_GATE_ERROR;
        } else {
            controller->gateInitialized = 1U;
            controller->state.gateState = UART_SORTING_GATE_RETURNING;
        }
    } else if ((controller->state.gateState != UART_SORTING_GATE_HOME) &&
               (controller->state.gateState != UART_SORTING_GATE_RETURNING) &&
               (sorting_control_move_gate(controller, UART_SORTING_DESTINATION_NONE, UART_SORTING_GATE_RETURNING) !=
                SORTING_CONTROL_OK)) {
        result = SORTING_CONTROL_GATE_ERROR;
    }

    controller->state.conveyorState = UART_SORTING_CONVEYOR_FAULT;
    controller->state.lastError = UART_ERROR_EMERGENCY_STOP;

    if (controller->state.gateState == UART_SORTING_GATE_RETURNING) {
        motionResult = sorting_control_service_motion(controller, &ignoredCompletion);

        if (motionResult == SORTING_CONTROL_MOTION_PENDING) {
            return SORTING_CONTROL_MOTION_PENDING;
        }

        if (motionResult != SORTING_CONTROL_OK) {
            return motionResult;
        }
    }

    return result;
}

sorting_control_result_t sorting_control_handle_safety_release(sorting_control_t* controller) {
    if ((controller == NULL) || (controller->motor.initialize == NULL) || (controller->motor.apply == NULL) ||
        (controller->gate.initialize == NULL) || (controller->gate.move == NULL)) {
        return SORTING_CONTROL_INVALID_ARGUMENT;
    }

    if (controller->safetyStopLatched == 0U) {
        return SORTING_CONTROL_OK;
    }

    if (controller->state.gateState != UART_SORTING_GATE_HOME) {
        return SORTING_CONTROL_MOTION_PENDING;
    }

    if (controller->motorInitialized == 0U) {
        if (controller->motor.initialize(controller->motor.context) != SORTING_MOTOR_OK) {
            sorting_control_set_motor_fault(controller);
            return SORTING_CONTROL_MOTOR_ERROR;
        }

        controller->motorInitialized = 1U;
    }

    if (controller->motor.apply(controller->motor.context, 0U, 0U) != SORTING_MOTOR_OK) {
        sorting_control_set_motor_fault(controller);
        return SORTING_CONTROL_MOTOR_ERROR;
    }

    controller->state.conveyorState = UART_SORTING_CONVEYOR_STOPPED;
    controller->state.lastError = UART_ERROR_NONE;
    controller->safetyStopLatched = 0U;
    sorting_control_clear_cycle(controller);
    return SORTING_CONTROL_OK;
}

void sorting_control_build_status_payload(const sorting_control_state_t* state,
                                          uint8_t payload[UART_SORTING_STATUS_PAYLOAD_SIZE]) {
    if ((state == NULL) || (payload == NULL)) {
        return;
    }

    payload[UART_RESPONSE_STATUS_INDEX] =
        ((state->gateState == UART_SORTING_GATE_FAULT) || (state->lastError != UART_ERROR_NONE)) ? UART_STATUS_ERROR
                                                                                                 : UART_STATUS_SUCCESS;
    payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_SORTING_GET_STATUS;
    payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)state->lastError;
    payload[UART_SORTING_STATUS_GATE_STATE_INDEX] = (uint8_t)state->gateState;
    payload[UART_SORTING_STATUS_CYCLE_ID_LOW_INDEX] = (uint8_t)(state->activeCycleId & 0xFFU);
    payload[UART_SORTING_STATUS_CYCLE_ID_HIGH_INDEX] = (uint8_t)((state->activeCycleId >> 8U) & 0xFFU);
    payload[UART_SORTING_STATUS_DESTINATION_INDEX] = (uint8_t)state->activeDestination;
}

void sorting_control_build_conveyor_status_payload(const sorting_control_state_t* state,
                                                   uint8_t payload[UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE]) {
    if ((state == NULL) || (payload == NULL)) {
        return;
    }

    payload[UART_RESPONSE_STATUS_INDEX] =
        ((state->conveyorState == UART_SORTING_CONVEYOR_FAULT) || (state->lastError != UART_ERROR_NONE))
            ? UART_STATUS_ERROR
            : UART_STATUS_SUCCESS;
    payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_SORTING_CONVEYOR_GET_STATUS;
    payload[UART_RESPONSE_ERROR_INDEX] = (uint8_t)state->lastError;
    payload[UART_SORTING_CONVEYOR_STATUS_STATE_INDEX] = (uint8_t)state->conveyorState;
    payload[UART_SORTING_CONVEYOR_STATUS_SPEED_INDEX] = state->speed;
}
