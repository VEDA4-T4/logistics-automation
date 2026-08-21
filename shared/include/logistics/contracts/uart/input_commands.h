#ifndef LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H
#define LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H

#include <stddef.h>
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
 * 전용 UART 채널의 Raspberry Pi -> input-controller STM32 명령이다.
 * 이 계약은 투입 컨베이어 벨트 1의 장치 제어만 담당하므로 conveyor_id를
 * 전송하지 않는다. 바코드, 상품 종류 및 분류 판단은 서버와 Raspberry Pi에서
 * 완료하고 STM32에는 최종 장치 제어 명령만 전송한다.
 */
typedef enum {
    UART_CMD_INPUT_CONVEYOR_START = 0x10U,
    UART_CMD_INPUT_CONVEYOR_STOP = 0x11U,
    UART_CMD_INPUT_CONVEYOR_SET_SPEED = 0x12U,
    UART_CMD_INPUT_CONVEYOR_GET_STATUS = 0x13U,
    UART_CMD_INPUT_CONTROL_RESET = 0x14U

} uart_input_command_t;

/*
 * 투입 초음파 센서 ID
 *
 * SENSOR_STATUS는 STM32 -> Raspberry Pi 방향의 상태 보고이며,
 * 투입 센서는 ID 1을 사용하고 거리값은 cm 단위로 전송한다.
 * Raspberry Pi -> STM32 수신 명령이 아니다.
 */
typedef enum {
    UART_INPUT_SENSOR_ID_1 = 0x01U

} uart_input_sensor_id_t;

#define UART_INPUT_SENSOR_ID_MIN UART_INPUT_SENSOR_ID_1
#define UART_INPUT_SENSOR_ID_MAX UART_INPUT_SENSOR_ID_1

/*
 * ============================================================================
 * 컨베이어 제어 Payload
 * ============================================================================
 */

/* INPUT_CONVEYOR_START / INPUT_CONVEYOR_STOP / INPUT_CONVEYOR_GET_STATUS */
#define UART_INPUT_CONVEYOR_COMMAND_PAYLOAD_SIZE 0U

/* INPUT_CONVEYOR_SET_SPEED */
#define UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX 0U
#define UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE 1U
#define UART_INPUT_CONVEYOR_SPEED_MIN 0U
#define UART_INPUT_CONVEYOR_SPEED_MAX 100U

/*
 * CONTROL_RESET
 *
 * Payload는 없다. 컨베이어를 정지하고 일반 제어 오류를 초기화하며 기존
 * 속도 설정은 유지한다. 비상정지 latch를 해제하거나 STM32를 재부팅하지 않는다.
 */
#define UART_INPUT_CONTROL_RESET_PAYLOAD_SIZE 0U

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
 *   [3] conveyor_state
 *   [4] speed
 */
typedef enum {
    UART_INPUT_CONVEYOR_STOPPED = 0x00U,
    UART_INPUT_CONVEYOR_RUNNING = 0x01U,
    UART_INPUT_CONVEYOR_FAULT = 0x02U

} uart_input_conveyor_state_t;

#define UART_INPUT_CONVEYOR_STATUS_STATE_INDEX UART_RESPONSE_HEADER_SIZE
#define UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX (UART_RESPONSE_HEADER_SIZE + 1U)
#define UART_INPUT_CONVEYOR_STATUS_DATA_SIZE 2U
#define UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE (UART_RESPONSE_HEADER_SIZE + UART_INPUT_CONVEYOR_STATUS_DATA_SIZE)

/*
 * ============================================================================
 * input-controller 검증 함수
 * ============================================================================
 */
static inline uint8_t uart_input_command_is_valid(uint32_t command) {
    switch (command) {
        case UART_CMD_INPUT_CONVEYOR_START:
        case UART_CMD_INPUT_CONVEYOR_STOP:
        case UART_CMD_INPUT_CONVEYOR_SET_SPEED:
        case UART_CMD_INPUT_CONVEYOR_GET_STATUS:
        case UART_CMD_INPUT_CONTROL_RESET:
            return 1U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_INPUT_COMMAND(command) uart_input_command_is_valid((uint32_t)(command))

static inline uint8_t uart_input_sensor_id_is_valid(uint32_t sensor_id) {
    return (sensor_id == UART_INPUT_SENSOR_ID_1) ? 1U : 0U;
}

static inline uint8_t uart_input_conveyor_state_is_valid(uint32_t state) {
    return (state <= UART_INPUT_CONVEYOR_FAULT) ? 1U : 0U;
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
        case UART_CMD_INPUT_CONTROL_RESET:
            return (length == UART_INPUT_CONTROL_RESET_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_INPUT_CONVEYOR_START:
        case UART_CMD_INPUT_CONVEYOR_STOP:
        case UART_CMD_INPUT_CONVEYOR_GET_STATUS:
            return (length == UART_INPUT_CONVEYOR_COMMAND_PAYLOAD_SIZE) ? 1U : 0U;

        case UART_CMD_INPUT_CONVEYOR_SET_SPEED:
            if (length != UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE || payload == NULL) {
                return 0U;
            }

            return (payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] <= UART_INPUT_CONVEYOR_SPEED_MAX) ? 1U : 0U;

        default:
            return 0U;
    }
}

#define UART_IS_VALID_INPUT_PAYLOAD(command, payload, length) \
    uart_input_payload_is_valid((uint32_t)(command), (const uint8_t*)(payload), (uint32_t)(length))

/* input-controller가 송신하는 SENSOR_STATUS 값 범위를 검증한다. */
static inline uint8_t uart_input_sensor_status_is_valid(const uint8_t* payload, uint32_t length) {
    if (length != UART_SENSOR_STATUS_PAYLOAD_SIZE || payload == NULL) {
        return 0U;
    }

    if (uart_input_sensor_id_is_valid(payload[UART_SENSOR_ID_INDEX]) == 0U) {
        return 0U;
    }

    return (payload[UART_SENSOR_STATE_INDEX] <= UART_SENSOR_FAULT) ? 1U : 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* LOGISTICS_CONTRACTS_UART_INPUT_COMMANDS_H */
