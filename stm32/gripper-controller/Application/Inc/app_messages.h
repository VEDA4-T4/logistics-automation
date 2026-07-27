#ifndef APP_MESSAGES_H
#define APP_MESSAGES_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

#define UART_RX_CHUNK_SIZE 64U

typedef struct {
    uint16_t length;
    uint8_t data[UART_RX_CHUNK_SIZE];
} uart_rx_chunk_t;

typedef enum {
    APP_CONTROL_MESSAGE_UART_COMMAND = 0,
    APP_CONTROL_MESSAGE_SAFETY_STOP,
    APP_CONTROL_MESSAGE_SAFETY_RELEASE
} app_control_message_kind_t;

typedef struct {
    uart_frame_t frame;
    app_control_message_kind_t kind;
    uint32_t safety_epoch;
} control_command_t;

typedef struct {
    uint8_t command;
    uint8_t length;
    uint8_t sequence;
    uint8_t use_provided_sequence;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} comm_tx_message_t;

#endif /* APP_MESSAGES_H */
