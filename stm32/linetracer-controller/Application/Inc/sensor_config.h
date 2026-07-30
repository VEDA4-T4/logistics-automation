#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include "app_timing.h"

/*
 * TCRT5000 digital modules commonly drive LOW on black. Set this to 0U if
 * hardware calibration shows that black produces HIGH.
 */
#define SENSOR_LINE_ACTIVE_LOW 0U

/*
 * TCRT5000 analog calibration measured on the current white track and black
 * line. Values outside the calibrated interval are saturated.
 */
#define SENSOR_LINE_LEFT_WHITE_RAW 200U
#define SENSOR_LINE_LEFT_BLACK_RAW 2000U
#define SENSOR_LINE_RIGHT_WHITE_RAW 200U
#define SENSOR_LINE_RIGHT_BLACK_RAW 2000U
#define SENSOR_LINE_NORMALIZED_MAX 1000U
#define SENSOR_LINE_FILTER_PREVIOUS_WEIGHT 3U
#define SENSOR_LINE_FILTER_DIVISOR 4U

/*
 * A physical marker is one to four short white stripes separated by centered
 * black line. The group is complete
 * when no next stripe arrives before the
 * group timeout.
 */
#define SENSOR_MARKER_MIN_GAP_MS 20U
#define SENSOR_MARKER_MAX_GAP_MS 300U
#define SENSOR_MARKER_REARM_MS 50U
#define SENSOR_MARKER_GROUP_TIMEOUT_MS 250U
#define SENSOR_MARKER_MAX_STRIPES 4U
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
#define SENSOR_ULTRASONIC_RECOVERY_SUCCESSES 3U
#define SENSOR_ULTRASONIC_ECHO_TIMEOUT_MS 30U
#define SENSOR_ULTRASONIC_STALE_MS 500U

/* TIM1 is configured at runtime to count one tick per microsecond. */
#define SENSOR_ULTRASONIC_TIMER_HZ 1000000U
#define SENSOR_ULTRASONIC_TRIGGER_PULSE_US 10U

#if (SENSOR_FSR_FILTER_SAMPLES == 0U)
#error "FSR filter must contain at least one sample"
#endif

#if (SENSOR_LINE_LEFT_WHITE_RAW >= SENSOR_LINE_LEFT_BLACK_RAW)
#error "Left line sensor white calibration must be lower than black calibration"
#endif

#if (SENSOR_LINE_RIGHT_WHITE_RAW >= SENSOR_LINE_RIGHT_BLACK_RAW)
#error "Right line sensor white calibration must be lower than black calibration"
#endif

#if ((SENSOR_LINE_FILTER_PREVIOUS_WEIGHT + 1U) != SENSOR_LINE_FILTER_DIVISOR)
#error "Line sensor filter divisor must include previous and current weights"
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

#if (SENSOR_MARKER_GROUP_TIMEOUT_MS <= SENSOR_MARKER_REARM_MS)
#error "Marker group timeout must exceed marker rearm time"
#endif

#if (SENSOR_MARKER_GROUP_TIMEOUT_MS >= SENSOR_LINE_LOST_TIMEOUT_MS)
#error "Marker group timeout must be shorter than line-lost time"
#endif

#if (SENSOR_MARKER_MAX_STRIPES != 4U)
#error "Marker contract currently defines exactly four valid stripe counts"
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

#if (SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES == 0U)
#error "Ultrasonic failure filter must contain at least one sample"
#endif

#if (SENSOR_ULTRASONIC_RECOVERY_SUCCESSES == 0U)
#error "Ultrasonic recovery filter must contain at least one sample"
#endif

#if (SENSOR_ULTRASONIC_STALE_MS <= (APP_TIMING_ULTRASONIC_PERIOD_MS * 4U))
#error "Ultrasonic stale time must exceed one complete four-sensor cycle"
#endif

#endif /* SENSOR_CONFIG_H */
