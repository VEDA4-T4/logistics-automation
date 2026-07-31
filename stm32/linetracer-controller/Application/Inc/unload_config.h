#ifndef UNLOAD_CONFIG_H
#define UNLOAD_CONFIG_H

#include <stdint.h>

#include "app_timing.h"

/* TIM4 runs at 1 MHz with a 20 ms period; values are servo pulse widths. */
#define UNLOAD_SERVO_HOME_PULSE_US 1000U
#define UNLOAD_SERVO_RELEASE_PULSE_US 2000U

/* Mechanical timings are intentionally conservative and must be calibrated on the vehicle. */
#define UNLOAD_SERVO_DEPLOY_MS 700U
#define UNLOAD_SERVO_HOME_MS 500U
#define UNLOAD_LOAD_OFF_STABLE_MS APP_TIMING_FSR_STABLE_MS
#define UNLOAD_OPERATION_TIMEOUT_MS 5000U
#define UNLOAD_SENSOR_SNAPSHOT_MAX_AGE_MS 100U

#if UNLOAD_OPERATION_TIMEOUT_MS <= UNLOAD_SERVO_DEPLOY_MS
#error "Unload timeout must allow the servo to reach its release position"
#endif

#if UNLOAD_SENSOR_SNAPSHOT_MAX_AGE_MS < APP_TIMING_SENSOR_PERIOD_MS
#error "Unload sensor snapshot age must allow at least one SensorTask period"
#endif

#if UNLOAD_SERVO_HOME_PULSE_US >= 20000U || UNLOAD_SERVO_RELEASE_PULSE_US >= 20000U
#error "Unload servo pulse width must fit within the TIM4 20 ms period"
#endif

#endif /* UNLOAD_CONFIG_H */
