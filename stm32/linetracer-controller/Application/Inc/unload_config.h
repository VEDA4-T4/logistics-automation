#ifndef UNLOAD_CONFIG_H
#define UNLOAD_CONFIG_H

#include <stdint.h>

#include "app_timing.h"

/*
 * TIM4 runs at 1 MHz with a 20 ms period. Servo 1 uses TIM4_CH1/PB6 and
 * servo 2 uses TIM4_CH2/PB7. The mirrored
 * defaults assume opposing linkage;
 * calibrate each pair independently on the assembled unload mechanism.
 */
#define UNLOAD_SERVO_REFERENCE_ANGLE_DEG 90U
#define UNLOAD_SERVO_RELEASE_ANGLE_DEG 70U
#define UNLOAD_SERVO_REFERENCE_PULSE_DELTA_US 1000U
#define UNLOAD_SERVO_RELEASE_PULSE_DELTA_US                                    \
    ((UNLOAD_SERVO_REFERENCE_PULSE_DELTA_US * UNLOAD_SERVO_RELEASE_ANGLE_DEG + \
      (UNLOAD_SERVO_REFERENCE_ANGLE_DEG / 2U)) /                               \
     UNLOAD_SERVO_REFERENCE_ANGLE_DEG)

/*
 * On the assembled unload linkage, raising the platform requires CH1 to move
 * toward a shorter pulse and CH2
 * toward a longer pulse. The servos are mounted
 * as a mirrored pair, so HOME and RELEASE must move in opposite
 * directions.
 */
#define UNLOAD_SERVO_1_HOME_PULSE_US 2000U
#define UNLOAD_SERVO_1_RELEASE_PULSE_US (UNLOAD_SERVO_1_HOME_PULSE_US - UNLOAD_SERVO_RELEASE_PULSE_DELTA_US)
#define UNLOAD_SERVO_2_HOME_PULSE_US 1000U
#define UNLOAD_SERVO_2_RELEASE_PULSE_US (UNLOAD_SERVO_2_HOME_PULSE_US + UNLOAD_SERVO_RELEASE_PULSE_DELTA_US)
#define UNLOAD_SERVO_RAMP_STEP_US 40U

/* Mechanical timings are intentionally conservative and must be calibrated on the vehicle. */
#define UNLOAD_SERVO_DEPLOY_MS 700U
#define UNLOAD_SERVO_HOME_MS 500U
#define UNLOAD_OPERATION_TIMEOUT_MS 5000U
#define UNLOAD_SENSOR_SNAPSHOT_MAX_AGE_MS 100U

#if UNLOAD_OPERATION_TIMEOUT_MS <= UNLOAD_SERVO_DEPLOY_MS
#error "Unload timeout must allow the servo to reach its release position"
#endif

#if UNLOAD_SENSOR_SNAPSHOT_MAX_AGE_MS < APP_TIMING_SENSOR_PERIOD_MS
#error "Unload sensor snapshot age must allow at least one SensorTask period"
#endif

#if UNLOAD_SERVO_1_HOME_PULSE_US >= 20000U || UNLOAD_SERVO_1_RELEASE_PULSE_US >= 20000U || \
    UNLOAD_SERVO_2_HOME_PULSE_US >= 20000U || UNLOAD_SERVO_2_RELEASE_PULSE_US >= 20000U
#error "Each unload servo pulse width must fit within the TIM4 20 ms period"
#endif

#if UNLOAD_SERVO_RAMP_STEP_US == 0U
#error "Unload servo ramp step must be greater than zero"
#endif

#endif /* UNLOAD_CONFIG_H */
