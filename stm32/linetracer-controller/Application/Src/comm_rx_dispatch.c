#include "comm_rx_dispatch.h"

#include <stddef.h>
#include <string.h>

#include "comm_rx_config.h"
#include "logistics/contracts/uart_crc16.h"

static void CommRxDispatch_ClearEffects(comm_rx_dispatch_effects_t* effects) {
    if (effects != NULL) {
        memset(effects, 0, sizeof(*effects));
    }
}

static void CommRxDispatch_ReportHealth(const comm_rx_dispatch_port_t* port, app_health_event_type_t type,
                                        uint32_t detail, uint32_t now_ms) {
    if (port != NULL && port->report_health != NULL) {
        port->report_health(port->context, type, detail, now_ms);
    }
}

static uint8_t CommRxDispatch_TimeReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1U : 0U;
}

static uint8_t CommRxDispatch_ScheduleResponse(comm_rx_dispatch_t* dispatcher, const app_tx_event_t* event,
                                               uint32_t now_ms) {
    uint32_t index;

    for (index = 0U; index < COMM_RX_DISPATCH_PENDING_CAPACITY; ++index) {
        comm_rx_pending_response_t* pending = &dispatcher->pending_responses[index];

        if (pending->active == 0U) {
            pending->event = *event;
            pending->event.retry_count = 0U;
            pending->retry_at_ms = now_ms + COMM_RX_DISPATCH_RESPONSE_RETRY_DELAY_MS;
            pending->active = 1U;
            return 1U;
        }
    }

    return 0U;
}

static uint8_t CommRxDispatch_PublishResponse(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port,
                                              const uart_frame_t* frame, uart_status_t status, uart_error_t error,
                                              uint32_t now_ms, comm_rx_dispatch_effects_t* effects) {
    app_tx_event_t event = { 0 };

    event.type = APP_TX_EVENT_COMMAND_ACK;
    event.created_at_ms = now_ms;
    event.original_payload_crc = uart_crc16_ccitt(frame->payload, frame->length);
    event.request_sequence = frame->sequence;
    event.original_command = frame->command;
    event.original_payload_length = frame->length;
    event.status = (uint8_t)status;
    event.error_code = (uint8_t)error;

    if (port != NULL && port->put_response != NULL && port->put_response(port->context, &event) != 0U) {
        ++effects->local_responses;
        return 1U;
    }

    if (CommRxDispatch_ScheduleResponse(dispatcher, &event, now_ms) != 0U) {
        return 1U;
    }

    ++effects->queue_drops;
    CommRxDispatch_ReportHealth(port, APP_HEALTH_EVENT_QUEUE_FULL,
                                COMM_RX_HEALTH_TX_RESPONSE_QUEUE_FULL | event.request_sequence, now_ms);
    return 0U;
}

static void CommRxDispatch_ReportQueueFull(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port,
                                           uint32_t detail, const uart_frame_t* frame, uint32_t now_ms,
                                           comm_rx_dispatch_effects_t* effects) {
    ++effects->queue_drops;
    CommRxDispatch_ReportHealth(port, APP_HEALTH_EVENT_QUEUE_FULL, detail, now_ms);
    (void)CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_BUSY, UART_ERROR_BUSY, now_ms, effects);
}

void CommRxDispatch_Init(comm_rx_dispatch_t* dispatcher) {
    if (dispatcher == NULL) {
        return;
    }

    memset(dispatcher, 0, sizeof(*dispatcher));
    CommRxLogic_Init(&dispatcher->sequence_history);
}

void CommRxDispatch_Frame(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port,
                          const uart_frame_t* frame, uint32_t now_ms, comm_rx_dispatch_effects_t* effects) {
    comm_rx_decoded_command_t decoded;
    comm_rx_decode_result_t result;

    CommRxDispatch_ClearEffects(effects);
    if (dispatcher == NULL || port == NULL || frame == NULL || effects == NULL) {
        return;
    }

    result = CommRxLogic_DecodeFrame(&dispatcher->sequence_history, frame, now_ms, &decoded);
    switch (result) {
        case COMM_RX_DECODE_ACCEPTED:
            break;

        case COMM_RX_DECODE_DUPLICATE:
            ++effects->duplicate_sequences;
            (void)CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_NACK, UART_ERROR_SEQUENCE, now_ms,
                                                 effects);
            return;

        case COMM_RX_DECODE_INVALID_FRAME:
        case COMM_RX_DECODE_INVALID_PAYLOAD:
            ++effects->invalid_payloads;
            (void)CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD,
                                                 now_ms, effects);
            return;

        case COMM_RX_DECODE_UNSUPPORTED_COMMAND:
        default:
            ++effects->unsupported_commands;
            (void)CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_NACK,
                                                 UART_ERROR_UNSUPPORTED_COMMAND, now_ms, effects);
            return;
    }

    if (decoded.destination == COMM_RX_DESTINATION_CONTROL) {
        if (port->put_control == NULL || port->put_control(port->context, &decoded.control_command) == 0U) {
            CommRxDispatch_ReportQueueFull(dispatcher, port, COMM_RX_HEALTH_CONTROL_QUEUE_FULL, frame, now_ms, effects);
            return;
        }
        ++effects->control_commands;
    } else if (decoded.destination == COMM_RX_DESTINATION_SAFETY) {
        uint8_t queue_priority = 0U;

        if (decoded.safety_event.type == APP_SAFETY_EVENT_EMERGENCY_STOP) {
            queue_priority = APP_TX_PRIORITY_SAFETY;
        }

        if (port->put_safety == NULL || port->put_safety(port->context, &decoded.safety_event, queue_priority) == 0U) {
            CommRxDispatch_ReportQueueFull(dispatcher, port, COMM_RX_HEALTH_SAFETY_QUEUE_FULL, frame, now_ms, effects);
            return;
        }
        ++effects->safety_commands;
    } else if (decoded.destination == COMM_RX_DESTINATION_LOCAL_ACK) {
        if (CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_ACK, UART_ERROR_NONE, now_ms,
                                           effects) == 0U) {
            return;
        }
    } else {
        ++effects->unsupported_commands;
        (void)CommRxDispatch_PublishResponse(dispatcher, port, frame, UART_STATUS_NACK, UART_ERROR_UNSUPPORTED_COMMAND,
                                             now_ms, effects);
        return;
    }

    CommRxLogic_CommitSequence(&dispatcher->sequence_history, frame->sequence, now_ms);
}

void CommRxDispatch_ProcessPending(comm_rx_dispatch_t* dispatcher, const comm_rx_dispatch_port_t* port, uint32_t now_ms,
                                   comm_rx_dispatch_effects_t* effects) {
    uint32_t index;

    CommRxDispatch_ClearEffects(effects);
    if (dispatcher == NULL || port == NULL || effects == NULL) {
        return;
    }

    for (index = 0U; index < COMM_RX_DISPATCH_PENDING_CAPACITY; ++index) {
        comm_rx_pending_response_t* pending = &dispatcher->pending_responses[index];

        if (pending->active == 0U || CommRxDispatch_TimeReached(now_ms, pending->retry_at_ms) == 0U) {
            continue;
        }

        ++pending->event.retry_count;
        ++effects->response_retries;
        if (port->put_response != NULL && port->put_response(port->context, &pending->event) != 0U) {
            ++effects->local_responses;
            pending->active = 0U;
            continue;
        }

        if (pending->event.retry_count >= COMM_RX_DISPATCH_RESPONSE_MAX_RETRIES) {
            ++effects->queue_drops;
            ++effects->response_timeouts;
            pending->active = 0U;
            CommRxDispatch_ReportHealth(port, APP_HEALTH_EVENT_UART_TX_TIMEOUT,
                                        COMM_RX_HEALTH_TX_RESPONSE_QUEUE_FULL | pending->event.request_sequence,
                                        now_ms);
            continue;
        }

        pending->retry_at_ms = now_ms + COMM_RX_DISPATCH_RESPONSE_RETRY_DELAY_MS;
    }
}

uint32_t CommRxDispatch_PendingCount(const comm_rx_dispatch_t* dispatcher) {
    uint32_t count = 0U;
    uint32_t index;

    if (dispatcher == NULL) {
        return 0U;
    }

    for (index = 0U; index < COMM_RX_DISPATCH_PENDING_CAPACITY; ++index) {
        if (dispatcher->pending_responses[index].active != 0U) {
            ++count;
        }
    }

    return count;
}
