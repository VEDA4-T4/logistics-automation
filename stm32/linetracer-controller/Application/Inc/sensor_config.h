#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

/*
 * TCRT5000 digital modules commonly drive LOW on black. Set this to 0U if
 * hardware calibration shows that black produces HIGH.
 */
#define SENSOR_LINE_ACTIVE_LOW 1U

/* FSR ADC1_IN0 is 12-bit (0..4095). Calibrate these with the actual load. */
#define SENSOR_FSR_FILTER_SAMPLES 8U
#define SENSOR_FSR_LOAD_ON_THRESHOLD 2200U
#define SENSOR_FSR_LOAD_OFF_THRESHOLD 1700U
#define SENSOR_FSR_OVERLOAD_ON_THRESHOLD 3800U
#define SENSOR_FSR_OVERLOAD_OFF_THRESHOLD 3400U

/* HC-SR04 operating range and obstacle hysteresis. */
#define SENSOR_ULTRASONIC_MIN_MM 20U
#define SENSOR_ULTRASONIC_MAX_MM 4000U
#define SENSOR_OBSTACLE_ON_MM 150U
#define SENSOR_OBSTACLE_OFF_MM 220U
#define SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES 3U

/* TIM1 is configured at runtime to count one tick per microsecond. */
#define SENSOR_ULTRASONIC_TIMER_HZ 1000000U
#define SENSOR_ULTRASONIC_TRIGGER_PULSE_US 10U

#if (SENSOR_FSR_FILTER_SAMPLES == 0U)
#error "FSR filter must contain at least one sample"
#endif

#if (SENSOR_FSR_LOAD_OFF_THRESHOLD >= SENSOR_FSR_LOAD_ON_THRESHOLD)
#error "FSR LOAD_OFF threshold must be lower than LOAD_ON threshold"
#endif

#if (SENSOR_FSR_OVERLOAD_OFF_THRESHOLD >= SENSOR_FSR_OVERLOAD_ON_THRESHOLD)
#error "FSR overload-off threshold must be lower than overload-on threshold"
#endif

#if (SENSOR_OBSTACLE_ON_MM >= SENSOR_OBSTACLE_OFF_MM)
#error "Obstacle-on distance must be shorter than obstacle-off distance"
#endif

#if (SENSOR_ULTRASONIC_MIN_MM >= SENSOR_ULTRASONIC_MAX_MM)
#error "Ultrasonic minimum distance must be shorter than maximum distance"
#endif

#endif /* SENSOR_CONFIG_H */
