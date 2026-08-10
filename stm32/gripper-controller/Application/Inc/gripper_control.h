#ifndef GRIPPER_CONTROL_H
#define GRIPPER_CONTROL_H

#include <stdint.h>

#include "gripper_servo.h"
#include "logistics/contracts/uart/gripper_commands.h"

typedef enum {
    GRIPPER_CONTROL_OK = 0,
    GRIPPER_CONTROL_INVALID_ARGUMENT,
    GRIPPER_CONTROL_INVALID_PAYLOAD,
    GRIPPER_CONTROL_BUSY,
    GRIPPER_CONTROL_NOT_HOMED,
    GRIPPER_CONTROL_SAFETY_STOP,
    GRIPPER_CONTROL_SERVO_ERROR
} gripper_control_result_t;

typedef struct {
    uint8_t valid;
    uint8_t fault;
    uint16_t motion_id;
    uart_gripper_motion_type_t motion_type;
    uart_error_t error;
} gripper_control_completion_t;

typedef struct {
    uart_gripper_state_t state;
    uint16_t active_motion_id;
    uint16_t base_angle;
    uint16_t shoulder_angle;
    uint16_t elbow_angle;
    uint8_t gripper_position;
    uint8_t homed;
    uint8_t safety_latched;
} gripper_control_snapshot_t;

typedef struct {
    const gripper_servo_port_t* servo;
    uart_gripper_state_t state;
    uart_gripper_motion_type_t motion_type;
    uint16_t active_motion_id;
    uint16_t base_angle;
    uint16_t shoulder_angle;
    uint16_t elbow_angle;
    uint8_t gripper_position;
    uint16_t start_base_angle;
    uint16_t start_shoulder_angle;
    uint16_t start_elbow_angle;
    uint8_t start_gripper_position;
    uint16_t target_base_angle;
    uint16_t target_shoulder_angle;
    uint16_t target_elbow_angle;
    uint8_t target_gripper_position;
    uint32_t motion_start_tick;
    uint32_t motion_duration_ms;
    uint8_t outputs_enabled;
    uint8_t homed;
    uint8_t safety_latched;
    gripper_control_completion_t completion;
} gripper_control_t;

gripper_control_result_t gripper_control_init(gripper_control_t* controller, const gripper_servo_port_t* servo);

gripper_control_result_t gripper_control_process_command(gripper_control_t* controller, uint8_t command,
                                                         const uint8_t* payload, uint8_t length, uint32_t now_ms);

void gripper_control_tick(gripper_control_t* controller, uint32_t now_ms);

uint8_t gripper_control_take_completion(gripper_control_t* controller, gripper_control_completion_t* completion);

void gripper_control_apply_safety_stop(gripper_control_t* controller);

void gripper_control_release_safety(gripper_control_t* controller);

void gripper_control_get_snapshot(const gripper_control_t* controller, gripper_control_snapshot_t* snapshot);

#endif /* GRIPPER_CONTROL_H */
