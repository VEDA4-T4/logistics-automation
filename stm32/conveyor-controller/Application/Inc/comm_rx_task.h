#ifndef COMM_RX_TASK_H
#define COMM_RX_TASK_H

#include <stdint.h>

#include "app_messages.h"

typedef struct {
    uint32_t receivedFrames;
    uint32_t inputCommands;
    uint32_t sortingCommands;
    uint32_t safetyCommands;
    uint32_t parserErrors;
    uint32_t invalidPayloads;
    uint32_t unsupportedCommands;
    uint32_t controlQueueDrops;
    uint32_t nackResponses;
    uint32_t busyResponses;
    uint32_t responseDrops;
    uint32_t rxQueueDrops;
    uint32_t uartRestarts;
} comm_rx_stats_t;

void StartCommRxTask(void* argument);

void comm_rx_process_frame(app_uart_channel_t source, const uart_frame_t* frame);

void comm_rx_get_stats(comm_rx_stats_t* stats);

#endif /* COMM_RX_TASK_H */
