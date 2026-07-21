#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdint.h>

#include "motor_control_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t MotorControl_Init(void);
uint8_t MotorControl_Apply(const motor_output_t* output);
void MotorControl_ForceStop(void);
void MotorControl_GetLastOutput(motor_output_t* output);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONTROL_H */
