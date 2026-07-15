#include "uart_codec.h"

#include <string.h>

/*
 * Frame 인코딩
 *
 * uart_frame_t 구조체를 UART로 전송할 byte 배열로 변환한다.
 */
uart_codec_result_t uart_encode_frame(const uart_frame_t* frame, uint8_t* output, size_t output_capacity,
                                      size_t* output_length) {
    size_t frame_length;
    uint16_t crc;

    if (frame == NULL || output == NULL || output_length == NULL) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    *output_length = 0U;

    /* 전체 Payload 최대 길이 검증 */
    if (!UART_IS_VALID_PAYLOAD_LENGTH(frame->length)) {
        return UART_CODEC_INVALID_LENGTH;
    }

    /* 명령어 자체의 유효성 검증 */
    if (!UART_IS_VALID_COMMAND(frame->command)) {
        return UART_CODEC_INVALID_COMMAND;
    }

    /*
     * 명령어별 Payload 길이 검증
     *
     * 예:
     *   PING              -> Payload 0바이트
     *   SENSOR_STATUS     -> Payload 4바이트
     *   ACK               -> Payload 5바이트
     *   장치별 명령       -> 0~128바이트
     */
    if (!UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(frame->command, frame->length)) {
        return UART_CODEC_INVALID_LENGTH;
    }

    /* 프로토콜 버전 검증 */
    if (frame->version != UART_PROTOCOL_VERSION) {
        return UART_CODEC_INVALID_VERSION;
    }

    /* 전체 Frame 크기 계산 */
    frame_length = UART_FRAME_OVERHEAD_SIZE + frame->length;

    /* 출력 버퍼 크기 검증 */
    if (output_capacity < frame_length) {
        return UART_CODEC_OUTPUT_TOO_SMALL;
    }

    /*
     * Header 작성
     *
     * [0] SOF
     * [1] VERSION
     * [2] SEQUENCE
     * [3] COMMAND
     * [4] LENGTH
     */
    output[0] = UART_SOF;
    output[1] = frame->version;
    output[2] = frame->sequence;
    output[3] = frame->command;
    output[4] = frame->length;

    /* Payload 복사 */
    if (frame->length > 0U) {
        memcpy(&output[UART_FRAME_HEADER_SIZE], frame->payload, frame->length);
    }

    /*
     * VERSION부터 PAYLOAD까지 CRC 계산
     *
     * output[1]부터 시작하며,
     * VERSION, SEQUENCE, COMMAND, LENGTH, PAYLOAD을 포함한다.
     */
    crc = uart_crc16_ccitt(&output[1], 4U + frame->length);

    /*
     * CRC는 Little-endian 순서로 저장한다.
     */
    output[5U + frame->length] = UART_CRC_LOW_BYTE(crc);

    output[6U + frame->length] = UART_CRC_HIGH_BYTE(crc);

    *output_length = frame_length;

    return UART_CODEC_OK;
}

/*
 * Frame 디코딩
 *
 * UART에서 수신한 완성된 byte 배열을
 * uart_frame_t 구조체로 변환한다.
 *
 * 주의:
 * 이 함수는 하나의 완성된 Frame만 입력으로 받는다.
 * 부분 수신이나 연속 Frame 처리는 uart_parser.c가 담당한다.
 */
uart_codec_result_t uart_decode_frame(const uint8_t* input, size_t input_length, uart_frame_t* frame) {
    uint8_t payload_length;
    size_t expected_length;
    uint16_t received_crc;
    uint16_t calculated_crc;

    if (input == NULL || frame == NULL) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    /*
     * 최소 Frame 크기 확인
     *
     * Payload가 0바이트인 경우에도
     * 총 7바이트가 필요하다.
     */
    if (input_length < UART_FRAME_OVERHEAD_SIZE) {
        return UART_CODEC_INCOMPLETE_FRAME;
    }

    /* SOF 검증 */
    if (input[0] != UART_SOF) {
        return UART_CODEC_INVALID_SOF;
    }

    /* 프로토콜 버전 검증 */
    if (input[1] != UART_PROTOCOL_VERSION) {
        return UART_CODEC_INVALID_VERSION;
    }

    /* 명령어 자체의 유효성 검증 */
    if (!UART_IS_VALID_COMMAND(input[3])) {
        return UART_CODEC_INVALID_COMMAND;
    }

    /* Payload 길이 읽기 */
    payload_length = input[4];

    /* 전체 Payload 최대 길이 검증 */
    if (!UART_IS_VALID_PAYLOAD_LENGTH(payload_length)) {
        return UART_CODEC_INVALID_LENGTH;
    }

    /*
     * 명령어별 Payload 길이 검증
     *
     * 명령어와 Payload 크기가 맞지 않으면
     * 해당 Frame을 실행하지 않는다.
     */
    if (!UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(input[3], payload_length)) {
        return UART_CODEC_INVALID_LENGTH;
    }

    /* 예상되는 전체 Frame 크기 계산 */
    expected_length = UART_FRAME_OVERHEAD_SIZE + payload_length;

    /*
     * 아직 Frame이 완성되지 않은 경우
     */
    if (input_length < expected_length) {
        return UART_CODEC_INCOMPLETE_FRAME;
    }

    /*
     * 하나의 Frame보다 더 많은 데이터가 들어온 경우
     *
     * 연속 Frame 처리는 parser가 담당하므로
     * decode 함수에서는 오류로 처리한다.
     */
    if (input_length > expected_length) {
        return UART_CODEC_TRAILING_DATA;
    }

    /*
     * 수신된 CRC 읽기
     *
     * CRC는 Low Byte, High Byte 순서이다.
     */
    received_crc = (uint16_t)input[5U + payload_length];

    received_crc |= (uint16_t)input[6U + payload_length] << 8U;

    /*
     * VERSION부터 PAYLOAD까지 CRC 계산
     */
    calculated_crc = uart_crc16_ccitt(&input[1], 4U + payload_length);

    /* CRC 비교 */
    if (received_crc != calculated_crc) {
        return UART_CODEC_CRC_MISMATCH;
    }

    /*
     * 검증이 모두 완료된 후 Frame 구조체에 저장한다.
     */
    frame->version = input[1];
    frame->sequence = input[2];
    frame->command = input[3];
    frame->length = payload_length;

    /* Payload 복사 */
    if (payload_length > 0U) {
        memcpy(frame->payload, &input[UART_FRAME_HEADER_SIZE], payload_length);
    }

    frame->crc = received_crc;

    return UART_CODEC_OK;
}