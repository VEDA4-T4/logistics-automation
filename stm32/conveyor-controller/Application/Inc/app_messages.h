#ifndef APP_MESSAGES_H
#define APP_MESSAGES_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

typedef enum { APP_UART_CHANNEL_1 = 1, APP_UART_CHANNEL_6 = 6 } app_uart_channel_t;

/* DMA 콜백에서 CommRxTask로 전달하는 최대 데이터 크기 */
#define UART_RX_CHUNK_SIZE 64U

typedef struct {
    app_uart_channel_t channel;
    uint16_t length;
    uint8_t data[UART_RX_CHUNK_SIZE];
} uart_rx_chunk_t;

/* CommRxTask에서 각 제어 태스크로 전달하는 명령 */
typedef struct {
    app_uart_channel_t source;
    uart_frame_t frame;
} control_command_t;

#endif /* APP_MESSAGES_H */
