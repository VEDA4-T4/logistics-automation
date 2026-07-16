#ifndef LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H
#define LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * input-controller CMD
 * ============================================================================
 *
 * Raspberry Pi -> input-controller STM32 명령이다.
 * 컨베이어 1번과 2번은 서로 다른 CMD를 사용하지 않고
 * Payload의 conveyor_id로 구분한다.
 *
 * 초음파 센서에 의한 자동 제어는 SensorTask 내부 동작이므로
 * 별도 수신 CMD로 정의하지 않는다. 센서 측정 결과는 공통
 * UART_CMD_SENSOR_STATUS 또는 UART_CMD_EVENT로 전송한다.
 */
typedef enum {
    UART_CMD_CONVEYOR_START = 0x10U,
    UART_CMD_CONVEYOR_STOP = 0x11U,
    UART_CMD_CONVEYOR_SET_SPEED = 0x12U,
    UART_CMD_CONVEYOR_GET_STATUS = 0x13U,
    UART_CMD_CONTROL_RESET = 0x14U,
    UART_CMD_VISION_RESULT = 0x15U

} uart_input_command_t;

/*
 * ============================================================================
 * 컨베이어와 초음파 센서 ID
 * ============================================================================
 */
typedef enum {
    UART_CONVEYOR_ID_1 = 0x01U,
    UART_CONVEYOR_ID_2 = 0x02U

} uart_conveyor_id_t;

#define UART_CONVEYOR_ID_MIN UART_CONVEYOR_ID_1
#define UART_CONVEYOR_ID_MAX UART_CONVEYOR_ID_2

typedef enum {
    UART_ULTRASONIC_SENSOR_ID_1 = 0x01U,
    UART_ULTRASONIC_SENSOR_ID_2 = 0x02U,
    UART_ULTRASONIC_SENSOR_ID_3 = 0x03U,
    UART_ULTRASONIC_SENSOR_ID_4 = 0x04U

} uart_ultrasonic_sensor_id_t;

#define UART_ULTRASONIC_SENSOR_ID_MIN UART_ULTRASONIC_SENSOR_ID_1
#define UART_ULTRASONIC_SENSOR_ID_MAX UART_ULTRASONIC_SENSOR_ID_4

/*
 * ============================================================================
 * 컨베이어 제어 Payload
 * ============================================================================
 */

/* CONVEYOR_START / CONVEYOR_STOP / CONVEYOR_GET_STATUS */
#define UART_CONVEYOR_ID_INDEX 0U
#define UART_CONVEYOR_COMMAND_PAYLOAD_SIZE 1U

/* CONVEYOR_SET_SPEED */
#define UART_CONVEYOR_SPEED_ID_INDEX 0U
#define UART_CONVEYOR_SPEED_VALUE_INDEX 1U
#define UART_CONVEYOR_SET_SPEED_PAYLOAD_SIZE 2U
#define UART_CONVEYOR_SPEED_MIN 0U
#define UART_CONVEYOR_SPEED_MAX 100U

/*
 * CONTROL_RESET
 *
 * Payload는 없다. 현재 제어 cycle과 일반 제어 오류만 초기화한다.
 * 비상정지 latch를 해제하거나 STM32를 재부팅하지 않는다.
 */
#define UART_CONTROL_RESET_PAYLOAD_SIZE 0U

/*
 * ============================================================================
 * VISION_RESULT Payload
 * ============================================================================
 *
 * Raspberry Pi -> input-controller STM32
 *
 *   [0] cycle_id_low
 *   [1] cycle_id_high
 *   [2] detected       (0: 미검출, 1: 검출)
 *   [3] product_type   (프로젝트에서 정의하는 상품 분류 ID)
 *   [4] confidence     (0~100)
 *
 * cycle_id는 little-endian uint16_t이다. UART Frame의 sequence는
 * 요청/응답 연결 번호이므로 공정 cycle_id 대신 사용하지 않는다.
 */
#define UART_VISION_CYCLE_ID_LOW_INDEX 0U
#define UART_VISION_CYCLE_ID_HIGH_INDEX 1U
#define UART_VISION_DETECTED_INDEX 2U
#define UART_VISION_PRODUCT_TYPE_INDEX 3U
#define UART_VISION_CONFIDENCE_INDEX 4U
#define UART_VISION_RESULT_PAYLOAD_SIZE 5U

#define UART_VISION_NOT_DETECTED 0U
#define UART_VISION_DETECTED 1U
#define UART_VISION_CONFIDENCE_MIN 0U
#define UART_VISION_CONFIDENCE_MAX 100U

/*
 * ============================================================================
 * CONVEYOR_GET_STATUS 응답 데이터
 * ============================================================================
 *
 * UART_CMD_RESPONSE의 공통 헤더 뒤에 다음 데이터를 붙인다.
 *
 *   [0] status
 *   [1] original_command
 *   [2] error_code
 *   [3] conveyor_id
 *   [4] conveyor_state
 *   [5] speed
 */
typedef enum {
    UART_CONVEYOR_STOPPED = 0x00U,
    UART_CONVEYOR_RUNNING = 0x01U,
    UART_CONVEYOR_FAULT = 0x02U

} uart_conveyor_state_t;

#define UART_CONVEYOR_STATUS_ID_INDEX UART_RESPONSE_HEADER_SIZE
#define UART_CONVEYOR_STATUS_STATE_INDEX (UART_RESPONSE_HEADER_SIZE + 1U)
#define UART_CONVEYOR_STATUS_SPEED_INDEX (UART_RESPONSE_HEADER_SIZE + 2U)
#define UART_CONVEYOR_STATUS_DATA_SIZE 3U
#define UART_CONVEYOR_STATUS_PAYLOAD_SIZE (UART_RESPONSE_HEADER_SIZE + UART_CONVEYOR_STATUS_DATA_SIZE)

/*
 * ============================================================================
 * input-controller 검증 함수
 * ============================================================================
 */
static inline uint8_t uart_input_command_is_valid(uint32_t command) {
    switch (command) {
        case UART_CMD_CONVEYOR_START:
        case UART_CMD_CONVEYOR_STOP:
        case UART_CMD_CONVEYOR_SET_SPEED:
        case UART_CMD_CONVEYOR_GET_STATUS:
        case UART_CMD_CONTROL_RESET:
        case UART_CMD_VISION_RESULT:
            return 1U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_INPUT_COMMAND(command) uart_input_command_is_valid((uint32_t)(command))

static inline uint8_t uart_conveyor_id_is_valid(uint32_t conveyor_id) {
    return (conveyor_id >= UART_CONVEYOR_ID_MIN && conveyor_id <= UART_CONVEYOR_ID_MAX) ? 1U : 0U;
}

static inline uint8_t uart_ultrasonic_sensor_id_is_valid(uint32_t sensor_id) {
    return (sensor_id >= UART_ULTRASONIC_SENSOR_ID_MIN && sensor_id <= UART_ULTRASONIC_SENSOR_ID_MAX) ? 1U : 0U;
}

static inline uint8_t uart_conveyor_state_is_valid(uint32_t state) {
    return (state <= UART_CONVEYOR_FAULT) ? 1U : 0U;
}

static inline uint16_t uart_vision_cycle_id(const uint8_t* payload) {
    if (payload == NULL) {
        return 0U;
    }

    return (uint16_t)((uint16_t)payload[UART_VISION_CYCLE_ID_LOW_INDEX] |
                      ((uint16_t)payload[UART_VISION_CYCLE_ID_HIGH_INDEX] << 8U));
}

/*
 * CMD별 Payload 길이와 값 범위를 검증한다.
 * Frame, CRC와 전체 최대 길이 검증이 끝난 뒤 호출한다.
 */
static inline uint8_t uart_input_payload_is_valid(uint32_t command, const uint8_t* payload, uint32_t length) {
    if (uart_input_command_is_valid(command) == 0U) {
        return 0U;
    }

    if (length > UART_MAX_PAYLOAD_SIZE) {
        return 0U;
    }

    switch (command) {
        case UART_CMD_CONTROL_RESET:
            return (length == UART_CONTROL_RESET_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_CONVEYOR_START:
        case UART_CMD_CONVEYOR_STOP:
        case UART_CMD_CONVEYOR_GET_STATUS:
            if (length != UART_CONVEYOR_COMMAND_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            return uart_conveyor_id_is_valid(payload[UART_CONVEYOR_ID_INDEX]);

        case UART_CMD_CONVEYOR_SET_SPEED:
            if (length != UART_CONVEYOR_SET_SPEED_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            if (uart_conveyor_id_is_valid(payload[UART_CONVEYOR_SPEED_ID_INDEX]) == 0U) {
                return 0U;
            }

            return (payload[UART_CONVEYOR_SPEED_VALUE_INDEX] <= UART_CONVEYOR_SPEED_MAX) ? 1U : 0U;

        case UART_CMD_VISION_RESULT:
            if (length != UART_VISION_RESULT_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            if (payload[UART_VISION_DETECTED_INDEX] > UART_VISION_DETECTED) {
                return 0U;
            }

            return (payload[UART_VISION_CONFIDENCE_INDEX] <= UART_VISION_CONFIDENCE_MAX) ? 1U : 0U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_INPUT_PAYLOAD(command, payload, length) \
    uart_input_payload_is_valid((uint32_t)(command), (const uint8_t*)(payload), (uint32_t)(length))

/* input-controller의 SENSOR_STATUS 값 범위를 검증한다. */
static inline uint8_t uart_input_sensor_status_is_valid(const uint8_t* payload, uint32_t length) {
    if (length != UART_SENSOR_STATUS_PAYLOAD_SIZE || payload == NULL) {
        return 0U;
    }

    if (uart_ultrasonic_sensor_id_is_valid(payload[UART_SENSOR_ID_INDEX]) == 0U) {
        return 0U;
    }

    return (payload[UART_SENSOR_STATE_INDEX] <= UART_SENSOR_FAULT) ? 1U : 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H */
