#ifndef COMM_TX_LOGIC_H
#define COMM_TX_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#include "app_messages.h"
#include "logistics/contracts/uart_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t next_sequence;
} comm_tx_logic_t;

/* Best-effort state reconstructed from the existing public TX events and sensor snapshot API. */
typedef struct {
    uint16_t job_id;
    uart_linetracer_route_t route_id;
    uart_linetracer_state_t state;
    uart_linetracer_load_state_t load_state;
    uint8_t sensor_flags;
    uint8_t error_code;
} comm_tx_observed_state_t;

typedef struct {
    uint32_t uptime_ms;
    uint16_t job_id;
    uart_linetracer_route_t route_id;
    uart_linetracer_state_t state;
    uart_linetracer_load_state_t load_state;
    uint8_t sensor_flags;
    uint8_t error_code;
} comm_tx_heartbeat_t;

void CommTxLogic_Init(comm_tx_logic_t* context);
void CommTxLogic_InitObservedState(comm_tx_observed_state_t* state);
void CommTxLogic_ObserveEvent(comm_tx_observed_state_t* state, const app_tx_event_t* event);
void CommTxLogic_ObserveSensor(comm_tx_observed_state_t* state, const app_sensor_snapshot_t* snapshot);
void CommTxLogic_ObserveControl(comm_tx_observed_state_t* state, const app_control_snapshot_t* snapshot);
void CommTxLogic_MakeHeartbeat(const comm_tx_observed_state_t* state, uint32_t uptime_ms,
                               uint8_t local_error_code, comm_tx_heartbeat_t* heartbeat);

uart_codec_result_t CommTxLogic_EncodeEvent(comm_tx_logic_t* context, const app_tx_event_t* event,
                                            uint8_t* output, size_t output_capacity, size_t* output_length);

uart_codec_result_t CommTxLogic_EncodeHeartbeat(comm_tx_logic_t* context, const comm_tx_heartbeat_t* heartbeat,
                                                uint8_t* output, size_t output_capacity, size_t* output_length);

#ifdef __cplusplus
}
#endif

#endif /* COMM_TX_LOGIC_H */
