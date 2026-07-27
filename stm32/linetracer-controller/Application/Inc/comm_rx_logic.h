#ifndef COMM_RX_LOGIC_H
#define COMM_RX_LOGIC_H

#include <stdint.h>

#include "app_messages.h"
#include "logistics/contracts/uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMM_RX_SEQUENCE_SPACE_SIZE 256U
#define COMM_RX_SEQUENCE_VALIDITY_BYTES (COMM_RX_SEQUENCE_SPACE_SIZE / 8U)
#define COMM_RX_SEQUENCE_REPLAY_WINDOW_MS UART_COMMAND_TIMEOUT_MS

typedef enum {
    COMM_RX_DECODE_ACCEPTED = 0,
    COMM_RX_DECODE_DUPLICATE,
    COMM_RX_DECODE_INVALID_FRAME,
    COMM_RX_DECODE_INVALID_PAYLOAD,
    COMM_RX_DECODE_UNSUPPORTED_COMMAND
} comm_rx_decode_result_t;

typedef enum {
    COMM_RX_DESTINATION_NONE = 0,
    COMM_RX_DESTINATION_CONTROL,
    COMM_RX_DESTINATION_SAFETY,
    COMM_RX_DESTINATION_LOCAL_ACK
} comm_rx_destination_t;

typedef struct {
    uint32_t accepted_at_ms[COMM_RX_SEQUENCE_SPACE_SIZE];
    uint8_t valid[COMM_RX_SEQUENCE_VALIDITY_BYTES];
} comm_rx_sequence_history_t;

typedef struct {
    uint32_t last_raw_activity_ms;
    uint32_t last_valid_frame_ms;
    uint8_t timeout_reported;
} comm_rx_link_monitor_t;

typedef struct {
    comm_rx_destination_t destination;
    app_control_command_t control_command;
    app_safety_event_t safety_event;
} comm_rx_decoded_command_t;

void CommRxLogic_Init(comm_rx_sequence_history_t* history);
uint8_t CommRxLogic_IsDuplicate(const comm_rx_sequence_history_t* history, uint8_t sequence, uint32_t now_ms);
void CommRxLogic_CommitSequence(comm_rx_sequence_history_t* history, uint8_t sequence, uint32_t now_ms);
void CommRxLogic_LinkInit(comm_rx_link_monitor_t* monitor, uint32_t now_ms);
void CommRxLogic_LinkRecordRaw(comm_rx_link_monitor_t* monitor, uint32_t now_ms);
void CommRxLogic_LinkRecordValidFrame(comm_rx_link_monitor_t* monitor, uint32_t now_ms);
uint8_t CommRxLogic_LinkCheckTimeout(comm_rx_link_monitor_t* monitor, uint32_t now_ms, uint32_t timeout_ms);
comm_rx_decode_result_t CommRxLogic_DecodeFrame(const comm_rx_sequence_history_t* history, const uart_frame_t* frame,
                                                uint32_t now_ms, comm_rx_decoded_command_t* decoded);

#ifdef __cplusplus
}
#endif

#endif /* COMM_RX_LOGIC_H */
