#ifndef UART_PARSER_H
#define UART_PARSER_H

#include <stdint.h>

#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * UART Parser 설정
 * ============================================================================
 *
 * Frame 일부를 수신한 후 다음 byte가 들어오지 않는 최대 시간이다.
 *
 * 예:
 *
 *   SOF, VERSION, SEQUENCE까지만 수신
 *   100ms 동안 추가 byte가 수신되지 않음
 *   -> 현재 Frame 폐기
 *   -> UART_PARSER_TIMEOUT 반환
 *
 * 프로젝트 환경에 따라 외부 빌드 옵션으로 변경할 수 있다.
 */
#ifndef UART_PARSER_TIMEOUT_MS
#define UART_PARSER_TIMEOUT_MS 100U
#endif

/*
 * ============================================================================
 * Parser 상태
 * ============================================================================
 *
 * Parser는 UART byte stream을 한 byte씩 받아
 * 현재 어느 필드를 수신하고 있는지 상태로 관리한다.
 */
typedef enum {
    /* SOF 대기 */
    UART_PARSER_WAIT_SOF = 0,

    /* VERSION 수신 */
    UART_PARSER_READ_VERSION,

    /* SEQUENCE 수신 */
    UART_PARSER_READ_SEQUENCE,

    /* COMMAND 수신 */
    UART_PARSER_READ_COMMAND,

    /* LENGTH 수신 */
    UART_PARSER_READ_LENGTH,

    /* Payload 수신 */
    UART_PARSER_READ_PAYLOAD,

    /* CRC Low Byte 수신 */
    UART_PARSER_READ_CRC_LOW,

    /* CRC High Byte 수신 */
    UART_PARSER_READ_CRC_HIGH

} uart_parser_state_t;

/*
 * ============================================================================
 * Parser 처리 결과
 * ============================================================================
 *
 * UART_PARSER_FRAME_READY가 반환된 경우에만
 * 출력 uart_frame_t의 내용이 유효하다.
 *
 * NO_FRAME 또는 오류가 반환된 경우에는
 * 출력 Frame을 사용하지 않아야 한다.
 */
typedef enum {
    /* 아직 완성된 Frame이 없음 */
    UART_PARSER_NO_FRAME = 0,

    /* 하나의 완성된 Frame이 정상적으로 수신됨 */
    UART_PARSER_FRAME_READY = 1,

    /* 함수 인자가 잘못됨 */
    UART_PARSER_INVALID_ARGUMENT = -1,

    /* 잘못된 SOF */
    UART_PARSER_INVALID_SOF = -2,

    /* 잘못된 프로토콜 버전 */
    UART_PARSER_INVALID_VERSION = -3,

    /* 잘못된 Payload 길이 */
    UART_PARSER_INVALID_LENGTH = -4,

    /* 등록되지 않은 명령어 */
    UART_PARSER_INVALID_COMMAND = -5,

    /* CRC 검증 실패 */
    UART_PARSER_CRC_ERROR = -6,

    /* 부분 Frame 수신 중 Timeout */
    UART_PARSER_TIMEOUT = -7

} uart_parser_result_t;

/*
 * ============================================================================
 * UART Parser 객체
 * ============================================================================
 *
 * Parser는 UART 채널마다 별도로 생성해야 한다.
 *
 * 예:
 *
 *   USART1 -> uart_parser_t usart1_parser;
 *   USART6 -> uart_parser_t usart6_parser;
 *
 * 두 채널이 같은 Parser 객체를 공유하면
 * 서로 다른 UART의 byte가 섞일 수 있다.
 */
typedef struct {
    /*
     * 현재 Parser 상태
     */
    uart_parser_state_t state;

    /*
     * 현재 수신 중인 Raw Frame
     *
     * SOF부터 CRC High Byte까지 저장한다.
     */
    uint8_t raw_frame[UART_MAX_FRAME_SIZE];

    /*
     * 현재 raw_frame에 저장된 byte 수
     */
    uint16_t raw_length;

    /*
     * LENGTH를 기준으로 계산한 예상 전체 Frame 크기
     */
    uint16_t expected_length;

    /*
     * Header에서 수신한 Payload 길이
     */
    uint8_t payload_length;

    /*
     * 현재까지 수신한 Payload byte 수
     */
    uint8_t payload_index;

    /*
     * 마지막 byte 수신 이후 경과한 시간
     *
     * uart_parser_tick()으로 갱신한다.
     */
    uint32_t elapsed_ms;

} uart_parser_t;

/*
 * Parser를 초기 상태로 초기화한다.
 *
 * 초기 상태:
 *
 *   state          = UART_PARSER_WAIT_SOF
 *   raw_length     = 0
 *   payload_index  = 0
 *   elapsed_ms     = 0
 */
void uart_parser_init(uart_parser_t* parser);

/*
 * 현재 수신 중인 Frame을 폐기하고
 * SOF 대기 상태로 되돌린다.
 */
void uart_parser_reset(uart_parser_t* parser);

/*
 * UART에서 수신한 byte 하나를 Parser에 입력한다.
 *
 * 호출 방식:
 *
 *   uint8_t byte;
 *   uart_frame_t frame;
 *
 *   result = uart_parser_feed(
 *       &parser,
 *       byte,
 *       &frame
 *   );
 *
 * 반환값이 UART_PARSER_FRAME_READY인 경우:
 *
 *   frame.version
 *   frame.sequence
 *   frame.command
 *   frame.length
 *   frame.payload
 *   frame.crc
 *
 * 를 사용할 수 있다.
 *
 * 부분 수신:
 *
 *   UART byte가 여러 번 나누어 수신되어도
 *   Parser 상태를 유지하면서 Frame을 완성한다.
 *
 * 연속 Frame:
 *
 *   하나의 Frame 처리가 끝나면 다음 byte부터
 *   새로운 Frame으로 자동 처리한다.
 */
uart_parser_result_t uart_parser_feed(uart_parser_t* parser, uint8_t byte, uart_frame_t* frame);

/*
 * 부분 수신 Timeout을 처리한다.
 *
 * elapsed_ms는 마지막 호출 이후 경과한 시간이다.
 *
 * 예:
 *
 *   uart_parser_tick(&parser, 10U);
 *
 * 10ms가 경과했음을 Parser에 알린다.
 *
 * UART 수신이 중단된 상태에서
 * UART_PARSER_TIMEOUT_MS 이상 시간이 지나면
 * 현재 수신 중인 Frame을 폐기한다.
 */
uart_parser_result_t uart_parser_tick(uart_parser_t* parser, uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif

#endif /* UART_PARSER_H */