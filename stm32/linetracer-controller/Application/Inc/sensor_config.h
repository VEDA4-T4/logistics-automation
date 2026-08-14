#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include "app_timing.h"

/*
 * Explicit opt-in for route-only hardware testing.
 *
 * Define LINETRACER_ROUTE_TEST_MODE=1 only in the CubeIDE
 * Debug configuration
 * used for route testing. Raw diagnostics remain available, but ultrasonic
 * findings are
 * excluded from SafetyTask.
 */
#ifndef LINETRACER_ROUTE_TEST_MODE
#define LINETRACER_ROUTE_TEST_MODE 0U
#endif

#if LINETRACER_ROUTE_TEST_MODE
#define SENSOR_ROUTE_TEST_IGNORED_ERROR_FLAGS                                                            \
    (SENSOR_LOGIC_ERROR_FSR_ADC | SENSOR_LOGIC_ERROR_FSR_TIMEOUT | SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | \
     SENSOR_LOGIC_ERROR_ULTRASONIC_REAR | SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT)
#define SENSOR_ROUTE_TEST_IGNORED_OBSTACLE_MASK                                                 \
    (SENSOR_LOGIC_DIRECTION_FRONT | SENSOR_LOGIC_DIRECTION_REAR | SENSOR_LOGIC_DIRECTION_LEFT | \
     SENSOR_LOGIC_DIRECTION_RIGHT)
#else
#define SENSOR_ROUTE_TEST_IGNORED_ERROR_FLAGS 0U
#define SENSOR_ROUTE_TEST_IGNORED_OBSTACLE_MASK 0U
#endif

/*
 * TCRT5000 digital modules commonly drive LOW on black. Set this to 0U if
 * hardware calibration shows that black
 * produces HIGH.
 */
#define SENSOR_LINE_ACTIVE_LOW 0U

/*
 * TCRT5000 analog calibration measured on the current white track and black
 * line. Values outside the calibrated interval are saturated.
 */
#define SENSOR_LINE_LEFT_WHITE_RAW 230U
#define SENSOR_LINE_LEFT_BLACK_RAW 3150U
#define SENSOR_LINE_RIGHT_WHITE_RAW 215U
#define SENSOR_LINE_RIGHT_BLACK_RAW 3400U
#define SENSOR_LINE_CENTER_WHITE_RAW 500U
#define SENSOR_LINE_CENTER_BLACK_RAW 1200U
#define SENSOR_LINE_NORMALIZED_MAX 1000U
#define SENSOR_LINE_FILTER_PREVIOUS_WEIGHT 3U
#define SENSOR_LINE_FILTER_DIVISOR 4U
#define SENSOR_LINE_BLACK_ENTER_THRESHOLD 650U
#define SENSOR_LINE_BLACK_EXIT_THRESHOLD 450U

/*
 * DO decides the debounced L/C/R state used for tracking and marker
 * detection. AO remains available for
 * diagnostics and later PID tuning, but
 * Set this to 1U to use the outer AO sensors for continuous PID correction.
 *
 * DO continues to provide the debounced L/C/R state and intersection marker
 * detection.
 */
#define SENSOR_LINE_USE_ANALOG_PID 1U

/*
 * When the black guide tape is centred between the two outer sensors, both
 * sensors see the white board. Small
 * left/right reflectance differences in
 * that state must not make the vehicle continuously steer. Values inside
 *
 * this normalized error band are therefore treated as centred.
 */
#define SENSOR_LINE_ERROR_DEADBAND 100U

/*
 * A confirmed outer DO hit owns the PID direction. AO then only supplies a
 * magnitude in that direction. This
 * floor gives the PID at least the same
 * initial correction as the discrete 80 PWM recovery rule with Kp = 0.25.
 */
#define SENSOR_LINE_DO_PID_MIN_ERROR 400U

/*
 * The 18 mm black guide tape normally runs between the two sensors. A
 * transverse black marker covers both sensors
 * at the same time. Hysteresis,
 * debounce, and minimum duration prevent a noisy sample from becoming a
 * marker
 * stripe.
 */
#define SENSOR_MARKER_MIN_BLACK_MS 20U
#define SENSOR_MARKER_MAX_BLACK_MS 600U
#define SENSOR_MARKER_REARM_MS 50U
#define SENSOR_MARKER_GROUP_TIMEOUT_MS 350U
#define SENSOR_MARKER_MAX_STRIPES 4U

/*
 * FSR ADC1_IN0 is 12-bit (0..4095). Its absolute value varies by parking
 * position and contact area, so capture a
 * local reference only after the
 * vehicle stops at each pickup or unload destination.
 */
#define SENSOR_FSR_FILTER_SAMPLES 8U
#define SENSOR_FSR_BASELINE_SAMPLES 24U
#define SENSOR_FSR_LOAD_ON_DELTA 60U
#define SENSOR_FSR_LOAD_OFF_DELTA 250U
#define SENSOR_FSR_OVERLOAD_THRESHOLD 3800U
#define SENSOR_FSR_OVERLOAD_CLEAR_THRESHOLD 3400U
#define SENSOR_FSR_ADC_TIMEOUT_MS 50U

/* HC-SR04 operating range and obstacle hysteresis. */
#define SENSOR_ULTRASONIC_REAR_ENABLED 0U
#define SENSOR_ULTRASONIC_MIN_MM 15U
#define SENSOR_ULTRASONIC_MAX_MM 4000U
#define SENSOR_OBSTACLE_ON_MM 30U
#define SENSOR_OBSTACLE_OFF_MM 50U
#define SENSOR_OBSTACLE_ACTIVATE_SAMPLES 1U
#define SENSOR_OBSTACLE_CLEAR_SAMPLES 1U
#define SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES 20U
#define SENSOR_ULTRASONIC_RECOVERY_SUCCESSES 3U
#define SENSOR_ULTRASONIC_ECHO_TIMEOUT_MS 40U
#define SENSOR_ULTRASONIC_STALE_MS 3000U

/* Any valid close obstacle can pause movement, regardless of direction. */
#define SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY 0U

/*
 * A missing echo is retained in diagnostics, but must not latch the vehicle
 * into SENSOR_FAULT. Only a valid,
 * close distance produces an obstacle stop.
 */
#define SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT 0U

/* TIM1 is configured at runtime to count one tick per microsecond. */
#define SENSOR_ULTRASONIC_TIMER_HZ 1000000U
#define SENSOR_ULTRASONIC_TRIGGER_PULSE_US 10U

#if ((LINETRACER_ROUTE_TEST_MODE != 0U) && (LINETRACER_ROUTE_TEST_MODE != 1U))
#error "LINETRACER_ROUTE_TEST_MODE must be either 0 or 1"
#endif

#if ((SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY != 0U) && (SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY != 1U))
#error "SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY must be either 0 or 1"
#endif

#if ((SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT != 0U) && (SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT != 1U))
#error "SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT must be either 0 or 1"
#endif

#if ((SENSOR_ULTRASONIC_REAR_ENABLED != 0U) && (SENSOR_ULTRASONIC_REAR_ENABLED != 1U))
#error "SENSOR_ULTRASONIC_REAR_ENABLED must be either 0 or 1"
#endif

#if (SENSOR_FSR_FILTER_SAMPLES == 0U)
#error "FSR filter must contain at least one sample"
#endif

#if (SENSOR_LINE_LEFT_WHITE_RAW >= SENSOR_LINE_LEFT_BLACK_RAW)
#error "Left line sensor white calibration must be lower than black calibration"
#endif

#if (SENSOR_LINE_RIGHT_WHITE_RAW >= SENSOR_LINE_RIGHT_BLACK_RAW)
#error "Right line sensor white calibration must be lower than black calibration"
#endif

#if (SENSOR_LINE_CENTER_WHITE_RAW >= SENSOR_LINE_CENTER_BLACK_RAW)
#error "Center line sensor white calibration must be lower than black calibration"
#endif

#if ((SENSOR_LINE_FILTER_PREVIOUS_WEIGHT + 1U) != SENSOR_LINE_FILTER_DIVISOR)
#error "Line sensor filter divisor must include previous and current weights"
#endif

#if (SENSOR_LINE_BLACK_EXIT_THRESHOLD >= SENSOR_LINE_BLACK_ENTER_THRESHOLD)
#error "Black exit threshold must be lower than black enter threshold"
#endif

#if (SENSOR_LINE_BLACK_ENTER_THRESHOLD > SENSOR_LINE_NORMALIZED_MAX)
#error "Black enter threshold must fit the normalized line-sensor range"
#endif

#if ((SENSOR_LINE_USE_ANALOG_PID != 0U) && (SENSOR_LINE_USE_ANALOG_PID != 1U))
#error "SENSOR_LINE_USE_ANALOG_PID must be either 0 or 1"
#endif

#if (SENSOR_LINE_ERROR_DEADBAND > SENSOR_LINE_NORMALIZED_MAX)
#error "Line error deadband must fit the normalized line-sensor range"
#endif

#if ((SENSOR_LINE_DO_PID_MIN_ERROR == 0U) || (SENSOR_LINE_DO_PID_MIN_ERROR > SENSOR_LINE_NORMALIZED_MAX))
#error "DO-guided PID minimum error must fit the normalized line-sensor range"
#endif

#if (SENSOR_MARKER_MIN_BLACK_MS >= SENSOR_MARKER_MAX_BLACK_MS)
#error "Marker minimum black time must be shorter than marker maximum black time"
#endif

#if (SENSOR_MARKER_REARM_MS < APP_TIMING_SENSOR_PERIOD_MS)
#error "Marker rearm time must cover at least one SensorTask period"
#endif

#if (SENSOR_MARKER_GROUP_TIMEOUT_MS <= SENSOR_MARKER_REARM_MS)
#error "Marker group timeout must exceed marker rearm time"
#endif

#if (SENSOR_MARKER_MAX_STRIPES != 4U)
#error "Marker contract currently defines exactly four valid stripe counts"
#endif

#if (SENSOR_FSR_BASELINE_SAMPLES == 0U)
#error "FSR baseline must contain at least one sample"
#endif

#if ((SENSOR_FSR_LOAD_ON_DELTA == 0U) || (SENSOR_FSR_LOAD_OFF_DELTA == 0U))
#error "FSR load change deltas must be greater than zero"
#endif

#if (SENSOR_FSR_OVERLOAD_CLEAR_THRESHOLD >= SENSOR_FSR_OVERLOAD_THRESHOLD)
#error "FSR overload-clear threshold must be lower than overload threshold"
#endif

#if (SENSOR_OBSTACLE_ON_MM >= SENSOR_OBSTACLE_OFF_MM)
#error "Obstacle-on distance must be shorter than obstacle-off distance"
#endif

#if (SENSOR_OBSTACLE_ACTIVATE_SAMPLES == 0U)
#error "Obstacle activation filter must contain at least one sample"
#endif

#if (SENSOR_OBSTACLE_CLEAR_SAMPLES == 0U)
#error "Obstacle clear filter must contain at least one sample"
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
