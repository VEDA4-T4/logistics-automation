#include "logistics/contracts/uart_parser.h"

#include <string.h>

#include "logistics/contracts/uart_codec.h"

/*
 * Parser를 초기 상태로 되돌린다.
 */
static void parser_reset_internal(uart_parser_t* parser) {
    parser->state = UART_PARSER_WAIT_SOF;
    parser->raw_length = 0U;
    parser->expected_length = 0U;
    parser->payload_length = 0U;
    parser->payload_index = 0U;
    parser->elapsed_ms = 0U;
}

/*
 * 새로운 Frame 수신을 시작한다.
 */
static void parser_start_frame(uart_parser_t* parser) {
    parser_reset_internal(parser);

    parser->raw_frame[0] = UART_SOF;
    parser->raw_length = 1U;
    parser->state = UART_PARSER_READ_VERSION;
}

/*
 * 현재 byte를 새로운 SOF로 사용하여 재동기화한다.
 */
static void parser_reset_and_resync(uart_parser_t* parser, uint8_t byte) {
    parser_reset_internal(parser);

    if (byte == UART_SOF) {
        parser_start_frame(parser);
    }
}

/*
 * Raw Frame에 byte를 저장한다.
 *
 * 반환값:
 *   1: 저장 성공
 *   0: 버퍼 부족
 */
static uint8_t parser_store_byte(uart_parser_t* parser, uint8_t byte) {
    if (parser->raw_length >= UART_MAX_FRAME_SIZE) {
        return 0U;
    }

    parser->raw_frame[parser->raw_length] = byte;
    parser->raw_length++;

    return 1U;
}

/*
 * Codec 결과를 Parser 결과로 변환한다.
 */
static uart_parser_result_t map_codec_result(uart_codec_result_t result) {
    switch (result) {
        case UART_CODEC_OK:
            return UART_PARSER_FRAME_READY;

        case UART_CODEC_INVALID_ARGUMENT:
            return UART_PARSER_INVALID_ARGUMENT;

        case UART_CODEC_INVALID_SOF:
            return UART_PARSER_INVALID_SOF;

        case UART_CODEC_INVALID_VERSION:
            return UART_PARSER_INVALID_VERSION;

        case UART_CODEC_INVALID_LENGTH:
            return UART_PARSER_INVALID_LENGTH;

        case UART_CODEC_INVALID_COMMAND:
            return UART_PARSER_INVALID_COMMAND;

        case UART_CODEC_CRC_MISMATCH:
            return UART_PARSER_CRC_ERROR;

        case UART_CODEC_INCOMPLETE_FRAME:
        case UART_CODEC_TRAILING_DATA:
        case UART_CODEC_OUTPUT_TOO_SMALL:
        default:
            return UART_PARSER_INVALID_LENGTH;
    }
}

/*
 * Parser 초기화
 */
void uart_parser_init(uart_parser_t* parser) {
    if (parser == NULL) {
        return;
    }

    memset(parser, 0, sizeof(*parser));
    parser_reset_internal(parser);
}

/*
 * Parser 초기화 및 수신 상태 삭제
 */
void uart_parser_reset(uart_parser_t* parser) {
    if (parser == NULL) {
        return;
    }

    parser_reset_internal(parser);
}

/*
 * UART에서 수신한 1바이트를 Parser에 전달한다.
 *
 * 부분 수신:
 *   여러 번 호출하면서 하나의 Frame을 완성한다.
 *
 * 연속 Frame:
 *   FRAME_READY 반환 후 다음 byte부터 새로운 Frame으로 처리한다.
 */
uart_parser_result_t uart_parser_feed(uart_parser_t* parser, uint8_t byte, uart_frame_t* frame) {
    uart_codec_result_t codec_result;
    uint8_t command;

    if (parser == NULL || frame == NULL) {
        return UART_PARSER_INVALID_ARGUMENT;
    }

    switch (parser->state) {
        /*
         * SOF 대기
         */
        case UART_PARSER_WAIT_SOF:

            if (byte != UART_SOF) {
                return UART_PARSER_NO_FRAME;
            }

            parser_start_frame(parser);

            return UART_PARSER_NO_FRAME;

        /*
         * VERSION 수신
         */
        case UART_PARSER_READ_VERSION:

            /*
             * 잘못된 VERSION은 즉시 폐기한다.
             */
            if (byte != UART_PROTOCOL_VERSION) {
                parser_reset_and_resync(parser, byte);
                return UART_PARSER_INVALID_VERSION;
            }

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->elapsed_ms = 0U;
            parser->state = UART_PARSER_READ_SEQUENCE;

            return UART_PARSER_NO_FRAME;

        /*
         * SEQUENCE 수신
         */
        case UART_PARSER_READ_SEQUENCE:

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->elapsed_ms = 0U;
            parser->state = UART_PARSER_READ_COMMAND;

            return UART_PARSER_NO_FRAME;

        /*
         * COMMAND 수신
         */
        case UART_PARSER_READ_COMMAND:

            /*
             * 잘못된 COMMAND는 즉시 폐기한다.
             */
            if (UART_IS_VALID_COMMAND(byte) == 0U) {
                parser_reset_and_resync(parser, byte);
                return UART_PARSER_INVALID_COMMAND;
            }

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->elapsed_ms = 0U;
            parser->state = UART_PARSER_READ_LENGTH;

            return UART_PARSER_NO_FRAME;

        /*
         * LENGTH 수신
         */
        case UART_PARSER_READ_LENGTH:

            command = parser->raw_frame[3];

            /*
             * 전체 Payload 최대 크기 검증
             */
            if (UART_IS_VALID_PAYLOAD_LENGTH(byte) == 0U) {
                parser_reset_and_resync(parser, byte);
                return UART_PARSER_INVALID_LENGTH;
            }

            /*
             * 명령어별 Payload 길이 검증
             */
            if (UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(command, byte) == 0U) {
                parser_reset_and_resync(parser, byte);
                return UART_PARSER_INVALID_LENGTH;
            }

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->payload_length = byte;
            parser->payload_index = 0U;
            parser->expected_length = UART_FRAME_OVERHEAD_SIZE + parser->payload_length;
            parser->elapsed_ms = 0U;

            if (parser->payload_length == 0U) {
                parser->state = UART_PARSER_READ_CRC_LOW;
            } else {
                parser->state = UART_PARSER_READ_PAYLOAD;
            }

            return UART_PARSER_NO_FRAME;

        /*
         * PAYLOAD 수신
         */
        case UART_PARSER_READ_PAYLOAD:

            if (parser->payload_index >= parser->payload_length) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->payload_index++;
            parser->elapsed_ms = 0U;

            if (parser->payload_index >= parser->payload_length) {
                parser->state = UART_PARSER_READ_CRC_LOW;
            }

            return UART_PARSER_NO_FRAME;

        /*
         * CRC Low Byte 수신
         */
        case UART_PARSER_READ_CRC_LOW:

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->elapsed_ms = 0U;
            parser->state = UART_PARSER_READ_CRC_HIGH;

            return UART_PARSER_NO_FRAME;

        /*
         * CRC High Byte 수신
         */
        case UART_PARSER_READ_CRC_HIGH:

            if (parser_store_byte(parser, byte) == 0U) {
                parser_reset_internal(parser);
                return UART_PARSER_INVALID_LENGTH;
            }

            parser->elapsed_ms = 0U;

            /*
             * 완성된 하나의 Frame을 Codec으로 검증한다.
             */
            codec_result = uart_decode_frame(parser->raw_frame, parser->raw_length, frame);

            /*
             * 다음 Frame 수신을 위해 Parser를 초기화한다.
             */
            parser_reset_internal(parser);

            /*
             * CRC 오류가 발생했고 현재 byte가 SOF라면
             * 다음 Frame의 시작으로 재동기화한다.
             */
            if (codec_result != UART_CODEC_OK && byte == UART_SOF) {
                parser_start_frame(parser);
            }

            return map_codec_result(codec_result);

        /*
         * 알 수 없는 상태
         */
        default:

            parser_reset_internal(parser);
            return UART_PARSER_INVALID_ARGUMENT;
    }
}

/*
 * Parser timeout 처리
 *
 * 일정 시간 동안 다음 byte가 수신되지 않으면
 * 현재 부분 Frame을 폐기한다.
 */
uart_parser_result_t uart_parser_tick(uart_parser_t* parser, uint32_t elapsed_ms) {
    if (parser == NULL) {
        return UART_PARSER_INVALID_ARGUMENT;
    }

    if (parser->state == UART_PARSER_WAIT_SOF) {
        return UART_PARSER_NO_FRAME;
    }

    /*
     * 이미 timeout을 초과했거나,
     * 이번 elapsed 시간이 남은 timeout 시간 이상이면
     * 현재 Frame을 폐기한다.
     */
    if (parser->elapsed_ms >= UART_PARSER_TIMEOUT_MS || elapsed_ms >= (UART_PARSER_TIMEOUT_MS - parser->elapsed_ms)) {
        parser_reset_internal(parser);
        return UART_PARSER_TIMEOUT;
    }

    parser->elapsed_ms += elapsed_ms;

    return UART_PARSER_NO_FRAME;
}
