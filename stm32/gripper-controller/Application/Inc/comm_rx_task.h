#ifndef COMM_RX_TASK_H
#define COMM_RX_TASK_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

typedef struct {
    uint32_t received_frames;
    uint32_t gripper_commands;
    uint32_t safety_commands;
    uint32_t parser_errors;
    uint32_t invalid_payloads;
    uint32_t unsupported_commands;
    uint32_t queue_drops;
    uint32_t uart_restarts;
} comm_rx_stats_t;

void StartCommRxTask(void* argument);
void comm_rx_process_frame(const uart_frame_t* frame);
void comm_rx_get_stats(comm_rx_stats_t* stats);
uint32_t comm_rx_get_last_rx_tick(void);

#endif /* COMM_RX_TASK_H */
