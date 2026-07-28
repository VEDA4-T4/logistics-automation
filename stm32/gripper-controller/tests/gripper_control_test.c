#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "gripper_calibration.h"
#include "gripper_control.h"

typedef struct {
    uint32_t enable_calls;
    uint32_t arm_writes;
    uint32_t gripper_writes;
    uint16_t base_angle;
    uint16_t shoulder_angle;
    uint16_t elbow_angle;
    uint8_t gripper_position;
    int32_t write_result;
} fake_servo_t;

static int32_t fake_enable(void* context) {
    fake_servo_t* servo = (fake_servo_t*)context;

    servo->enable_calls++;
    return 0;
}

static int32_t fake_write_arm(void* context, uint16_t base_angle, uint16_t shoulder_angle,
                              uint16_t elbow_angle) {
    fake_servo_t* servo = (fake_servo_t*)context;

    servo->arm_writes++;
    servo->base_angle = base_angle;
    servo->shoulder_angle = shoulder_angle;
    servo->elbow_angle = elbow_angle;
    return servo->write_result;
}

static int32_t fake_write_gripper(void* context, uint8_t position_percent) {
    fake_servo_t* servo = (fake_servo_t*)context;

    servo->gripper_writes++;
    servo->gripper_position = position_percent;
    return servo->write_result;
}

static void write_u16(uint8_t* payload, uint32_t low_index, uint16_t value) {
    payload[low_index] = (uint8_t)(value & 0xFFU);
    payload[low_index + 1U] = (uint8_t)(value >> 8U);
}

static gripper_servo_port_t make_port(fake_servo_t* servo) {
    gripper_servo_port_t port = {
        .context = servo,
        .enable = fake_enable,
        .write_arm = fake_write_arm,
        .write_gripper = fake_write_gripper,
    };

    return port;
}

static void home_controller(gripper_control_t* controller, uint32_t start_tick) {
    gripper_control_completion_t completion;
    uint8_t payload[UART_GRIPPER_HOME_PAYLOAD_SIZE] = {0};

    write_u16(payload, UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, 1U);
    assert(gripper_control_process_command(controller, UART_CMD_GRIPPER_HOME, payload, sizeof(payload), start_tick) ==
           GRIPPER_CONTROL_OK);
    gripper_control_tick(controller, start_tick + GRIPPER_HOME_DURATION_MS);
    assert(controller->homed == 1U);
    assert(gripper_control_take_completion(controller, &completion) == 1U);
}

static void test_arm_motion_is_interpolated_and_completed(void) {
    fake_servo_t servo = {0};
    gripper_servo_port_t port = make_port(&servo);
    gripper_control_t controller;
    gripper_control_completion_t completion;
    uint8_t payload[UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE] = {0};

    assert(gripper_control_init(&controller, &port) == GRIPPER_CONTROL_OK);
    assert(controller.homed == 0U);
    write_u16(payload, UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, 17U);
    write_u16(payload, UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, 1200U);
    write_u16(payload, UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 1000U);
    write_u16(payload, UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 800U);
    write_u16(payload, UART_GRIPPER_MOVE_DURATION_LOW_INDEX, 1000U);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, payload, sizeof(payload), 0U) ==
           GRIPPER_CONTROL_NOT_HOMED);
    home_controller(&controller, 0U);

    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, payload, sizeof(payload), 2100U) ==
           GRIPPER_CONTROL_OK);
    assert(servo.enable_calls == 1U);
    gripper_control_tick(&controller, 2600U);
    assert(servo.base_angle == 1050U);
    assert(servo.shoulder_angle == 950U);
    assert(servo.elbow_angle == 850U);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_HOME, payload,
                                           UART_GRIPPER_HOME_PAYLOAD_SIZE, 2600U) == GRIPPER_CONTROL_BUSY);

    gripper_control_tick(&controller, 3100U);
    assert(servo.base_angle == 1200U);
    assert(controller.state == UART_GRIPPER_STATE_IDLE);
    assert(gripper_control_take_completion(&controller, &completion) == 1U);
    assert(completion.motion_id == 17U);
    assert(completion.motion_type == UART_GRIPPER_MOTION_ARM);
    assert(completion.fault == 0U);
}

static void test_mechanical_limits_reject_unsafe_target(void) {
    fake_servo_t servo = {0};
    gripper_servo_port_t port = make_port(&servo);
    gripper_control_t controller;
    uint8_t payload[UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE] = {0};

    assert(gripper_control_init(&controller, &port) == GRIPPER_CONTROL_OK);
    home_controller(&controller, 0U);
    write_u16(payload, UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, 1U);
    write_u16(payload, UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, GRIPPER_BASE_MIN_ANGLE_DECI_DEG - 1U);
    write_u16(payload, UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 900U);
    write_u16(payload, UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 900U);
    write_u16(payload, UART_GRIPPER_MOVE_DURATION_LOW_INDEX, 1000U);

    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, payload, sizeof(payload), 0U) ==
           GRIPPER_CONTROL_INVALID_PAYLOAD);
    assert(servo.enable_calls == 1U);
}

static void test_short_requested_duration_is_extended_to_safe_joint_speed(void) {
    fake_servo_t servo = {0};
    gripper_servo_port_t port = make_port(&servo);
    gripper_control_t controller;
    uint8_t payload[UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE] = {0};

    assert(gripper_control_init(&controller, &port) == GRIPPER_CONTROL_OK);
    home_controller(&controller, 0U);
    write_u16(payload, UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, 18U);
    write_u16(payload, UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, 1200U);
    write_u16(payload, UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 1050U);
    write_u16(payload, UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 900U);
    write_u16(payload, UART_GRIPPER_MOVE_DURATION_LOW_INDEX, UART_GRIPPER_DURATION_MS_MIN);

    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, payload, sizeof(payload), 2100U) ==
           GRIPPER_CONTROL_OK);
    assert(controller.motion_duration_ms == 1000U);

    gripper_control_tick(&controller, 2600U);
    assert(servo.base_angle == 1050U);
    assert(servo.shoulder_angle == 975U);
    assert(controller.state == UART_GRIPPER_STATE_MOVING_ARM);

    gripper_control_tick(&controller, 3100U);
    assert(servo.base_angle == 1200U);
    assert(servo.shoulder_angle == 1050U);
    assert(controller.state == UART_GRIPPER_STATE_IDLE);
}

static void test_safety_stop_requires_release_and_explicit_home(void) {
    fake_servo_t servo = {0};
    gripper_servo_port_t port = make_port(&servo);
    gripper_control_t controller;
    uint8_t move[UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE] = {0};
    uint8_t home[UART_GRIPPER_HOME_PAYLOAD_SIZE] = {0};
    uint32_t writes_before_stop;

    assert(gripper_control_init(&controller, &port) == GRIPPER_CONTROL_OK);
    home_controller(&controller, 0U);
    write_u16(move, UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, 2U);
    write_u16(move, UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, 1000U);
    write_u16(move, UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 1000U);
    write_u16(move, UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 1000U);
    write_u16(move, UART_GRIPPER_MOVE_DURATION_LOW_INDEX, 1000U);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, move, sizeof(move), 2100U) ==
           GRIPPER_CONTROL_OK);

    writes_before_stop = servo.arm_writes;
    gripper_control_apply_safety_stop(&controller);
    assert(controller.state == UART_GRIPPER_STATE_EMERGENCY_STOP);
    gripper_control_tick(&controller, 2600U);
    assert(servo.arm_writes == writes_before_stop);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_MOVE_ARM, move, sizeof(move), 2600U) ==
           GRIPPER_CONTROL_SAFETY_STOP);

    gripper_control_release_safety(&controller);
    assert(controller.state == UART_GRIPPER_STATE_STOPPED);
    assert(controller.active_motion_id == UART_GRIPPER_MOTION_ID_NONE);

    write_u16(home, UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, 3U);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_HOME, home, sizeof(home), 2700U) ==
           GRIPPER_CONTROL_OK);
    assert(controller.state == UART_GRIPPER_STATE_HOMING);
}

static void test_gripper_motion_and_servo_fault(void) {
    fake_servo_t servo = {0};
    gripper_servo_port_t port = make_port(&servo);
    gripper_control_t controller;
    gripper_control_completion_t completion;
    uint8_t payload[UART_GRIPPER_SET_GRIPPER_PAYLOAD_SIZE] = {0};

    assert(gripper_control_init(&controller, &port) == GRIPPER_CONTROL_OK);
    home_controller(&controller, 0U);
    write_u16(payload, UART_GRIPPER_SET_MOTION_ID_LOW_INDEX, 9U);
    payload[UART_GRIPPER_SET_POSITION_INDEX] = 20U;
    write_u16(payload, UART_GRIPPER_SET_DURATION_LOW_INDEX, 100U);
    assert(gripper_control_process_command(&controller, UART_CMD_GRIPPER_SET_GRIPPER, payload, sizeof(payload),
                                           2100U) ==
           GRIPPER_CONTROL_OK);
    assert(controller.motion_duration_ms == 2000U);

    servo.write_result = -1;
    gripper_control_tick(&controller, 2120U);
    assert(controller.state == UART_GRIPPER_STATE_FAULT);
    assert(gripper_control_take_completion(&controller, &completion) == 1U);
    assert(completion.fault == 1U);
    assert(completion.error == UART_ERROR_SERVO);
}

int main(void) {
    test_arm_motion_is_interpolated_and_completed();
    test_mechanical_limits_reject_unsafe_target();
    test_short_requested_duration_is_extended_to_safe_joint_speed();
    test_safety_stop_requires_release_and_explicit_home();
    test_gripper_motion_and_servo_fault();
    puts("gripper_control_test: PASS");
    return 0;
}
