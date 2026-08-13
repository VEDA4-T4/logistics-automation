#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include "app_timing.h"

/*
 * TCRT5000 digital modules commonly drive LOW on black. Set this to 0U if
 * hardware calibration shows that black produces HIGH.
 */
#define SENSOR_LINE_ACTIVE_LOW 0U

/* A physical marker is a short CENTERED -> WHITE_GAP -> CENTERED sequence. */
#define SENSOR_MARKER_MIN_GAP_MS 20U
#define SENSOR_MARKER_MAX_GAP_MS 300U
#define SENSOR_MARKER_REARM_MS 50U
#define SENSOR_LINE_LOST_TIMEOUT_MS 500U

/* FSR ADC1_IN0 is 12-bit (0..4095). Calibrate these with the actual load. */
#define SENSOR_FSR_FILTER_SAMPLES 8U
#define SENSOR_FSR_LOAD_ON_THRESHOLD 2200U
#define SENSOR_FSR_LOAD_OFF_THRESHOLD 1700U
#define SENSOR_FSR_OVERLOAD_THRESHOLD 3800U
#define SENSOR_FSR_OVERLOAD_CLEAR_THRESHOLD 3400U
#define SENSOR_FSR_ADC_TIMEOUT_MS 50U

/* HC-SR04 operating range and obstacle hysteresis. */
#define SENSOR_ULTRASONIC_MIN_MM 20U
#define SENSOR_ULTRASONIC_MAX_MM 4000U
#define SENSOR_OBSTACLE_ON_MM 150U
#define SENSOR_OBSTACLE_OFF_MM 220U
#define SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES 3U
#define SENSOR_ULTRASONIC_ECHO_TIMEOUT_MS 30U
#define SENSOR_ULTRASONIC_STALE_MS 750U

/* TIM1 is configured at runtime to count one tick per microsecond. */
#define SENSOR_ULTRASONIC_TIMER_HZ 1000000U
#define SENSOR_ULTRASONIC_TRIGGER_PULSE_US 10U

#if (SENSOR_FSR_FILTER_SAMPLES == 0U)
#error "FSR filter must contain at least one sample"
#endif

#if (SENSOR_MARKER_MIN_GAP_MS >= SENSOR_MARKER_MAX_GAP_MS)
#error "Marker minimum time must be shorter than marker maximum time"
#endif

#if (SENSOR_MARKER_MAX_GAP_MS >= SENSOR_LINE_LOST_TIMEOUT_MS)
#error "Marker maximum time must be shorter than line-lost time"
#endif

#if (SENSOR_MARKER_REARM_MS < APP_TIMING_SENSOR_PERIOD_MS)
#error "Marker rearm time must cover at least one SensorTask period"
#endif

#if (SENSOR_FSR_LOAD_OFF_THRESHOLD >= SENSOR_FSR_LOAD_ON_THRESHOLD)
#error "FSR LOAD_OFF threshold must be lower than LOAD_ON threshold"
#endif

#if (SENSOR_FSR_OVERLOAD_CLEAR_THRESHOLD >= SENSOR_FSR_OVERLOAD_THRESHOLD)
#error "FSR overload-clear threshold must be lower than overload threshold"
#endif

#if (SENSOR_OBSTACLE_ON_MM >= SENSOR_OBSTACLE_OFF_MM)
#error "Obstacle-on distance must be shorter than obstacle-off distance"
#endif

#if (SENSOR_ULTRASONIC_MIN_MM >= SENSOR_ULTRASONIC_MAX_MM)
#error "Ultrasonic minimum distance must be shorter than maximum distance"
#endif

#if (SENSOR_ULTRASONIC_STALE_MS <= (APP_TIMING_ULTRASONIC_PERIOD_MS * 3U))
#error "Ultrasonic stale time must exceed one complete three-sensor cycle"
#endif

#endif /* SENSOR_CONFIG_H */
