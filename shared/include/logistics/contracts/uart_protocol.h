#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * 필독 - 각 장치 담당자는 CMD와 CMD검증 및 payload 규격을 정의해야한다.
 * ============================================================================
 * UART 통신 프로토콜 정의
 * ============================================================================
 *
 * Frame 구조
 *
 *   SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16
 *
 * 각 필드의 크기
 *
 *   SOF       : 1바이트
 *   VERSION   : 1바이트
 *   SEQUENCE  : 1바이트
 *   COMMAND   : 1바이트
 *   LENGTH    : 1바이트
 *   PAYLOAD   : 0~128바이트
 *   CRC16     : 2바이트
 *
 * CRC 계산 범위
 *
 *   VERSION부터 PAYLOAD 마지막 바이트까지 계산한다.
 *
 * CRC 전송 순서
 *
 *   CRC Low Byte -> CRC High Byte
 *
 * 다중 바이트 정수
 *
 *   Little-endian 형식을 사용한다.
 */

/*
 * ============================================================================
 * 기본 통신 설정
 * ============================================================================
 */

/* Frame 시작을 나타내는 고정 바이트 */
#define UART_SOF 0xAAU

/* 현재 UART 프로토콜 버전 */
#define UART_PROTOCOL_VERSION 0x01U

/* UART 통신 속도 */
#define UART_BAUDRATE 115200U

/* 하나의 Frame에서 허용하는 최대 Payload 크기 */
#define UART_MAX_PAYLOAD_SIZE 128U

/*
 * Frame 크기 정의
 *
 * Header:
 *   SOF       1바이트
 *   VERSION   1바이트
 *   SEQUENCE  1바이트
 *   COMMAND   1바이트
 *   LENGTH    1바이트
 *
 * CRC:
 *   2바이트
 */
#define UART_FRAME_HEADER_SIZE 5U
#define UART_CRC_SIZE 2U
#define UART_FRAME_OVERHEAD_SIZE 7U

/* 하나의 완성된 Frame이 가질 수 있는 최대 크기 */
#define UART_MAX_FRAME_SIZE (UART_FRAME_OVERHEAD_SIZE + UART_MAX_PAYLOAD_SIZE)

/*
 * ============================================================================
 * 통신 시간 설정
 * ============================================================================
 *
 * 실제 송신 큐와 ACK 재전송 로직에서 사용하는 공통 기준값이다.
 */

/* ACK 응답을 기다리는 최대 시간 */
#define UART_ACK_TIMEOUT_MS 100U

/* 재전송 전 대기 시간 */
#define UART_RETRY_INTERVAL_MS 20U

/* 최대 재전송 횟수 */
#define UART_MAX_RETRY_COUNT 3U

/* 하나의 명령을 처리하는 최대 시간 */
#define UART_COMMAND_TIMEOUT_MS 3000U

/*
 * ============================================================================
 * 장치별 명령 예약 영역
 * ============================================================================
 *
 * 공통 프로토콜은 명령 번호 영역만 예약한다.
 *
 * input-controller (컨베이어/초음파 센서):
 *   0x10 ~ 0x1F
 *
 * gripper-controller (컨베이어 사이 상품 이송, 예약):
 *   0x20 ~ 0x2F
 *
 * sorting-controller (분류장치):
 *   0x30 ~ 0x3F
 *
 * linetracer-controller:
 *   0x40 ~ 0x4F
 *
 * 각 담당자는 해당 영역 안에서 장치 전용 명령을 정의한다.
 *
 * 공통 헤더에서는 장치별 명령의 Payload 의미까지 판단하지 않는다.
 * 장치별 CMD, Payload 길이와 값 범위는 장치별 contract에서 검증한다.
 */
#define UART_CMD_INPUT_MIN 0x10U
#define UART_CMD_INPUT_MAX 0x1FU

#define UART_CMD_GRIPPER_MIN 0x20U
#define UART_CMD_GRIPPER_MAX 0x2FU

/* 기존 개발 브랜치와의 소스 호환성을 위한 임시 별칭. 새 코드는 GRIPPER 이름을 사용한다. */
#define UART_CMD_ROTATION_MIN UART_CMD_GRIPPER_MIN
#define UART_CMD_ROTATION_MAX UART_CMD_GRIPPER_MAX

#define UART_CMD_SORTING_MIN 0x30U
#define UART_CMD_SORTING_MAX 0x3FU

#define UART_CMD_LINETRACER_MIN 0x40U
#define UART_CMD_LINETRACER_MAX 0x4FU

#define UART_CMD_DEVICE_MIN UART_CMD_INPUT_MIN
#define UART_CMD_DEVICE_MAX UART_CMD_LINETRACER_MAX

/*
 * 장치별 명령인지 확인한다.
 */
static inline uint8_t uart_is_device_command(uint32_t command) {
    if (command >= UART_CMD_DEVICE_MIN && command <= UART_CMD_DEVICE_MAX) {
        return 1U;
    }

    return 0U;
}

#define UART_IS_DEVICE_COMMAND(command) uart_is_device_command((uint32_t)(command))

/*
 * ============================================================================
 * CRC16-CCITT-FALSE 설정
 * ============================================================================
 *
 * Polynomial : 0x1021
 * Initial    : 0xFFFF
 * XOR Out    : 0x0000
 * RefIn      : false
 * RefOut     : false
 */
#define UART_CRC16_POLYNOMIAL 0x1021U
#define UART_CRC16_INITIAL_VALUE 0xFFFFU
#define UART_CRC16_XOR_OUT 0x0000U
#define UART_CRC16_REFLECT_INPUT 0U
#define UART_CRC16_REFLECT_OUTPUT 0U

/*
 * ============================================================================
 * 공통 Command ID
 * ============================================================================
 *
 * 모든 STM32와 Raspberry Pi에서 공통으로 사용할 수 있는 명령이다.
 */
typedef enum {
    /* 예약된 명령 없음 */
    UART_CMD_NONE = 0x00U,

    /*
     * 공통 요청 명령
     *
     * PING:
     *   장치 연결 여부 및 통신 상태 확인
     *
     * GET_STATUS:
     *   장치의 현재 상태 요청
     *
     * RESET_DEVICE:
     *   장치 초기화 요청
     */
    UART_CMD_PING = 0x01U,
    UART_CMD_GET_STATUS = 0x02U,
    UART_CMD_RESET_DEVICE = 0x03U,

    /*
     * 상태 및 처리 결과 보고
     *
     * SENSOR_STATUS:
     *   초음파, 포토센서, 라인센서 등의 상태 보고
     *
     * DEVICE_STATUS:
     *   STM32 장치의 현재 상태 보고
     *
     * OPERATION_RESULT:
     *   명령 실행 결과 보고
     */
    UART_CMD_SENSOR_STATUS = 0x50U,
    UART_CMD_DEVICE_STATUS = 0x51U,
    UART_CMD_OPERATION_RESULT = 0x52U,

    /*
     * 응답 명령
     *
     * RESPONSE:
     *   일반적인 명령 응답
     *
     * ACK:
     *   명령 수신 또는 처리 결과 확인
     *
     * EVENT:
     *   비동기 이벤트 보고
     */
    UART_CMD_RESPONSE = 0xE0U,
    UART_CMD_ACK = 0xE1U,
    UART_CMD_EVENT = 0xE2U,

    /*
     * 비상정지 명령
     *
     * 일반 명령 Queue보다 우선 처리해야 한다.
     */
    UART_CMD_EMERGENCY_STOP = 0xF0U

} uart_command_t;

/*
 * ============================================================================
 * 응답 상태 코드
 * ============================================================================
 */
typedef enum {
    /* 명령을 정상적으로 수신함 */
    UART_STATUS_ACK = 0x00U,

    /* 명령 형식 또는 Payload가 잘못됨 */
    UART_STATUS_NACK = 0x01U,

    /* 현재 다른 명령을 처리 중임 */
    UART_STATUS_BUSY = 0x02U,

    /* 명령 실행이 정상적으로 완료됨 */
    UART_STATUS_SUCCESS = 0x03U,

    /* 명령 실행 중 오류가 발생함 */
    UART_STATUS_ERROR = 0x04U

} uart_status_t;

/*
 * ============================================================================
 * 오류 코드
 * ============================================================================
 */
typedef enum {
    UART_ERROR_NONE = 0x00U,
    UART_ERROR_INVALID_VERSION = 0x01U,
    UART_ERROR_INVALID_COMMAND = 0x02U,
    UART_ERROR_INVALID_LENGTH = 0x03U,
    UART_ERROR_CRC_MISMATCH = 0x04U,
    UART_ERROR_SEQUENCE = 0x05U,
    UART_ERROR_TIMEOUT = 0x06U,
    UART_ERROR_BUSY = 0x07U,
    UART_ERROR_SENSOR = 0x08U,
    UART_ERROR_MOTOR = 0x09U,
    UART_ERROR_SERVO = 0x0AU,
    UART_ERROR_EMERGENCY_STOP = 0x0BU,
    UART_ERROR_UNSUPPORTED_COMMAND = 0x0CU,
    UART_ERROR_INVALID_PAYLOAD = 0x0DU,
    UART_ERROR_INTERNAL = 0xFFU

} uart_error_t;

/*
 * ============================================================================
 * 장치 상태 코드
 * ============================================================================
 */
typedef enum {
    /* 초기화되지 않았거나 대기 중 */
    UART_DEVICE_IDLE = 0x00U,

    /* 명령 수신 및 동작 준비 완료 */
    UART_DEVICE_READY = 0x01U,

    /* 현재 동작 중 */
    UART_DEVICE_RUNNING = 0x02U,

    /* 정지 상태 */
    UART_DEVICE_STOPPED = 0x03U,

    /* 다른 작업을 처리 중 */
    UART_DEVICE_BUSY = 0x04U,

    /* 오류 상태 */
    UART_DEVICE_ERROR = 0x05U,

    /* 비상정지 상태 */
    UART_DEVICE_EMERGENCY_STOP = 0x06U

} uart_device_state_t;

/*
 * ============================================================================
 * 센서 상태 코드
 * ============================================================================
 */
typedef enum {
    /* 감지되지 않음 */
    UART_SENSOR_CLEAR = 0x00U,

    /* 물체 또는 대상 감지 */
    UART_SENSOR_DETECTED = 0x01U,

    /* 센서 오류 */
    UART_SENSOR_FAULT = 0x02U

} uart_sensor_state_t;

/*
 * ============================================================================
 * SENSOR_STATUS Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] sensor_id
 *   [1] sensor_state
 *   [2] distance_low
 *   [3] distance_high
 *
 * 초음파센서 거리를 사용하지 않는 경우
 * distance 값에 UART_SENSOR_DISTANCE_UNKNOWN을 사용할 수 있다.
 */
#define UART_SENSOR_ID_INDEX 0U
#define UART_SENSOR_STATE_INDEX 1U
#define UART_SENSOR_DISTANCE_LOW_INDEX 2U
#define UART_SENSOR_DISTANCE_HIGH_INDEX 3U

#define UART_SENSOR_STATUS_PAYLOAD_SIZE 4U

#define UART_SENSOR_DISTANCE_UNKNOWN 0xFFFFU

/*
 * ============================================================================
 * DEVICE_STATUS Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] device_state
 *   [1] error_code
 *   [2] device_flags
 */
#define UART_DEVICE_STATUS_STATE_INDEX 0U
#define UART_DEVICE_STATUS_ERROR_INDEX 1U
#define UART_DEVICE_STATUS_FLAGS_INDEX 2U

#define UART_DEVICE_STATUS_PAYLOAD_SIZE 3U

/*
 * ============================================================================
 * OPERATION_RESULT Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] result_status
 *   [1] error_code
 */
#define UART_OPERATION_RESULT_STATUS_INDEX 0U
#define UART_OPERATION_RESULT_ERROR_INDEX 1U

#define UART_OPERATION_RESULT_PAYLOAD_SIZE 2U

/*
 * ============================================================================
 * RESPONSE Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] status
 *   [1] original_command
 *   [2] error_code
 *   [3...] response_data
 */
#define UART_RESPONSE_STATUS_INDEX 0U
#define UART_RESPONSE_COMMAND_INDEX 1U
#define UART_RESPONSE_ERROR_INDEX 2U
#define UART_RESPONSE_HEADER_SIZE 3U

/*
 * ============================================================================
 * ACK Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] status
 *   [1] original_command
 *   [2] original_payload_length
 *   [3] original_payload_crc_low
 *   [4] original_payload_crc_high
 */
#define UART_ACK_STATUS_INDEX 0U
#define UART_ACK_COMMAND_INDEX 1U
#define UART_ACK_LENGTH_INDEX 2U
#define UART_ACK_CRC_LOW_INDEX 3U
#define UART_ACK_CRC_HIGH_INDEX 4U

#define UART_ACK_PAYLOAD_SIZE 5U

/*
 * ============================================================================
 * EVENT Payload 정의
 * ============================================================================
 *
 * Payload 구조:
 *
 *   [0] event_id
 *   [1...] event_data
 */
#define UART_EVENT_ID_INDEX 0U
#define UART_EVENT_HEADER_SIZE 1U

/*
 * ============================================================================
 * UART Frame 구조체
 * ============================================================================
 *
 * 실제 전송 시에는 구조체 자체를 UART로 보내지 않는다.
 *
 * 송신:
 *   uart_frame_t
 *   -> uart_encode_frame()
 *   -> byte 배열
 *   -> UART 전송
 *
 * 수신:
 *   UART byte
 *   -> uart_parser_feed()
 *   -> uart_decode_frame()
 *   -> uart_frame_t
 */
typedef struct {
    uint8_t version;
    uint8_t sequence;
    uint8_t command;
    uint8_t length;

    uint8_t payload[UART_MAX_PAYLOAD_SIZE];

    uint16_t crc;

} uart_frame_t;

/*
 * ============================================================================
 * Payload 길이 검증
 * ============================================================================
 *
 * 명령어별 검증 전에 전체 Payload 최대 크기를 확인한다.
 */
static inline uint8_t uart_payload_length_is_valid(uint32_t length) {
    if (length <= UART_MAX_PAYLOAD_SIZE) {
        return 1U;
    }

    return 0U;
}

#define UART_IS_VALID_PAYLOAD_LENGTH(length) uart_payload_length_is_valid((uint32_t)(length))

/*
 * ============================================================================
 * 공통 명령어 검증
 * ============================================================================
 */
static inline uint8_t uart_command_is_common(uint32_t command) {
    switch (command) {
        case UART_CMD_PING:
        case UART_CMD_GET_STATUS:
        case UART_CMD_RESET_DEVICE:

        case UART_CMD_SENSOR_STATUS:
        case UART_CMD_DEVICE_STATUS:
        case UART_CMD_OPERATION_RESULT:

        case UART_CMD_RESPONSE:
        case UART_CMD_ACK:
        case UART_CMD_EVENT:

        case UART_CMD_EMERGENCY_STOP:
            return 1U;

        default:
            return 0U;
    }
}

/*
 * ============================================================================
 * 전체 명령어 검증
 * ============================================================================
 *
 * 공통 CMD이거나 장치 CMD 예약 범위에 속하는지 검사한다.
 *
 * 실제 장치에서 지원하는 CMD인지와 Payload가 유효한지는
 * 각 장치별 contract에서 추가로 검사해야 한다.
 */
static inline uint8_t uart_command_is_valid(uint32_t command) {
    if (uart_command_is_common(command) != 0U) {
        return 1U;
    }

    if (UART_IS_DEVICE_COMMAND(command) != 0U) {
        return 1U;
    }

    return 0U;
}

#define UART_IS_VALID_COMMAND(command) uart_command_is_valid((uint32_t)(command))

/*
 * ============================================================================
 * 명령어별 Payload 길이 검증
 * ============================================================================
 *
 * 공통 명령어는 이 함수에서 고정 길이를 검증한다.
 *
 * 장치별 명령어는 공통 헤더에서 Payload의 의미를 알 수 없으므로
 * 0~128바이트 범위만 확인한다. 정확한 길이와 값 범위는
 * 각 장치별 contract에서 검사한다.
 */
static inline uint8_t uart_command_payload_length_is_valid(uint32_t command, uint32_t length) {
    /*
     * 명령어 자체가 유효한지 확인한다.
     */
    if (UART_IS_VALID_COMMAND(command) == 0U) {
        return 0U;
    }

    /*
     * 전체 Payload 최대 길이를 확인한다.
     */
    if (UART_IS_VALID_PAYLOAD_LENGTH(length) == 0U) {
        return 0U;
    }

    switch (command) {
        /*
         * Payload가 없는 명령
         */
        case UART_CMD_PING:
        case UART_CMD_GET_STATUS:
        case UART_CMD_RESET_DEVICE:
        case UART_CMD_EMERGENCY_STOP:
            return (length == 0U) ? 1U : 0U;

        /*
         * 센서 상태는 4바이트
         */
        case UART_CMD_SENSOR_STATUS:
            return (length == UART_SENSOR_STATUS_PAYLOAD_SIZE) ? 1U : 0U;

        /*
         * 장치 상태는 3바이트
         */
        case UART_CMD_DEVICE_STATUS:
            return (length == UART_DEVICE_STATUS_PAYLOAD_SIZE) ? 1U : 0U;

        /*
         * 동작 결과는 2바이트
         */
        case UART_CMD_OPERATION_RESULT:
            return (length == UART_OPERATION_RESULT_PAYLOAD_SIZE) ? 1U : 0U;

        /*
         * RESPONSE는 응답 헤더 3바이트 이상
         */
        case UART_CMD_RESPONSE:
            return (length >= UART_RESPONSE_HEADER_SIZE) ? 1U : 0U;

        /*
         * ACK는 고정 5바이트
         */
        case UART_CMD_ACK:
            return (length == UART_ACK_PAYLOAD_SIZE) ? 1U : 0U;

        /*
         * EVENT는 event_id 1바이트 이상
         */
        case UART_CMD_EVENT:
            return (length >= UART_EVENT_HEADER_SIZE) ? 1U : 0U;

        /*
         * 장치별 명령은 공통 코드에서
         * 최대 Payload 길이만 검증한다.
         */
        default:
            if (UART_IS_DEVICE_COMMAND(command) != 0U) {
                return 1U;
            }

            return 0U;
    }
}

#define UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(command, length) \
    uart_command_payload_length_is_valid((uint32_t)(command), (uint32_t)(length))

/*
 * ============================================================================
 * 비상정지 명령 확인
 * ============================================================================
 *
 * 이 매크로는 비상정지 명령인지 여부만 확인한다.
 *
 * 실제 비상정지 처리:
 *
 *   1. 일반 명령 Queue보다 우선 처리
 *   2. 컨베이어 모터 정지
 *   3. 서보모터 동작 정지
 *   4. 라인트레이서 주행 정지
 *   5. 자동 재시작 금지
 *
 * 위 동작은 STM32 main.c의 안전 제어 코드에서 구현한다.
 */
#define UART_IS_EMERGENCY_COMMAND(command) ((uint32_t)(command) == UART_CMD_EMERGENCY_STOP)

/*
 * ============================================================================
 * CRC 바이트 변환 매크로
 * ============================================================================
 */
#define UART_CRC_LOW_BYTE(crc) ((uint8_t)((uint16_t)(crc) & 0xFFU))

#define UART_CRC_HIGH_BYTE(crc) ((uint8_t)(((uint16_t)(crc) >> 8U) & 0xFFU))

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */
