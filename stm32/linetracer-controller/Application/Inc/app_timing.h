#ifndef APP_TIMING_H
#define APP_TIMING_H

#include "logistics/contracts/uart_protocol.h"

/* Periodic tasks. Event-driven tasks intentionally have no polling period. */
#define APP_TIMING_SAFETY_PERIOD_MS 5U
#define APP_TIMING_SENSOR_PERIOD_MS 10U
#define APP_TIMING_CONTROL_PERIOD_MS 10U
#define APP_TIMING_HEALTH_PERIOD_MS 500U

/* Slower work executed from the owning task without blocking other tasks. */
#define APP_TIMING_ULTRASONIC_PERIOD_MS 60U
#define APP_TIMING_COMM_TX_HEARTBEAT_MS 1000U
#define APP_TIMING_UNLOAD_STEP_MS 20U

/* Sensor filtering and communication supervision. */
#define APP_TIMING_LINE_DEBOUNCE_SAMPLES 1U
#define APP_TIMING_FSR_STABLE_MS 300U
#define APP_TIMING_UART_RX_TIMEOUT_MS UART_COMMAND_TIMEOUT_MS

#if (APP_TIMING_CONTROL_PERIOD_MS % APP_TIMING_SAFETY_PERIOD_MS) != 0U
#error "ControlTask period must be a multiple of SafetyTask period"
#endif

#if (APP_TIMING_ULTRASONIC_PERIOD_MS % APP_TIMING_SENSOR_PERIOD_MS) != 0U
#error "Ultrasonic period must be a multiple of SensorTask period"
#endif

#endif /* APP_TIMING_H */
