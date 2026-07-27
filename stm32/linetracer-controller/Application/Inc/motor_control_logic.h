#ifndef MOTOR_CONTROL_LOGIC_H
#define MOTOR_CONTROL_LOGIC_H

#include <stdint.h>

#include "linetracer_control_types.h"
#include "route_planner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MOTOR_DIRECTION_COAST = 0, MOTOR_DIRECTION_FORWARD, MOTOR_DIRECTION_REVERSE } motor_direction_t;

typedef struct {
    uint16_t left_pwm;
    uint16_t right_pwm;
    motor_direction_t left_direction;
    motor_direction_t right_direction;
    uint8_t standby;
} motor_output_t;

uint16_t MotorControlLogic_ClampPwm(int32_t pwm);
void MotorControlLogic_MakeSafeStop(motor_output_t* output);
uint8_t MotorControlLogic_ComputeLineFollow(linetracer_line_state_t line_state, motor_output_t* output);
uint8_t MotorControlLogic_ComputeRouteAction(route_action_t action, motor_output_t* output);
uint8_t MotorControlLogic_ComputeControlOutput(linetracer_control_state_t state, route_action_t pending_action,
                                               linetracer_line_state_t line_state, uint8_t route_active,
                                               uint8_t safety_latched, motor_output_t* output);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONTROL_LOGIC_H */
