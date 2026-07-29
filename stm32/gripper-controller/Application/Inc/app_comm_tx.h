#ifndef APP_COMM_TX_H
#define APP_COMM_TX_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"
#include "stm32f4xx_hal.h"

#define APP_EVENT_HEARTBEAT 0x01U
#define APP_EVENT_SAFETY 0x02U

#define APP_HEARTBEAT_STATE_INDEX 1U
#define APP_HEARTBEAT_ERROR_INDEX 2U
#define APP_HEARTBEAT_UPTIME_INDEX 3U
#define APP_HEARTBEAT_PAYLOAD_SIZE 7U

typedef struct {
    uint32_t enqueued;
    uint32_t urgent_enqueued;
    uint32_t dropped;
    uint32_t encode_errors;
    uint32_t transmit_errors;
    uint32_t dma_start_errors;
    uint32_t dma_timeouts;
    uint32_t dma_errors;
    uint32_t aborts;
    uint32_t abort_timeouts;
    uint32_t retries;
    uint32_t sent;
    uint32_t heartbeats;
} comm_tx_stats_t;

int32_t CommTx_Send(uint8_t command, const uint8_t* payload, uint8_t length);
int32_t CommTx_SendWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length);
int32_t CommTx_SendUrgent(uint8_t command, const uint8_t* payload, uint8_t length);
int32_t CommTx_SendUrgentWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length);

void CommTx_SetDeviceStatus(uint8_t state, uint8_t error);
void CommTx_GetStats(comm_tx_stats_t* stats);
uint8_t CommTx_HandleUartError(UART_HandleTypeDef* huart);
void StartCommTxTask(void* argument);

#endif /* APP_COMM_TX_H */
