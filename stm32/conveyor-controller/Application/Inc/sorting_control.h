#ifndef SORTING_CONTROL_H
#define SORTING_CONTROL_H

#include <stdint.h>

#include "app_messages.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "sorting_gate.h"
#include "sorting_motor.h"

typedef enum {
    SORTING_CONTROL_OK = 0,
    SORTING_CONTROL_INVALID_ARGUMENT,
    SORTING_CONTROL_INVALID_SOURCE,
    SORTING_CONTROL_UNSUPPORTED_COMMAND,
    SORTING_CONTROL_INVALID_PAYLOAD,
    SORTING_CONTROL_SPEED_NOT_CONFIGURED,
    SORTING_CONTROL_FAULT_LATCHED,
    SORTING_CONTROL_MOTOR_ERROR,
    SORTING_CONTROL_GATE_ERROR,
    SORTING_CONTROL_BUSY,
    SORTING_CONTROL_NO_ACTIVE_CYCLE,
    SORTING_CONTROL_CYCLE_MISMATCH,
    SORTING_CONTROL_MOTION_PENDING,
    SORTING_CONTROL_STALE_COMMAND,
    SORTING_CONTROL_SEQUENCE_CONFLICT,
    SORTING_CONTROL_TX_BUSY
} sorting_control_result_t;

typedef struct {
    uart_sorting_conveyor_state_t conveyorState;
    uint8_t speed;
    uart_sorting_gate_state_t gateState;
    uint16_t activeCycleId;
    uart_sorting_destination_t activeDestination;
    uart_error_t lastError;
} sorting_control_state_t;

typedef struct {
    uint8_t valid;
    uint16_t cycleId;
    uart_sorting_destination_t destination;
} sorting_cycle_complete_t;

typedef struct {
    sorting_control_state_t state;
    sorting_motor_port_t motor;
    sorting_gate_port_t gate;
    uint8_t motorInitialized;
    uint8_t gateInitialized;
    uint8_t safetyStopLatched;
    uint8_t completeCycleOnHome;
} sorting_control_t;

sorting_control_result_t sorting_control_init(sorting_control_t* controller, const sorting_motor_port_t* motor,
                                              const sorting_gate_port_t* gate);

sorting_control_result_t sorting_control_process_command(sorting_control_t* controller,
                                                         const control_command_t* message);

sorting_control_result_t sorting_control_service_motion(sorting_control_t* controller,
                                                        sorting_cycle_complete_t* completion);

sorting_control_result_t sorting_control_handle_safety_stop(sorting_control_t* controller);

sorting_control_result_t sorting_control_handle_safety_release(sorting_control_t* controller);

void sorting_control_build_status_payload(const sorting_control_state_t* state,
                                          uint8_t payload[UART_SORTING_STATUS_PAYLOAD_SIZE]);

void sorting_control_build_conveyor_status_payload(const sorting_control_state_t* state,
                                                   uint8_t payload[UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE]);

#endif /* SORTING_CONTROL_H */
