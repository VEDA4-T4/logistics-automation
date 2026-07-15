#include "logistics/contracts/uart_crc16.h"

uint16_t uart_crc16_ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = UART_CRC16_INITIAL_VALUE;

    if (data == NULL && length != 0U)
        return 0U;

    for (size_t i = 0U; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8U;

        for (uint8_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U)
                crc = (uint16_t)((crc << 1U) ^ UART_CRC16_POLYNOMIAL);
            else
                crc <<= 1U;
        }
    }

    return (uint16_t)(crc ^ UART_CRC16_XOR_OUT);
}
