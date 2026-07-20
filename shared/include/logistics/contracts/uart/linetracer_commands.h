#ifndef LOGISTICS_CONTRACTS_UART_LINETRACER_COMMANDS_H
#define LOGISTICS_CONTRACTS_UART_LINETRACER_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Line-tracer device contract carried by the common binary UART frame.
 * Existing command values 0x40 through 0x43 are kept for compatibility.
 */
typedef enum {
    UART_CMD_LINETRACER_START_ROUTE = 0x40U,
    UART_CMD_LINETRACER_STOP = 0x41U,
    UART_CMD_LINETRACER_GET_STATUS = 0x42U,
    UART_CMD_LINETRACER_RESET = 0x43U,
    UART_CMD_LINETRACER_SET_CURRENT_POSITION = 0x44U,
    UART_CMD_LINETRACER_RESUME_DRIVE = 0x45U,
    UART_CMD_LINETRACER_MANUAL_UNLOAD = 0x46U
} uart_linetracer_command_t;

/* Specification names mapped to the existing public command names. */
#define UART_CMD_LINETRACER_ASSIGN_ROUTE UART_CMD_LINETRACER_START_ROUTE
#define UART_CMD_LINETRACER_STOP_DRIVE UART_CMD_LINETRACER_STOP
#define UART_CMD_LINETRACER_STATUS_REQUEST UART_CMD_LINETRACER_GET_STATUS
#define UART_CMD_LINETRACER_RESET_SYSTEM UART_CMD_LINETRACER_RESET

typedef enum {
    UART_LINETRACER_ROUTE_NONE = 0x00U,
    UART_LINETRACER_ROUTE_1 = 0x01U,
    UART_LINETRACER_ROUTE_2 = 0x02U,
    UART_LINETRACER_ROUTE_3 = 0x03U
} uart_linetracer_route_t;

#define UART_LINETRACER_ROUTE_A UART_LINETRACER_ROUTE_1
#define UART_LINETRACER_ROUTE_B UART_LINETRACER_ROUTE_2
#define UART_LINETRACER_ROUTE_C UART_LINETRACER_ROUTE_3
#define UART_LINETRACER_ROUTE_MIN UART_LINETRACER_ROUTE_1
#define UART_LINETRACER_ROUTE_MAX UART_LINETRACER_ROUTE_3

typedef enum {
    UART_LINETRACER_POSITION_NONE = 0x00U,
    UART_LINETRACER_POSITION_DEST_A = 0x01U,
    UART_LINETRACER_POSITION_DEST_B = 0x02U,
    UART_LINETRACER_POSITION_DEST_C = 0x03U
} uart_linetracer_position_t;

#define UART_LINETRACER_POSITION_MIN UART_LINETRACER_POSITION_DEST_A
#define UART_LINETRACER_POSITION_MAX UART_LINETRACER_POSITION_DEST_C

#define UART_LINETRACER_JOB_ID_NONE 0x0000U
#define UART_LINETRACER_JOB_ID_MIN 0x0001U
#define UART_LINETRACER_JOB_ID_MAX 0xFFFFU

/* Multi-byte values use the common protocol's little-endian ordering. */
#define UART_LINETRACER_START_JOB_ID_LOW_INDEX 0U
#define UART_LINETRACER_START_JOB_ID_HIGH_INDEX 1U
#define UART_LINETRACER_START_ROUTE_ID_INDEX 2U
#define UART_LINETRACER_START_PAYLOAD_SIZE 3U

#define UART_LINETRACER_STOP_JOB_ID_LOW_INDEX 0U
#define UART_LINETRACER_STOP_JOB_ID_HIGH_INDEX 1U
#define UART_LINETRACER_STOP_PAYLOAD_SIZE 2U

#define UART_LINETRACER_GET_STATUS_PAYLOAD_SIZE 0U
#define UART_LINETRACER_RESET_PAYLOAD_SIZE 0U

#define UART_LINETRACER_SET_POSITION_ID_INDEX 0U
#define UART_LINETRACER_SET_POSITION_PAYLOAD_SIZE 1U

#define UART_LINETRACER_RESUME_DRIVE_PAYLOAD_SIZE 0U
#define UART_LINETRACER_MANUAL_UNLOAD_PAYLOAD_SIZE 0U

typedef enum {
    UART_LINETRACER_STATE_IDLE = 0x00U,
    UART_LINETRACER_STATE_LOAD_WAIT = 0x01U,
    UART_LINETRACER_STATE_FOLLOWING_LINE = 0x02U,
    UART_LINETRACER_STATE_CORRECTING = 0x03U,
    UART_LINETRACER_STATE_ARRIVED = 0x04U,
    UART_LINETRACER_STATE_UNLOADING = 0x05U,
    UART_LINETRACER_STATE_STOPPED = 0x06U,
    UART_LINETRACER_STATE_FAULT = 0x07U,
    UART_LINETRACER_STATE_EMERGENCY_STOP = 0x08U
} uart_linetracer_state_t;

typedef enum {
    UART_LINETRACER_LOAD_EMPTY = 0x00U,
    UART_LINETRACER_LOAD_PRESENT = 0x01U,
    UART_LINETRACER_LOAD_UNLOADING = 0x02U
} uart_linetracer_load_state_t;

/* GET_STATUS response payload: common response header followed by device data. */
#define UART_LINETRACER_STATUS_STATE_INDEX UART_RESPONSE_HEADER_SIZE
#define UART_LINETRACER_STATUS_JOB_ID_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 1U)
#define UART_LINETRACER_STATUS_JOB_ID_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 2U)
#define UART_LINETRACER_STATUS_ROUTE_ID_INDEX (UART_RESPONSE_HEADER_SIZE + 3U)
#define UART_LINETRACER_STATUS_LOAD_STATE_INDEX (UART_RESPONSE_HEADER_SIZE + 4U)
#define UART_LINETRACER_STATUS_DATA_SIZE 5U
#define UART_LINETRACER_STATUS_PAYLOAD_SIZE (UART_RESPONSE_HEADER_SIZE + UART_LINETRACER_STATUS_DATA_SIZE)

/* DEVICE_STATUS flags used by the periodic heartbeat. */
#define UART_LINETRACER_FLAG_LINE_DETECTED (1U << 0U)
#define UART_LINETRACER_FLAG_OBSTACLE_DETECTED (1U << 1U)
#define UART_LINETRACER_FLAG_LOAD_PRESENT (1U << 2U)
#define UART_LINETRACER_FLAG_ROUTE_ACTIVE (1U << 3U)

typedef enum {
    UART_LINETRACER_EVENT_ARRIVED = 0x01U,
    UART_LINETRACER_EVENT_LOAD_DETECTED = 0x02U,
    UART_LINETRACER_EVENT_UNLOAD_COMPLETE = 0x03U,
    UART_LINETRACER_EVENT_STATE_CHANGED = 0x04U,
    UART_LINETRACER_EVENT_FAULT = 0x05U
} uart_linetracer_event_t;

#define UART_LINETRACER_EVENT_JOB_ID_LOW_INDEX UART_EVENT_HEADER_SIZE
#define UART_LINETRACER_EVENT_JOB_ID_HIGH_INDEX (UART_EVENT_HEADER_SIZE + 1U)
#define UART_LINETRACER_EVENT_ROUTE_ID_INDEX (UART_EVENT_HEADER_SIZE + 2U)
#define UART_LINETRACER_JOB_EVENT_DATA_SIZE 3U
#define UART_LINETRACER_JOB_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_LINETRACER_JOB_EVENT_DATA_SIZE)

#define UART_LINETRACER_STATE_EVENT_STATE_INDEX (UART_EVENT_HEADER_SIZE + 3U)
#define UART_LINETRACER_STATE_EVENT_DATA_SIZE 4U
#define UART_LINETRACER_STATE_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_LINETRACER_STATE_EVENT_DATA_SIZE)

#define UART_LINETRACER_FAULT_EVENT_ERROR_INDEX (UART_EVENT_HEADER_SIZE + 3U)
#define UART_LINETRACER_FAULT_EVENT_DATA_SIZE 4U
#define UART_LINETRACER_FAULT_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_LINETRACER_FAULT_EVENT_DATA_SIZE)

static inline uint8_t uart_linetracer_command_is_valid(uint32_t command) {
    switch (command) {
        case UART_CMD_LINETRACER_START_ROUTE:
        case UART_CMD_LINETRACER_STOP:
        case UART_CMD_LINETRACER_GET_STATUS:
        case UART_CMD_LINETRACER_RESET:
        case UART_CMD_LINETRACER_SET_CURRENT_POSITION:
        case UART_CMD_LINETRACER_RESUME_DRIVE:
        case UART_CMD_LINETRACER_MANUAL_UNLOAD:
            return 1U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_LINETRACER_COMMAND(command) uart_linetracer_command_is_valid((uint32_t)(command))

static inline uint8_t uart_linetracer_route_is_valid(uint32_t route_id) {
    return (route_id >= UART_LINETRACER_ROUTE_MIN && route_id <= UART_LINETRACER_ROUTE_MAX) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_status_route_is_valid(uint32_t route_id) {
    return (route_id == UART_LINETRACER_ROUTE_NONE) ? 1U : uart_linetracer_route_is_valid(route_id);
}

static inline uint8_t uart_linetracer_position_is_valid(uint32_t position) {
    return (position >= UART_LINETRACER_POSITION_MIN && position <= UART_LINETRACER_POSITION_MAX) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_status_position_is_valid(uint32_t position) {
    return (position == UART_LINETRACER_POSITION_NONE) ? 1U : uart_linetracer_position_is_valid(position);
}

static inline uint8_t uart_linetracer_job_id_is_valid(uint32_t job_id) {
    return (job_id >= UART_LINETRACER_JOB_ID_MIN && job_id <= UART_LINETRACER_JOB_ID_MAX) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_state_is_valid(uint32_t state) {
    return (state <= UART_LINETRACER_STATE_EMERGENCY_STOP) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_load_state_is_valid(uint32_t state) {
    return (state <= UART_LINETRACER_LOAD_UNLOADING) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_event_is_valid(uint32_t event_id) {
    return (event_id >= UART_LINETRACER_EVENT_ARRIVED && event_id <= UART_LINETRACER_EVENT_FAULT) ? 1U : 0U;
}

static inline uint8_t uart_linetracer_fault_error_is_valid(uint32_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
        case UART_ERROR_SENSOR:
        case UART_ERROR_MOTOR:
        case UART_ERROR_SERVO:
        case UART_ERROR_EMERGENCY_STOP:
        case UART_ERROR_INTERNAL:
            return 1U;

        default:
            return 0U;
    }
}

static inline uint16_t uart_linetracer_read_job_id(const uint8_t* payload, uint32_t low_index, uint32_t high_index) {
    if (payload == NULL) {
        return UART_LINETRACER_JOB_ID_NONE;
    }

    return (uint16_t)((uint16_t)payload[low_index] | ((uint16_t)payload[high_index] << 8U));
}

static inline uint16_t uart_linetracer_start_job_id(const uint8_t* payload) {
    return uart_linetracer_read_job_id(payload, UART_LINETRACER_START_JOB_ID_LOW_INDEX,
                                       UART_LINETRACER_START_JOB_ID_HIGH_INDEX);
}

static inline uint16_t uart_linetracer_stop_job_id(const uint8_t* payload) {
    return uart_linetracer_read_job_id(payload, UART_LINETRACER_STOP_JOB_ID_LOW_INDEX,
                                       UART_LINETRACER_STOP_JOB_ID_HIGH_INDEX);
}

static inline uint16_t uart_linetracer_event_job_id(const uint8_t* payload) {
    return uart_linetracer_read_job_id(payload, UART_LINETRACER_EVENT_JOB_ID_LOW_INDEX,
                                       UART_LINETRACER_EVENT_JOB_ID_HIGH_INDEX);
}

static inline uint8_t uart_linetracer_payload_is_valid(uint32_t command, const uint8_t* payload, uint32_t length) {
    uint16_t job_id;

    if (uart_linetracer_command_is_valid(command) == 0U || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    switch (command) {
        case UART_CMD_LINETRACER_GET_STATUS:
            return (length == UART_LINETRACER_GET_STATUS_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_LINETRACER_RESET:
            return (length == UART_LINETRACER_RESET_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_LINETRACER_RESUME_DRIVE:
            return (length == UART_LINETRACER_RESUME_DRIVE_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_LINETRACER_MANUAL_UNLOAD:
            return (length == UART_LINETRACER_MANUAL_UNLOAD_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_LINETRACER_SET_CURRENT_POSITION:
            if (length != UART_LINETRACER_SET_POSITION_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }
            return uart_linetracer_position_is_valid(payload[UART_LINETRACER_SET_POSITION_ID_INDEX]);

        case UART_CMD_LINETRACER_START_ROUTE:
            if (length != UART_LINETRACER_START_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            job_id = uart_linetracer_start_job_id(payload);
            if (uart_linetracer_job_id_is_valid(job_id) == 0U) {
                return 0U;
            }
            return uart_linetracer_route_is_valid(payload[UART_LINETRACER_START_ROUTE_ID_INDEX]);

        case UART_CMD_LINETRACER_STOP:
            if (length != UART_LINETRACER_STOP_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }
            job_id = uart_linetracer_stop_job_id(payload);
            return uart_linetracer_job_id_is_valid(job_id);

        default:
            return 0U;
    }
}

#define UART_IS_VALID_LINETRACER_PAYLOAD(command, payload, length) \
    uart_linetracer_payload_is_valid((uint32_t)(command), (const uint8_t*)(payload), (uint32_t)(length))

static inline uint8_t uart_linetracer_event_payload_is_valid(const uint8_t* payload, uint32_t length) {
    uint32_t event_id;
    uint16_t job_id;

    if (payload == NULL || length < UART_EVENT_HEADER_SIZE || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    event_id = payload[UART_EVENT_ID_INDEX];
    if (uart_linetracer_event_is_valid(event_id) == 0U) {
        return 0U;
    }

    switch (event_id) {
        case UART_LINETRACER_EVENT_ARRIVED:
        case UART_LINETRACER_EVENT_LOAD_DETECTED:
        case UART_LINETRACER_EVENT_UNLOAD_COMPLETE:
            if (length != UART_LINETRACER_JOB_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }
            break;

        case UART_LINETRACER_EVENT_STATE_CHANGED:
            if (length != UART_LINETRACER_STATE_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }
            break;

        case UART_LINETRACER_EVENT_FAULT:
            if (length != UART_LINETRACER_FAULT_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }
            break;

        default:
            return 0U;
    }

    job_id = uart_linetracer_event_job_id(payload);
    if (uart_linetracer_job_id_is_valid(job_id) == 0U ||
        uart_linetracer_route_is_valid(payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX]) == 0U) {
        return 0U;
    }

    switch (event_id) {
        case UART_LINETRACER_EVENT_ARRIVED:
        case UART_LINETRACER_EVENT_LOAD_DETECTED:
        case UART_LINETRACER_EVENT_UNLOAD_COMPLETE:
            return 1U;

        case UART_LINETRACER_EVENT_STATE_CHANGED:
            return uart_linetracer_state_is_valid(payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX]);

        case UART_LINETRACER_EVENT_FAULT:
            return uart_linetracer_fault_error_is_valid(payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX]);

        default:
            return 0U;
    }
}

#define UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(payload, length) \
    uart_linetracer_event_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_LINETRACER_COMMANDS_H */
