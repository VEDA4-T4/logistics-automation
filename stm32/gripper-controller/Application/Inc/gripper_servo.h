#ifndef GRIPPER_SERVO_H
#define GRIPPER_SERVO_H

#include <stdint.h>

typedef struct {
    void* context;
    int32_t (*enable)(void* context);
    int32_t (*write_arm)(void* context, uint16_t base_angle, uint16_t shoulder_angle, uint16_t elbow_angle);
    int32_t (*write_gripper)(void* context, uint8_t position_percent);
} gripper_servo_port_t;

const gripper_servo_port_t* gripper_servo_mg90s_port(void);

#endif /* GRIPPER_SERVO_H */
