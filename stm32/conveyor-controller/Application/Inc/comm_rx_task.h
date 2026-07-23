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

/* HealthTask가 채널 단절(timeout) 판정에 쓰는, 해당 채널의 마지막 바이트 수신
 * 시각(HAL_GetTick 기준). 프레임 파싱 성공 여부와 무관하다. */
uint32_t comm_rx_get_channel_last_rx_tick(app_uart_channel_t channel);

#endif /* COMM_RX_TASK_H */
