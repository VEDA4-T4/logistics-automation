#ifndef UART_CODEC_H
#define UART_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "uart_crc16.h"
#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UART_CODEC_OK = 0,
    UART_CODEC_INVALID_ARGUMENT = -1,
    UART_CODEC_OUTPUT_TOO_SMALL = -2,
    UART_CODEC_INCOMPLETE_FRAME = -3,
    UART_CODEC_INVALID_SOF = -4,
    UART_CODEC_INVALID_VERSION = -5,
    UART_CODEC_INVALID_LENGTH = -6,
    UART_CODEC_INVALID_COMMAND = -7,
    UART_CODEC_CRC_MISMATCH = -8,
    UART_CODEC_TRAILING_DATA = -9

} uart_codec_result_t;

/* 논리 Frame을 송신용 byte 배열로 변환 */
uart_codec_result_t uart_encode_frame(const uart_frame_t* frame, uint8_t* output, size_t output_capacity,
                                      size_t* output_length);

/* 하나의 완성된 byte 배열을 논리 Frame으로 변환 */
uart_codec_result_t uart_decode_frame(const uint8_t* input, size_t input_length, uart_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif /* UART_CODEC_H */