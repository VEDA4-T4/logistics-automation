#ifndef COMM_RX_TASK_H
#define COMM_RX_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t received_bytes;
    uint32_t received_frames;
    uint32_t control_commands;
    uint32_t safety_commands;
    uint32_t local_responses;
    uint32_t duplicate_sequences;
    uint32_t invalid_payloads;
    uint32_t unsupported_commands;
    uint32_t parser_errors;
    uint32_t queue_drops;
    uint32_t response_retries;
    uint32_t response_timeouts;
    uint32_t uart_restarts;
    uint32_t link_timeouts;
    uint32_t last_raw_activity_ms;
    uint32_t last_valid_frame_ms;
} comm_rx_stats_t;

void StartCommRxTask(void* argument);

/* Runs only in CommRxTask context; DMA/USART callbacks never call the parser. */
void CommRxTask_ProcessReceivedBytes(const uint8_t* data, uint16_t length);
void CommRxTask_GetStats(comm_rx_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif /* COMM_RX_TASK_H */
