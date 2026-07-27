#ifndef COMM_TX_TASK_H
#define COMM_TX_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames_started;
    uint32_t frames_completed;
    uint32_t heartbeats_sent;
    uint32_t events_sent;
    uint32_t encode_errors;
    uint32_t dma_start_errors;
    uint32_t dma_timeouts;
    uint32_t abort_timeouts;
    uint32_t dropped_events;
} comm_tx_stats_t;

void StartCommTxTask(void* argument);
void CommTxTask_NotifyQueueReady(void);
void CommTxTask_GetStats(comm_tx_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif /* COMM_TX_TASK_H */
