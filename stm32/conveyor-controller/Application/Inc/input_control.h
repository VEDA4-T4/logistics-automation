#ifndef INPUT_CONTROL_H
#define INPUT_CONTROL_H

#include <stdint.h>

#include "app_messages.h"
#include "input_motor.h"
#include "logistics/contracts/uart/input_commands.h"

typedef enum {
    INPUT_CONTROL_OK = 0,
    INPUT_CONTROL_INVALID_ARGUMENT,
    INPUT_CONTROL_INVALID_SOURCE,
    INPUT_CONTROL_UNSUPPORTED_COMMAND,
    INPUT_CONTROL_INVALID_PAYLOAD,
    INPUT_CONTROL_SPEED_NOT_CONFIGURED,
    INPUT_CONTROL_FAULT_LATCHED,
    INPUT_CONTROL_MOTOR_ERROR
} input_control_result_t;

typedef struct {
    uart_input_conveyor_state_t conveyorState;
    uint8_t speed;
    uart_error_t lastError;
} input_control_state_t;

typedef struct {
    input_control_state_t state;
    input_motor_port_t motor;
    uint8_t motorInitialized;
} input_control_t;

input_control_result_t input_control_init(input_control_t* controller, const input_motor_port_t* motor);

input_control_result_t input_control_process_command(input_control_t* controller, const control_command_t* message);

void input_control_build_status_payload(const input_control_state_t* state,
                                        uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE]);

#endif /* INPUT_CONTROL_H */
