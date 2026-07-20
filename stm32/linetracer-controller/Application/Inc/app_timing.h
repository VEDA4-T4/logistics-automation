#ifndef APP_TIMING_H
#define APP_TIMING_H

#include "logistics/contracts/uart_protocol.h"

/* Periodic tasks. Event-driven tasks intentionally have no polling period. */
#define APP_TIMING_SAFETY_PERIOD_MS 5U
#define APP_TIMING_SENSOR_PERIOD_MS 10U
#define APP_TIMING_CONTROL_PERIOD_MS 10U
#define APP_TIMING_HEALTH_PERIOD_MS 500U

/* Slower work executed from the owning task without blocking other tasks. */
#define APP_TIMING_ULTRASONIC_PERIOD_MS 50U
#define APP_TIMING_COMM_TX_HEARTBEAT_MS 1000U
#define APP_TIMING_UNLOAD_STEP_MS 20U

/* Sensor filtering and communication supervision. */
#define APP_TIMING_LINE_DEBOUNCE_SAMPLES 3U
#define APP_TIMING_MARKER_MIN_MS 20U
#define APP_TIMING_MARKER_MAX_MS 300U
#define APP_TIMING_LINE_LOST_MS 500U
#define APP_TIMING_FSR_STABLE_MS 300U
#define APP_TIMING_FSR_ADC_TIMEOUT_MS 50U
#define APP_TIMING_ULTRASONIC_ECHO_TIMEOUT_MS 30U
#define APP_TIMING_ULTRASONIC_STALE_MS 750U
#define APP_TIMING_UART_RX_TIMEOUT_MS UART_COMMAND_TIMEOUT_MS

#if (APP_TIMING_CONTROL_PERIOD_MS % APP_TIMING_SAFETY_PERIOD_MS) != 0U
#error "ControlTask period must be a multiple of SafetyTask period"
#endif

#if (APP_TIMING_ULTRASONIC_PERIOD_MS % APP_TIMING_SENSOR_PERIOD_MS) != 0U
#error "Ultrasonic period must be a multiple of SensorTask period"
#endif

#if (APP_TIMING_MARKER_MIN_MS >= APP_TIMING_MARKER_MAX_MS)
#error "Marker minimum time must be shorter than marker maximum time"
#endif

#if (APP_TIMING_MARKER_MAX_MS >= APP_TIMING_LINE_LOST_MS)
#error "Marker maximum time must be shorter than line-lost time"
#endif

#if (APP_TIMING_ULTRASONIC_STALE_MS <= (APP_TIMING_ULTRASONIC_PERIOD_MS * 4U))
#error "Ultrasonic stale time must exceed one complete four-sensor cycle"
#endif

#endif /* APP_TIMING_H */
