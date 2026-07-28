#ifndef CONVEYOR_MOTOR_POWER_H
#define CONVEYOR_MOTOR_POWER_H

#include <stdint.h>

/* PB6/STBY is shared by both TB6612FNG motor channels. */
uint8_t conveyor_motor_power_enable(void);
void conveyor_motor_power_latch_disable(void);
void conveyor_motor_power_release_latch(void);

#endif /* CONVEYOR_MOTOR_POWER_H */
