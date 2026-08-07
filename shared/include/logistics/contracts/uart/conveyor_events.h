#ifndef LOGISTICS_CONTRACTS_UART_CONVEYOR_EVENTS_H
#define LOGISTICS_CONTRACTS_UART_CONVEYOR_EVENTS_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { APP_EVENT_HEARTBEAT = 0x01U, APP_EVENT_SAFETY = 0x03U, APP_EVENT_HEALTH = 0x04U } app_event_id_t;

#define APP_HEARTBEAT_STATE_INDEX 1U
#define APP_HEARTBEAT_ERROR_INDEX 2U
#define APP_HEARTBEAT_UPTIME_INDEX 3U
#define APP_HEARTBEAT_INPUT_SENSOR_INDEX 7U
#define APP_HEARTBEAT_SORTING_SENSOR_INDEX 8U
#define APP_HEARTBEAT_PAYLOAD_SIZE 9U

typedef enum {
    SAFETY_CAUSE_NONE = 0U,
    SAFETY_CAUSE_ESTOP_INPUT_PI = 1U,
    SAFETY_CAUSE_ESTOP_SORTING_PI = 2U,
    SAFETY_CAUSE_FATAL_ERROR = 3U
} safety_cause_t;

#define APP_SAFETY_EVENT_KIND_INDEX 1U
#define APP_SAFETY_EVENT_CAUSE_INDEX 2U
#define APP_SAFETY_EVENT_TIMESTAMP_INDEX 3U
#define APP_SAFETY_EVENT_RESULT_INDEX 7U
#define APP_SAFETY_EVENT_PAYLOAD_SIZE 8U

typedef enum {
    SAFETY_EVENT_ESTOP_LATCHED = 1U,
    SAFETY_EVENT_RESET_COMPLETE = 2U,
    SAFETY_EVENT_RESET_REJECTED = 3U
} safety_event_kind_t;

typedef enum {
    SAFETY_RESET_OK = 0U,
    SAFETY_RESET_INPUT_NOT_READY = 1U,
    SAFETY_RESET_SORTING_NOT_READY = 2U,
    SAFETY_RESET_TIMEOUT = 3U
} safety_reset_result_t;

#define APP_HEALTH_EVENT_KIND_INDEX 1U
#define APP_HEALTH_EVENT_CAUSE_INDEX 2U
#define APP_HEALTH_EVENT_SENSOR_ID_INDEX 3U
#define APP_HEALTH_EVENT_TIMESTAMP_INDEX 4U
#define APP_HEALTH_EVENT_PAYLOAD_SIZE 8U

#define HEALTH_ISSUE_CAUSE_INPUT 0U
#define HEALTH_ISSUE_CAUSE_SORTING 1U
#define HEALTH_ISSUE_CAUSE_DEVICE_WIDE 0xFFU
#define HEALTH_ISSUE_SENSOR_ID_MIN 1U
#define HEALTH_ISSUE_SENSOR_ID_MAX 3U
#define HEALTH_ISSUE_SENSOR_ID_NONE 0xFFU

typedef enum {
    HEALTH_ISSUE_UART_CHANNEL_TIMEOUT = 1U,
    HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT = 2U,
    HEALTH_ISSUE_SENSOR_STALE = 3U,
    HEALTH_ISSUE_UART_RECOVERY = 4U
} health_issue_kind_t;

static inline uint8_t uart_app_error_is_valid(uint32_t error) {
    return (error <= UART_ERROR_INVALID_PAYLOAD || error == UART_ERROR_INTERNAL) ? 1U : 0U;
}

static inline uint8_t uart_app_heartbeat_payload_is_valid(const uint8_t* payload, uint32_t length) {
    if (payload == (const uint8_t*)0 || length != APP_HEARTBEAT_PAYLOAD_SIZE ||
        payload[UART_EVENT_ID_INDEX] != APP_EVENT_HEARTBEAT) {
        return 0U;
    }

    return (payload[APP_HEARTBEAT_STATE_INDEX] <= UART_DEVICE_EMERGENCY_STOP &&
            uart_app_error_is_valid(payload[APP_HEARTBEAT_ERROR_INDEX]) != 0U &&
            payload[APP_HEARTBEAT_INPUT_SENSOR_INDEX] <= UART_SENSOR_FAULT &&
            payload[APP_HEARTBEAT_SORTING_SENSOR_INDEX] <= UART_SENSOR_FAULT)
               ? 1U
               : 0U;
}

static inline uint8_t uart_app_safety_payload_is_valid(const uint8_t* payload, uint32_t length) {
    uint8_t kind;
    uint8_t result;

    if (payload == (const uint8_t*)0 || length != APP_SAFETY_EVENT_PAYLOAD_SIZE ||
        payload[UART_EVENT_ID_INDEX] != APP_EVENT_SAFETY ||
        payload[APP_SAFETY_EVENT_CAUSE_INDEX] > SAFETY_CAUSE_FATAL_ERROR) {
        return 0U;
    }

    kind = payload[APP_SAFETY_EVENT_KIND_INDEX];
    result = payload[APP_SAFETY_EVENT_RESULT_INDEX];
    if (kind == SAFETY_EVENT_ESTOP_LATCHED || kind == SAFETY_EVENT_RESET_COMPLETE) {
        return (result == SAFETY_RESET_OK) ? 1U : 0U;
    }
    if (kind == SAFETY_EVENT_RESET_REJECTED) {
        return (result >= SAFETY_RESET_INPUT_NOT_READY && result <= SAFETY_RESET_TIMEOUT) ? 1U : 0U;
    }
    return 0U;
}

static inline uint8_t uart_app_health_payload_is_valid(const uint8_t* payload, uint32_t length) {
    uint8_t kind;
    uint8_t cause;
    uint8_t sensor_id;

    if (payload == (const uint8_t*)0 || length != APP_HEALTH_EVENT_PAYLOAD_SIZE ||
        payload[UART_EVENT_ID_INDEX] != APP_EVENT_HEALTH) {
        return 0U;
    }

    kind = payload[APP_HEALTH_EVENT_KIND_INDEX];
    cause = payload[APP_HEALTH_EVENT_CAUSE_INDEX];
    sensor_id = payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX];
    if (kind < HEALTH_ISSUE_UART_CHANNEL_TIMEOUT || kind > HEALTH_ISSUE_UART_RECOVERY ||
        (cause != HEALTH_ISSUE_CAUSE_INPUT && cause != HEALTH_ISSUE_CAUSE_SORTING &&
         cause != HEALTH_ISSUE_CAUSE_DEVICE_WIDE)) {
        return 0U;
    }
    if (kind == HEALTH_ISSUE_SENSOR_STALE) {
        return (cause != HEALTH_ISSUE_CAUSE_DEVICE_WIDE && sensor_id >= HEALTH_ISSUE_SENSOR_ID_MIN &&
                sensor_id <= HEALTH_ISSUE_SENSOR_ID_MAX)
                   ? 1U
                   : 0U;
    }
    return (sensor_id == HEALTH_ISSUE_SENSOR_ID_NONE) ? 1U : 0U;
}

#define UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(payload, length) \
    uart_app_heartbeat_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))
#define UART_IS_VALID_APP_SAFETY_PAYLOAD(payload, length) \
    uart_app_safety_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))
#define UART_IS_VALID_APP_HEALTH_PAYLOAD(payload, length) \
    uart_app_health_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_CONVEYOR_EVENTS_H */
