#ifndef LOGISTICS_CONTRACTS_UART_SORTING_COMMANDS_H
#define LOGISTICS_CONTRACTS_UART_SORTING_COMMANDS_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * sorting-controller CMD
 * ============================================================================
 *
 * Raspberry Pi -> sorting-controller STM32 명령이다.
 *
 * 분류 게이트는 1개이므로 gate_id를 전송하지 않는다. Raspberry Pi는
 * 목적지 ID만 전송하고, 목적지별 실제 게이트 각도는 STM32 설정에서
 * 관리한다.
 *
 * 목적지 초음파 센서 감지는 STM32 내부 동작이다. 현재 목적지와 같은
 * 센서가 물품을 감지하면 게이트를 자동으로 Home 위치로 복귀시킨다.
 */
typedef enum {
    UART_CMD_SORTING_ROUTE_ITEM = 0x30U,
    UART_CMD_SORTING_GET_STATUS = 0x31U,
    UART_CMD_SORTING_CANCEL = 0x32U,
    UART_CMD_SORTING_RESET = 0x33U

} uart_sorting_command_t;

/*
 * ============================================================================
 * 분류 목적지와 초음파 센서 ID
 * ============================================================================
 *
 * 각 목적지에는 초음파 센서가 1개씩 있으며 목적지 ID와 센서 ID는
 * 1:1로 대응한다.
 */
typedef enum {
    UART_SORTING_DESTINATION_NONE = 0x00U,
    UART_SORTING_DESTINATION_1 = 0x01U,
    UART_SORTING_DESTINATION_2 = 0x02U,
    UART_SORTING_DESTINATION_3 = 0x03U

} uart_sorting_destination_t;

#define UART_SORTING_DESTINATION_MIN UART_SORTING_DESTINATION_1
#define UART_SORTING_DESTINATION_MAX UART_SORTING_DESTINATION_3

typedef enum {
    UART_SORTING_SENSOR_ID_1 = UART_SORTING_DESTINATION_1,
    UART_SORTING_SENSOR_ID_2 = UART_SORTING_DESTINATION_2,
    UART_SORTING_SENSOR_ID_3 = UART_SORTING_DESTINATION_3

} uart_sorting_sensor_id_t;

#define UART_SORTING_SENSOR_ID_MIN UART_SORTING_SENSOR_ID_1
#define UART_SORTING_SENSOR_ID_MAX UART_SORTING_SENSOR_ID_3

/*
 * ============================================================================
 * SORTING_ROUTE_ITEM Payload
 * ============================================================================
 *
 *   [0] cycle_id_low
 *   [1] cycle_id_high
 *   [2] destination_id
 *
 * cycle_id는 little-endian uint16_t이다. 게이트가 Home으로 복귀하여
 * 분류 완료 이벤트를 송신할 때까지 같은 cycle_id를 유지한다.
 */
#define UART_SORTING_ROUTE_CYCLE_ID_LOW_INDEX 0U
#define UART_SORTING_ROUTE_CYCLE_ID_HIGH_INDEX 1U
#define UART_SORTING_ROUTE_DESTINATION_INDEX 2U
#define UART_SORTING_ROUTE_PAYLOAD_SIZE 3U

/*
 * SORTING_GET_STATUS
 *
 * 게이트가 1개이므로 Payload는 없다.
 */
#define UART_SORTING_GET_STATUS_PAYLOAD_SIZE 0U

/*
 * SORTING_CANCEL Payload
 *
 *   [0] cycle_id_low
 *   [1] cycle_id_high
 *
 * 현재 처리 중인 cycle_id와 일치할 때만 취소하고 Home으로 복귀한다.
 */
#define UART_SORTING_CANCEL_CYCLE_ID_LOW_INDEX 0U
#define UART_SORTING_CANCEL_CYCLE_ID_HIGH_INDEX 1U
#define UART_SORTING_CANCEL_PAYLOAD_SIZE 2U

/*
 * SORTING_RESET
 *
 * Payload는 없다. 일반 분류 오류와 현재 cycle을 초기화하고 게이트를
 * Home으로 복귀시킨다. 비상정지 latch는 해제하지 않는다.
 */
#define UART_SORTING_RESET_PAYLOAD_SIZE 0U

/*
 * ============================================================================
 * 게이트 상태와 SORTING_GET_STATUS 응답
 * ============================================================================
 */
typedef enum {
    UART_SORTING_GATE_HOME = 0x00U,
    UART_SORTING_GATE_MOVING = 0x01U,
    UART_SORTING_GATE_WAIT_ITEM = 0x02U,
    UART_SORTING_GATE_RETURNING = 0x03U,
    UART_SORTING_GATE_FAULT = 0x04U

} uart_sorting_gate_state_t;

/*
 * UART_CMD_RESPONSE의 공통 헤더 뒤에 다음 데이터를 붙인다.
 *
 *   [0] status
 *   [1] original_command
 *   [2] error_code
 *   [3] gate_state
 *   [4] cycle_id_low
 *   [5] cycle_id_high
 *   [6] destination_id
 *
 * 활성 cycle이 없으면 cycle_id는 0, destination_id는
 * UART_SORTING_DESTINATION_NONE으로 전송한다.
 */
#define UART_SORTING_STATUS_GATE_STATE_INDEX UART_RESPONSE_HEADER_SIZE
#define UART_SORTING_STATUS_CYCLE_ID_LOW_INDEX (UART_RESPONSE_HEADER_SIZE + 1U)
#define UART_SORTING_STATUS_CYCLE_ID_HIGH_INDEX (UART_RESPONSE_HEADER_SIZE + 2U)
#define UART_SORTING_STATUS_DESTINATION_INDEX (UART_RESPONSE_HEADER_SIZE + 3U)
#define UART_SORTING_STATUS_DATA_SIZE 4U
#define UART_SORTING_STATUS_PAYLOAD_SIZE (UART_RESPONSE_HEADER_SIZE + UART_SORTING_STATUS_DATA_SIZE)

/*
 * ============================================================================
 * 분류 이벤트
 * ============================================================================
 *
 * ITEM_DETECTED:
 *   현재 목적지의 센서가 물품을 감지하여 Home 복귀를 시작했음
 *
 * CYCLE_COMPLETE:
 *   게이트가 Home 위치로 복귀하여 분류 cycle이 완료되었음
 *
 * UNEXPECTED_SENSOR:
 *   현재 목적지와 다른 위치의 센서가 물품을 감지했음
 *
 * FAULT:
 *   센서 timeout, 센서 오류 또는 서보 오류가 발생했음
 */
typedef enum {
    UART_SORTING_EVENT_ITEM_DETECTED = 0x01U,
    UART_SORTING_EVENT_CYCLE_COMPLETE = 0x02U,
    UART_SORTING_EVENT_UNEXPECTED_SENSOR = 0x03U,
    UART_SORTING_EVENT_FAULT = 0x04U

} uart_sorting_event_t;

/* ITEM_DETECTED / CYCLE_COMPLETE */
#define UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX UART_EVENT_HEADER_SIZE
#define UART_SORTING_EVENT_CYCLE_ID_HIGH_INDEX (UART_EVENT_HEADER_SIZE + 1U)
#define UART_SORTING_EVENT_DESTINATION_INDEX (UART_EVENT_HEADER_SIZE + 2U)
#define UART_SORTING_CYCLE_EVENT_DATA_SIZE 3U
#define UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_SORTING_CYCLE_EVENT_DATA_SIZE)

/* UNEXPECTED_SENSOR */
#define UART_SORTING_UNEXPECTED_EXPECTED_DESTINATION_INDEX (UART_EVENT_HEADER_SIZE + 2U)
#define UART_SORTING_UNEXPECTED_DETECTED_SENSOR_INDEX (UART_EVENT_HEADER_SIZE + 3U)
#define UART_SORTING_UNEXPECTED_EVENT_DATA_SIZE 4U
#define UART_SORTING_UNEXPECTED_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_SORTING_UNEXPECTED_EVENT_DATA_SIZE)

/* FAULT */
#define UART_SORTING_FAULT_DESTINATION_INDEX (UART_EVENT_HEADER_SIZE + 2U)
#define UART_SORTING_FAULT_ERROR_INDEX (UART_EVENT_HEADER_SIZE + 3U)
#define UART_SORTING_FAULT_EVENT_DATA_SIZE 4U
#define UART_SORTING_FAULT_EVENT_PAYLOAD_SIZE (UART_EVENT_HEADER_SIZE + UART_SORTING_FAULT_EVENT_DATA_SIZE)

/*
 * ============================================================================
 * sorting-controller 검증 함수
 * ============================================================================
 */
static inline uint8_t uart_sorting_command_is_valid(uint32_t command) {
    switch (command) {
        case UART_CMD_SORTING_ROUTE_ITEM:
        case UART_CMD_SORTING_GET_STATUS:
        case UART_CMD_SORTING_CANCEL:
        case UART_CMD_SORTING_RESET:
            return 1U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_SORTING_COMMAND(command) uart_sorting_command_is_valid((uint32_t)(command))

static inline uint8_t uart_sorting_destination_is_valid(uint32_t destination_id) {
    return (destination_id >= UART_SORTING_DESTINATION_MIN && destination_id <= UART_SORTING_DESTINATION_MAX) ? 1U : 0U;
}

static inline uint8_t uart_sorting_status_destination_is_valid(uint32_t destination_id) {
    if (destination_id == UART_SORTING_DESTINATION_NONE) {
        return 1U;
    }

    return uart_sorting_destination_is_valid(destination_id);
}

static inline uint8_t uart_sorting_sensor_id_is_valid(uint32_t sensor_id) {
    return (sensor_id >= UART_SORTING_SENSOR_ID_MIN && sensor_id <= UART_SORTING_SENSOR_ID_MAX) ? 1U : 0U;
}

static inline uint8_t uart_sorting_sensor_matches_destination(uint32_t sensor_id, uint32_t destination_id) {
    if (uart_sorting_sensor_id_is_valid(sensor_id) == 0U || uart_sorting_destination_is_valid(destination_id) == 0U) {
        return 0U;
    }

    return (sensor_id == destination_id) ? 1U : 0U;
}

static inline uint8_t uart_sorting_gate_state_is_valid(uint32_t state) {
    return (state <= UART_SORTING_GATE_FAULT) ? 1U : 0U;
}

static inline uint8_t uart_sorting_event_is_valid(uint32_t event_id) {
    switch (event_id) {
        case UART_SORTING_EVENT_ITEM_DETECTED:
        case UART_SORTING_EVENT_CYCLE_COMPLETE:
        case UART_SORTING_EVENT_UNEXPECTED_SENSOR:
        case UART_SORTING_EVENT_FAULT:
            return 1U;

        default:
            return 0U;
    }
}

static inline uint8_t uart_sorting_fault_error_is_valid(uint32_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
        case UART_ERROR_SENSOR:
        case UART_ERROR_SERVO:
        case UART_ERROR_EMERGENCY_STOP:
        case UART_ERROR_INTERNAL:
            return 1U;

        default:
            return 0U;
    }
}

static inline uint16_t uart_sorting_route_cycle_id(const uint8_t* payload) {
    if (payload == NULL) {
        return 0U;
    }

    return (uint16_t)((uint16_t)payload[UART_SORTING_ROUTE_CYCLE_ID_LOW_INDEX] |
                      ((uint16_t)payload[UART_SORTING_ROUTE_CYCLE_ID_HIGH_INDEX] << 8U));
}

static inline uint16_t uart_sorting_cancel_cycle_id(const uint8_t* payload) {
    if (payload == NULL) {
        return 0U;
    }

    return (uint16_t)((uint16_t)payload[UART_SORTING_CANCEL_CYCLE_ID_LOW_INDEX] |
                      ((uint16_t)payload[UART_SORTING_CANCEL_CYCLE_ID_HIGH_INDEX] << 8U));
}

/* CMD별 Payload 길이와 값 범위를 검증한다. */
static inline uint8_t uart_sorting_payload_is_valid(uint32_t command, const uint8_t* payload, uint32_t length) {
    if (uart_sorting_command_is_valid(command) == 0U || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    switch (command) {
        case UART_CMD_SORTING_GET_STATUS:
            return (length == UART_SORTING_GET_STATUS_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_SORTING_RESET:
            return (length == UART_SORTING_RESET_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_SORTING_ROUTE_ITEM:
            if (length != UART_SORTING_ROUTE_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            return uart_sorting_destination_is_valid(payload[UART_SORTING_ROUTE_DESTINATION_INDEX]);

        case UART_CMD_SORTING_CANCEL:
            return (length == UART_SORTING_CANCEL_PAYLOAD_SIZE && payload != NULL) ? 1U : 0U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_SORTING_PAYLOAD(command, payload, length) \
    uart_sorting_payload_is_valid((uint32_t)(command), (const uint8_t*)(payload), (uint32_t)(length))

/* UART_CMD_EVENT의 분류 이벤트 ID, 길이와 값 범위를 검증한다. */
static inline uint8_t uart_sorting_event_payload_is_valid(const uint8_t* payload, uint32_t length) {
    uint32_t event_id;

    if (payload == NULL || length < UART_EVENT_HEADER_SIZE || length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    event_id = payload[UART_EVENT_ID_INDEX];
    if (uart_sorting_event_is_valid(event_id) == 0U) {
        return 0U;
    }

    switch (event_id) {
        case UART_SORTING_EVENT_ITEM_DETECTED:
        case UART_SORTING_EVENT_CYCLE_COMPLETE:
            if (length != UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }

            return uart_sorting_destination_is_valid(payload[UART_SORTING_EVENT_DESTINATION_INDEX]);

        case UART_SORTING_EVENT_UNEXPECTED_SENSOR:
            if (length != UART_SORTING_UNEXPECTED_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }

            if (uart_sorting_destination_is_valid(payload[UART_SORTING_UNEXPECTED_EXPECTED_DESTINATION_INDEX]) == 0U ||
                uart_sorting_sensor_id_is_valid(payload[UART_SORTING_UNEXPECTED_DETECTED_SENSOR_INDEX]) == 0U) {
                return 0U;
            }

            return uart_sorting_sensor_matches_destination(
                       payload[UART_SORTING_UNEXPECTED_DETECTED_SENSOR_INDEX],
                       payload[UART_SORTING_UNEXPECTED_EXPECTED_DESTINATION_INDEX]) == 0U
                       ? 1U
                       : 0U;

        case UART_SORTING_EVENT_FAULT:
            if (length != UART_SORTING_FAULT_EVENT_PAYLOAD_SIZE) {
                return 0U;
            }

            if (uart_sorting_status_destination_is_valid(payload[UART_SORTING_FAULT_DESTINATION_INDEX]) == 0U) {
                return 0U;
            }

            return uart_sorting_fault_error_is_valid(payload[UART_SORTING_FAULT_ERROR_INDEX]);

        default:
            return 0U;
    }
}

#define UART_IS_VALID_SORTING_EVENT_PAYLOAD(payload, length) \
    uart_sorting_event_payload_is_valid((const uint8_t*)(payload), (uint32_t)(length))

/* sorting-controller의 SENSOR_STATUS 값 범위를 검증한다. */
static inline uint8_t uart_sorting_sensor_status_is_valid(const uint8_t* payload, uint32_t length) {
    if (length != UART_SENSOR_STATUS_PAYLOAD_SIZE || payload == NULL) {
        return 0U;
    }

    if (uart_sorting_sensor_id_is_valid(payload[UART_SENSOR_ID_INDEX]) == 0U) {
        return 0U;
    }

    return (payload[UART_SENSOR_STATE_INDEX] <= UART_SENSOR_FAULT) ? 1U : 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_SORTING_COMMANDS_H */
