#ifndef APP_MESSAGES_H
#define APP_MESSAGES_H

#include <stdint.h>

#include "linetracer_control_types.h"
#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All queue messages contain values only; no message owns a pointer. */
typedef enum {
    APP_TASK_SENSOR = 0,
    APP_TASK_COMM_RX,
    APP_TASK_CONTROL,
    APP_TASK_SAFETY,
    APP_TASK_COMM_TX,
    APP_TASK_HEALTH,
    APP_TASK_UNLOAD,
    APP_TASK_COUNT
} app_task_id_t;

typedef enum {
    APP_CONTROL_COMMAND_NONE = 0,
    APP_CONTROL_COMMAND_ASSIGN_ROUTE,
    APP_CONTROL_COMMAND_SET_CURRENT_POSITION,
    APP_CONTROL_COMMAND_STOP_DRIVE,
    APP_CONTROL_COMMAND_RESUME_DRIVE,
    APP_CONTROL_COMMAND_MANUAL_UNLOAD,
    APP_CONTROL_COMMAND_RESET_SYSTEM,
    APP_CONTROL_COMMAND_STATUS_REQUEST
} app_control_command_type_t;

typedef struct {
    app_control_command_type_t type;
    uint32_t received_at_ms;
    uint16_t job_id;
    uint16_t original_payload_crc;
    uart_linetracer_route_t route_id;
    uart_linetracer_position_t position;
    uint8_t sequence;
    uint8_t original_command;
    uint8_t original_payload_length;
} app_control_command_t;

/* Thread-safe ControlTask snapshot consumed by telemetry producers. */
typedef struct {
    uint32_t updated_at_ms;
    uint16_t job_id;
    uart_linetracer_route_t route_id;
    uart_linetracer_state_t state;
    uart_linetracer_load_state_t load_state;
    uint8_t error_code;
    uint8_t safety_latched;
} app_control_snapshot_t;

typedef enum {
    APP_SENSOR_EVENT_NONE = 0U,
    APP_SENSOR_EVENT_LINE_CHANGED = (1U << 0U),
    APP_SENSOR_EVENT_MARKER = (1U << 1U),
    APP_SENSOR_EVENT_LINE_LOST = (1U << 2U),
    APP_SENSOR_EVENT_LOAD_ON = (1U << 3U),
    APP_SENSOR_EVENT_LOAD_OFF = (1U << 4U),
    APP_SENSOR_EVENT_OVERLOAD = (1U << 5U),
    APP_SENSOR_EVENT_OBSTACLE = (1U << 6U)
} app_sensor_event_flags_t;

typedef struct {
    uint32_t sampled_at_ms;
    uint32_t event_flags;
    uint16_t fsr_raw;
    uint16_t ultrasonic_front_mm;
    uint16_t ultrasonic_rear_mm;
    uint16_t ultrasonic_left_mm;
    uint16_t ultrasonic_right_mm;
    linetracer_line_state_t line_state;
    uart_linetracer_load_state_t load_state;
    uint8_t line_left;
    uint8_t line_right;
} app_sensor_snapshot_t;

typedef enum {
    APP_SAFETY_EVENT_NONE = 0,
    APP_SAFETY_EVENT_EMERGENCY_STOP,
    APP_SAFETY_EVENT_LINE_LOST,
    APP_SAFETY_EVENT_OBSTACLE,
    APP_SAFETY_EVENT_LOAD_LOST,
    APP_SAFETY_EVENT_OVERLOAD,
    APP_SAFETY_EVENT_TURN_TIMEOUT,
    APP_SAFETY_EVENT_MARKER_SEQUENCE,
    APP_SAFETY_EVENT_COMM_TIMEOUT,
    APP_SAFETY_EVENT_RESET_REQUEST,
    APP_SAFETY_EVENT_SENSOR_FAULT
} app_safety_event_type_t;

typedef struct {
    app_safety_event_type_t type;
    uint32_t occurred_at_ms;
    linetracer_stop_reason_t reason;
    uint16_t original_payload_crc;
    app_task_id_t source_task;
    uint8_t error_code;
    uint8_t active;
    uint8_t request_sequence;
    uint8_t original_command;
    uint8_t original_payload_length;
} app_safety_event_t;

/* SafetyTask is the single producer; ControlTask is the single consumer. */
typedef enum {
    APP_CONTROL_SAFETY_NONE = 0,
    APP_CONTROL_SAFETY_LATCHED,
    APP_CONTROL_SAFETY_RESET_APPROVED,
    APP_CONTROL_SAFETY_RESET_REJECTED
} app_control_safety_event_type_t;

typedef struct {
    app_control_safety_event_type_t type;
    uint32_t occurred_at_ms;
    linetracer_stop_reason_t reason;
    uint16_t original_payload_crc;
    uint8_t error_code;
    uint8_t request_sequence;
    uint8_t original_command;
    uint8_t original_payload_length;
} app_control_safety_event_t;

typedef enum {
    APP_UNLOAD_COMMAND_NONE = 0,
    APP_UNLOAD_COMMAND_START,
    APP_UNLOAD_COMMAND_ABORT,
    APP_UNLOAD_COMMAND_RESET
} app_unload_command_type_t;

typedef struct {
    app_unload_command_type_t type;
    uint32_t requested_at_ms;
    uint16_t job_id;
    uart_linetracer_route_t route_id;
} app_unload_command_t;

typedef enum {
    APP_TX_EVENT_NONE = 0,
    APP_TX_EVENT_COMMAND_ACK,
    APP_TX_EVENT_STATUS,
    APP_TX_EVENT_HEARTBEAT,
    APP_TX_EVENT_STARTED,
    APP_TX_EVENT_ARRIVED,
    APP_TX_EVENT_LOAD_DETECTED,
    APP_TX_EVENT_UNLOAD_COMPLETE,
    APP_TX_EVENT_STATE_CHANGED,
    APP_TX_EVENT_FAULT
} app_tx_event_type_t;

typedef enum {
    APP_TX_PRIORITY_TELEMETRY = 0,
    APP_TX_PRIORITY_EVENT = 1,
    APP_TX_PRIORITY_RESPONSE = 2,
    APP_TX_PRIORITY_SAFETY = 3
} app_tx_priority_t;

typedef struct {
    app_tx_event_type_t type;
    uint32_t created_at_ms;
    uint16_t job_id;
    uint16_t original_payload_crc;
    uart_linetracer_route_t route_id;
    uart_linetracer_state_t state;
    uart_linetracer_load_state_t load_state;
    uint8_t request_sequence;
    uint8_t original_command;
    uint8_t original_payload_length;
    uint8_t status;
    uint8_t error_code;
    uint8_t retry_count;
} app_tx_event_t;

static inline uint8_t app_tx_event_is_response(app_tx_event_type_t type) {
    return (type == APP_TX_EVENT_COMMAND_ACK || type == APP_TX_EVENT_STATUS) ? 1U : 0U;
}

static inline uint8_t app_tx_event_is_emergency(app_tx_event_type_t type) {
    return (type == APP_TX_EVENT_FAULT) ? 1U : 0U;
}

static inline uint8_t app_tx_event_priority(app_tx_event_type_t type) {
    switch (type) {
        case APP_TX_EVENT_COMMAND_ACK:
        case APP_TX_EVENT_STATUS:
            return APP_TX_PRIORITY_RESPONSE;

        case APP_TX_EVENT_FAULT:
            return APP_TX_PRIORITY_SAFETY;

        case APP_TX_EVENT_STARTED:
        case APP_TX_EVENT_ARRIVED:
        case APP_TX_EVENT_LOAD_DETECTED:
        case APP_TX_EVENT_UNLOAD_COMPLETE:
        case APP_TX_EVENT_STATE_CHANGED:
            return APP_TX_PRIORITY_EVENT;

        case APP_TX_EVENT_HEARTBEAT:
        case APP_TX_EVENT_NONE:
        default:
            return APP_TX_PRIORITY_TELEMETRY;
    }
}

typedef enum {
    APP_HEALTH_EVENT_NONE = 0,
    APP_HEALTH_EVENT_TASK_ALIVE,
    APP_HEALTH_EVENT_QUEUE_FULL,
    APP_HEALTH_EVENT_UART_RX_TIMEOUT,
    APP_HEALTH_EVENT_UART_TX_TIMEOUT,
    APP_HEALTH_EVENT_INTERNAL_ERROR
} app_health_event_type_t;

typedef struct {
    app_health_event_type_t type;
    uint32_t occurred_at_ms;
    uint32_t detail;
    app_task_id_t source_task;
} app_health_event_t;

/* Task notification bits are task-local even when the bit values overlap. */
#define APP_COMM_RX_NOTIFY_DATA_READY (1UL << 0U)
#define APP_COMM_RX_NOTIFY_UART_ERROR (1UL << 1U)
#define APP_SAFETY_NOTIFY_EMERGENCY_STOP (1UL << 0U)
#define APP_COMM_TX_NOTIFY_QUEUE_READY (1UL << 0U)
#define APP_COMM_TX_NOTIFY_TX_COMPLETE (1UL << 1U)
#define APP_COMM_TX_NOTIFY_ABORT_COMPLETE (1UL << 2U)

#ifdef __cplusplus
}
#endif

#endif /* APP_MESSAGES_H */
