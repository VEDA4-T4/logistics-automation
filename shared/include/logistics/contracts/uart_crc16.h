#ifndef UART_CRC16_H
#define UART_CRC16_H

#include <stddef.h>
#include <stdint.h>

#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CRC16-CCITT 계산
 *
 * 계산 범위는 호출자가 전달한 byte 배열 전체이다.
 * UART Frame에서는 VERSION부터 PAYLOAD까지 전달한다.
 */
uint16_t uart_crc16_ccitt(
    const uint8_t *data,
    size_t length
);

#ifdef __cplusplus
}
#endif

#endif /* UART_CRC16_H */
