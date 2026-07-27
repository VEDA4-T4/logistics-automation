#ifndef LOGISTICS_CONTRACTS_UART_GRIPPER_COMMANDS_H
#define LOGISTICS_CONTRACTS_UART_GRIPPER_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Gripper Raspberry Pi -> gripper-controller STM32 commands.
 *
 * Image coordinates, box dimensions, camera calibration, and inverse
 * kinematics belong to the Raspberry Pi. The STM32 receives calibrated joint
 * targets, validates its mechanical limits, and performs the servo motion.
 */
typedef enum {
    UART_CMD_GRIPPER_MOVE_ARM = 0x20U,
    UART_CMD_GRIPPER_SET_GRIPPER = 0x21U,
    UART_CMD_GRIPPER_HOME = 0x22U,
    UART_CMD_GRIPPER_STOP = 0x23U,
    UART_CMD_GRIPPER_GET_STATUS = 0x24U,
    UART_CMD_GRIPPER_RESET = 0x25U
} uart_gripper_command_t;

/* A zero motion ID means that no motion is active. */
#define UART_GRIPPER_MOTION_ID_NONE 0x0000U
#define UART_GRIPPER_MOTION_ID_MIN 0x0001U
#define UART_GRIPPER_MOTION_ID_MAX 0xFFFFU

/* Joint angles are unsigned deci-degrees: 900 means 90.0 degrees. */
#define UART_GRIPPER_ANGLE_DECI_DEG_MIN 0U
#define UART_GRIPPER_ANGLE_DECI_DEG_MAX 1800U

/* The controller interpolates each requested motion over this duration. */
#define UART_GRIPPER_DURATION_MS_MIN 100U
#define UART_GRIPPER_DURATION_MS_MAX 10000U

/* 0 is fully closed and 100 is fully open after hardware calibration. */
#define UART_GRIPPER_POSITION_MIN 0U
#define UART_GRIPPER_POSITION_MAX 100U

/* Device-specific RESPONSE error: HOME has not completed since boot/E-Stop. */
#define UART_GRIPPER_ERROR_NOT_HOMED 0x20U

/*
 * MOVE_ARM payload (10 bytes):
 *
 *   [0] motion_id_low
 *   [1] motion_id_high
 *   [2] base_angle_low
 *   [3] base_angle_high
 *   [4] shoulder_angle_low
 *   [5] shoulder_angle_high
 *   [6] elbow_angle_low
 *   [7] elbow_angle_high
 *   [8] duration_ms_low
 *   [9] duration_ms_high
 */
#define UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX 0U
#define UART_GRIPPER_MOVE_MOTION_ID_HIGH_INDEX 1U
#define UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX 2U
#define UART_GRIPPER_MOVE_BASE_ANGLE_HIGH_INDEX 3U
#define UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX 4U
#define UART_GRIPPER_MOVE_SHOULDER_ANGLE_HIGH_INDEX 5U
#define UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX 6U
#define UART_GRIPPER_MOVE_ELBOW_ANGLE_HIGH_INDEX 7U
#define UART_GRIPPER_MOVE_DURATION_LOW_INDEX 8U
#define UART_GRIPPER_MOVE_DURATION_HIGH_INDEX 9U
#define UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE 10U

/*
 * SET_GRIPPER payload (5 bytes):
 *
 *   [0] motion_id_low
 *   [1] motion_id_high
 *   [2] position_percent
 *   [3] duration_ms_low
 *   [4] duration_ms_high
 */
#define UART_GRIPPER_SET_MOTION_ID_LOW_INDEX 0U
#define UART_GRIPPER_SET_MOTION_ID_HIGH_INDEX 1U
#define UART_GRIPPER_SET_POSITION_INDEX 2U
#define UART_GRIPPER_SET_DURATION_LOW_INDEX 3U
#define UART_GRIPPER_SET_DURATION_HIGH_INDEX 4U
#define UART_GRIPPER_SET_GRIPPER_PAYLOAD_SIZE 5U

/* HOME starts an asynchronous motion and therefore carries a motion ID. */
#define UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX 0U
#define UART_GRIPPER_HOME_MOTION_ID_HIGH_INDEX 1U
#define UART_GRIPPER_HOME_PAYLOAD_SIZE 2U

#define UART_GRIPPER_STOP_PAYLOAD_SIZE 0U
#define UART_GRIPPER_GET_STATUS_PAYLOAD_SIZE 0U
#define UART_GRIPPER_RESET_PAYLOAD_SIZE 0U

typedef enum {
    UART_GRIPPER_STATE_IDLE = 0x00U,
    UART_GRIPPER_STATE_MOVING_ARM = 0x01U,
    UART_GRIPPER_STATE_MOVING_GRIPPER = 0x02U,
    UART_GRIPPER_STATE_HOMING = 0x03U,
    UART_GRIPPER_STATE_STOPPED = 0x04U,
    UART_GRIPPER_STATE_FAULT = 0x05U,
    UART_GRIPPER_STATE_EMERGENCY_STOP = 0x06U
} uart_gripper_state_t;

/*
 * GET_STATUS response data after the common response header:
 *
 *   [3]    gripper_state
 *   [4..5] active_motion_id
 *   [6..7] base_angle_deci_deg
 *   [8..9] shoulder_angle_deci_deg
 *   [10..11] elbow_angle_deci_deg
 *   [12]   gripper_position_percent
 *   [13]   homed (0 or 1)
 */
#define UART_GRIPPER_STATUS_STATE_INDEX UART_RESPONSE_HEADER_SIZE
#define UART_GRIPPER_STATUS_MOTION_ID_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 1U)
#define UART_GRIPPER_STATUS_MOTION_ID_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 2U)
#define UART_GRIPPER_STATUS_BASE_ANGLE_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 3U)
#define UART_GRIPPER_STATUS_BASE_ANGLE_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 4U)
#define UART_GRIPPER_STATUS_SHOULDER_ANGLE_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 5U)
#define UART_GRIPPER_STATUS_SHOULDER_ANGLE_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 6U)
#define UART_GRIPPER_STATUS_ELBOW_ANGLE_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 7U)
#define UART_GRIPPER_STATUS_ELBOW_ANGLE_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 8U)
#define UART_GRIPPER_STATUS_POSITION_INDEX (UART_RESPONSE_HEADER_SIZE + 9U)
#define UART_GRIPPER_STATUS_HOMED_INDEX (UART_RESPONSE_HEADER_SIZE + 10U)
#define UART_GRIPPER_STATUS_DATA_SIZE 11U
#define UART_GRIPPER_STATUS_PAYLOAD_SIZE (UART_RESPONSE_HEADER_SIZE + UART_GRIPPER_STATUS_DATA_SIZE)

typedef enum {
    UART_GRIPPER_MOTION_ARM = 0x01U,
    UART_GRIPPER_MOTION_GRIPPER = 0x02U,
    UART_GRIPPER_MOTION_HOME = 0x03U
} uart_gripper_motion_type_t;

typedef enum {
    UART_GRIPPER_EVENT_MOTION_COMPLETE = 0x01U,
    UART_GRIPPER_EVENT_FAULT = 0x02U
} uart_gripper_event_t;

/*
 * MOTION_COMPLETE event:
 *   [0] event_id
 *   [1] motion_id_low
 *   [2] motion_id_high
 *   [3] motion_type
 */
#define UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX UART_EVENT_HEADER_SIZE
#define UART_GRIPPER_EVENT_MOTION_ID_HIGH_INDEX (UART_EVENT_HEADER_SIZE + 1U)
#define UART_GRIPPER_EVENT_MOTION_TYPE_INDEX (UART_EVENT_HEADER_SIZE + 2U)
#define UART_GRIPPER_MOTION_EVENT_DATA_SIZE 3U
#define UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_GRIPPER_MOTION_EVENT_DATA_SIZE)

/* FAULT adds one error byte after the common motion event data. */
#define UART_GRIPPER_FAULT_EVENT_ERROR_INDEX (UART_EVENT_HEADER_SIZE + 3U)
#define UART_GRIPPER_FAULT_EVENT_DATA_SIZE 4U
#define UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_GRIPPER_FAULT_EVENT_DATA_SIZE)

static inline uint16_t uart_gripper_read_u16(const uint8_t* payload, uint32_t low_index, uint32_t high_index) {
    if (payload == NULL) {
        return 0U;
    }

    return (uint16_t)((uint16_t)payload[low_index] | ((uint16_t)payload[high_index] << 8U));
}

static inline uint16_t uart_gripper_move_motion_id(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX,
                                 UART_GRIPPER_MOVE_MOTION_ID_HIGH_INDEX);
}

static inline uint16_t uart_gripper_move_base_angle(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX,
                                 UART_GRIPPER_MOVE_BASE_ANGLE_HIGH_INDEX);
}

static inline uint16_t uart_gripper_move_shoulder_angle(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX,
                                 UART_GRIPPER_MOVE_SHOULDER_ANGLE_HIGH_INDEX);
}

static inline uint16_t uart_gripper_move_elbow_angle(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX,
                                 UART_GRIPPER_MOVE_ELBOW_ANGLE_HIGH_INDEX);
}

static inline uint16_t uart_gripper_move_duration_ms(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_MOVE_DURATION_LOW_INDEX, UART_GRIPPER_MOVE_DURATION_HIGH_INDEX);
}

static inline uint16_t uart_gripper_set_motion_id(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_SET_MOTION_ID_LOW_INDEX, UART_GRIPPER_SET_MOTION_ID_HIGH_INDEX);
}

static inline uint16_t uart_gripper_set_duration_ms(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_SET_DURATION_LOW_INDEX, UART_GRIPPER_SET_DURATION_HIGH_INDEX);
}

static inline uint16_t uart_gripper_home_motion_id(const uint8_t* payload) {
    return uart_gripper_read_u16(payload, UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX,
                                 UART_GRIPPER_HOME_MOTION_ID_HIGH_INDEX);
}

static inline uint8_t uart_gripper_command_is_valid(uint32_t command) {
    switch (command) {
        case UART_CMD_GRIPPER_MOVE_ARM:
        case UART_CMD_GRIPPER_SET_GRIPPER:
        case UART_CMD_GRIPPER_HOME:
        case UART_CMD_GRIPPER_STOP:
        case UART_CMD_GRIPPER_GET_STATUS:
        case UART_CMD_GRIPPER_RESET:
            return 1U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_GRIPPER_COMMAND(command) uart_gripper_command_is_valid((uint32_t)(command))

static inline uint8_t uart_gripper_motion_id_is_valid(uint32_t motion_id) {
    return (motion_id >= UART_GRIPPER_MOTION_ID_MIN && motion_id <= UART_GRIPPER_MOTION_ID_MAX) ? 1U : 0U;
}

static inline uint8_t uart_gripper_angle_is_valid(uint32_t angle_deci_deg) {
    return (angle_deci_deg <= UART_GRIPPER_ANGLE_DECI_DEG_MAX) ? 1U : 0U;
}

static inline uint8_t uart_gripper_duration_is_valid(uint32_t duration_ms) {
    return (duration_ms >= UART_GRIPPER_DURATION_MS_MIN && duration_ms <= UART_GRIPPER_DURATION_MS_MAX) ? 1U : 0U;
}

static inline uint8_t uart_gripper_position_is_valid(uint32_t position_percent) {
    return (position_percent <= UART_GRIPPER_POSITION_MAX) ? 1U : 0U;
}

static inline uint8_t uart_gripper_state_is_valid(uint32_t state) {
    return (state <= UART_GRIPPER_STATE_EMERGENCY_STOP) ? 1U : 0U;
}

static inline uint8_t uart_gripper_motion_type_is_valid(uint32_t motion_type) {
    return (motion_type >= UART_GRIPPER_MOTION_ARM && motion_type <= UART_GRIPPER_MOTION_HOME) ? 1U : 0U;
}

static inline uint8_t uart_gripper_fault_error_is_valid(uint32_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
        case UART_ERROR_SERVO:
        case UART_ERROR_EMERGENCY_STOP:
        case UART_ERROR_INTERNAL:
            return 1U;

        default:
            return 0U;
    }
}

static inline uint8_t uart_gripper_payload_is_valid(uint32_t command, const uint8_t* payload, uint32_t length) {
    uint16_t motion_id;
    uint16_t duration_ms;

    if (uart_gripper_command_is_valid(command) == 0U || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    switch (command) {
        case UART_CMD_GRIPPER_STOP:
            return (length == UART_GRIPPER_STOP_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_GRIPPER_GET_STATUS:
            return (length == UART_GRIPPER_GET_STATUS_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_GRIPPER_RESET:
            return (length == UART_GRIPPER_RESET_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_GRIPPER_HOME:
            if (length != UART_GRIPPER_HOME_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }
            return uart_gripper_motion_id_is_valid(uart_gripper_home_motion_id(payload));

        case UART_CMD_GRIPPER_MOVE_ARM:
            if (length != UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            motion_id = uart_gripper_move_motion_id(payload);
            duration_ms = uart_gripper_move_duration_ms(payload);
            return (uart_gripper_motion_id_is_valid(motion_id) != 0U &&
                    uart_gripper_angle_is_valid(uart_gripper_move_base_angle(payload)) != 0U &&
                    uart_gripper_angle_is_valid(uart_gripper_move_shoulder_angle(payload)) != 0U &&
                    uart_gripper_angle_is_valid(uart_gripper_move_elbow_angle(payload)) != 0U &&
                    uart_gripper_duration_is_valid(duration_ms) != 0U)
                       ? 1U
                       : 0U;

        case UART_CMD_GRIPPER_SET_GRIPPER:
            if (length != UART_GRIPPER_SET_GRIPPER_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            motion_id = uart_gripper_set_motion_id(payload);
            duration_ms = uart_gripper_set_duration_ms(payload);
            return (uart_gripper_motion_id_is_valid(motion_id) != 0U &&
                    uart_gripper_position_is_valid(payload[UART_GRIPPER_SET_POSITION_INDEX]) != 0U &&
                    uart_gripper_duration_is_valid(duration_ms) != 0U)
                       ? 1U
                       : 0U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_GRIPPER_PAYLOAD(command, payload, length) \
    uart_gripper_payload_is_valid((uint32_t)(command), (const uint8_t*)(payload), (uint32_t)(length))

static inline uint8_t uart_gripper_event_payload_is_valid(const uint8_t* payload, uint32_t length) {
    uint16_t motion_id;

    if (payload == NULL || length < UART_EVENT_HEADER_SIZE || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    switch (payload[UART_EVENT_ID_INDEX]) {
        case UART_GRIPPER_EVENT_MOTION_COMPLETE:
            if (length != UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }
            break;

        case UART_GRIPPER_EVENT_FAULT:
            if (length != UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE ||
                uart_gripper_fault_error_is_valid(payload[UART_GRIPPER_FAULT_EVENT_ERROR_INDEX]) == 0U) {
                return 0U;
            }
            break;

        default:
            return 0U;
    }

    motion_id = uart_gripper_read_u16(payload, UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX,
                                      UART_GRIPPER_EVENT_MOTION_ID_HIGH_INDEX);
    return (uart_gripper_motion_id_is_valid(motion_id) != 0U &&
            uart_gripper_motion_type_is_valid(payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX]) != 0U)
               ? 1U
               : 0U;
}

#define UART_IS_VALID_GRIPPER_EVENT_PAYLOAD(payload, length) \
    uart_gripper_event_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_GRIPPER_COMMANDS_H */
