#ifndef COMM_RX_DISPATCH_H
#define COMM_RX_DISPATCH_H

#include <stdint.h>

#include "comm_rx_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMM_RX_DISPATCH_PENDING_CAPACITY 8U
#define COMM_RX_DISPATCH_RESPONSE_RETRY_DELAY_MS 10U
#define COMM_RX_DISPATCH_RESPONSE_MAX_RETRIES 3U

typedef uint8_t (*comm_rx_put_control_fn)(void* context, const app_control_command_t* command);
typedef uint8_t (*comm_rx_put_safety_fn)(void* context, const app_safety_event_t* event, uint8_t priority);
typedef uint8_t (*comm_rx_put_response_fn)(void* context, const app_tx_event_t* event);
typedef void (*comm_rx_report_health_fn)(void* context, app_health_event_type_t type, uint32_t detail, uint32_t now_ms);

typedef struct {
    void* context;
    comm_rx_put_control_fn put_control;
    comm_rx_put_safety_fn put_safety;
    comm_rx_put_response_fn put_response;
    comm_rx_report_health_fn report_health;
} comm_rx_dispatch_port_t;

typedef struct {
    app_tx_event_t event;
    uint32_t retry_at_ms;
    uint8_t active;
} comm_rx_pending_response_t;

typedef struct {
    comm_rx_sequence_history_t sequence_history;
    comm_rx_pending_response_t pending_responses[COMM_RX_DISPATCH_PENDING_CAPACITY];
} comm_rx_dispatch_t;

typedef struct {
    uint32_t control_commands;
    uint32_t safety_commands;
    uint32_t local_responses;
    uint32_t duplicate_sequences;
    uint32_t invalid_payloads;
    uint32_t unsupported_commands;
    uint32_t queue_drops;
    uint32_t response_retries;
    uint32_t response_timeouts;
} comm_rx_dispatch_effects_t;

void CommRxDispatch_Init(comm_rx_dispatch_t* dispatcher);
void CommRxDispatch_Frame(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port,
                          const uart_frame_t* frame, uint32_t now_ms, comm_rx_dispatch_effects_t* effects);
void CommRxDispatch_ProcessPending(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port, uint32_t now_ms,
                                   comm_rx_dispatch_effects_t* effects);
uint32_t CommRxDispatch_PendingCount(const comm_rx_dispatch_t* dispatcher);

#ifdef __cplusplus
}
#endif

#endif /* COMM_RX_DISPATCH_H */
