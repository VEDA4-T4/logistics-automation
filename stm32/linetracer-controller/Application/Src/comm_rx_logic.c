#include "comm_rx_logic.h"

#include <stddef.h>
#include <string.h>

#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/contracts/uart_crc16.h"

static uint16_t CommRxLogic_PayloadCrc(const uart_frame_t* frame) {
    return uart_crc16_ccitt(frame->payload, frame->length);
}

static void CommRxLogic_SetControlMetadata(app_control_command_t* command, const uart_frame_t* frame, uint32_t now_ms) {
    command->received_at_ms = now_ms;
    command->original_payload_crc = CommRxLogic_PayloadCrc(frame);
    command->route_id = UART_LINETRACER_ROUTE_NONE;
    command->position = UART_LINETRACER_POSITION_NONE;
    command->sequence = frame->sequence;
    command->original_command = frame->command;
    command->original_payload_length = frame->length;
}

static void CommRxLogic_SetSafetyMetadata(app_safety_event_t* event, const uart_frame_t* frame, uint32_t now_ms) {
    event->occurred_at_ms = now_ms;
    event->original_payload_crc = CommRxLogic_PayloadCrc(frame);
    event->source_task = APP_TASK_COMM_RX;
    event->error_code = UART_ERROR_NONE;
    event->request_sequence = frame->sequence;
    event->original_command = frame->command;
    event->original_payload_length = frame->length;
}

void CommRxLogic_Init(comm_rx_sequence_history_t* history) {
    if (history == NULL) {
        return;
    }

    memset(history, 0, sizeof(*history));
}

static uint8_t CommRxLogic_SequenceIsValid(const comm_rx_sequence_history_t* history, uint8_t sequence) {
    uint32_t byte_index = (uint32_t)sequence / 8U;
    uint32_t bit_index = (uint32_t)sequence % 8U;

    if (history == NULL) {
        return 0U;
    }

    return ((history->valid[byte_index] & (uint8_t)(1U << bit_index)) != 0U) ? 1U : 0U;
}

uint8_t CommRxLogic_IsDuplicate(const comm_rx_sequence_history_t* history, uint8_t sequence, uint32_t now_ms) {
    if (CommRxLogic_SequenceIsValid(history, sequence) == 0U) {
        return 0U;
    }

    return ((uint32_t)(now_ms - history->accepted_at_ms[sequence]) <= COMM_RX_SEQUENCE_REPLAY_WINDOW_MS) ? 1U : 0U;
}

void CommRxLogic_CommitSequence(comm_rx_sequence_history_t* history, uint8_t sequence, uint32_t now_ms) {
    uint32_t byte_index = (uint32_t)sequence / 8U;
    uint32_t bit_index = (uint32_t)sequence % 8U;

    if (history == NULL) {
        return;
    }

    history->accepted_at_ms[sequence] = now_ms;
    history->valid[byte_index] |= (uint8_t)(1U << bit_index);
}

void CommRxLogic_LinkInit(comm_rx_link_monitor_t* monitor, uint32_t now_ms) {
    if (monitor == NULL) {
        return;
    }

    monitor->last_raw_activity_ms = now_ms;
    monitor->last_valid_frame_ms = now_ms;
    monitor->timeout_reported = 0U;
}

void CommRxLogic_LinkRecordRaw(comm_rx_link_monitor_t* monitor, uint32_t now_ms) {
    if (monitor != NULL) {
        monitor->last_raw_activity_ms = now_ms;
    }
}

void CommRxLogic_LinkRecordValidFrame(comm_rx_link_monitor_t* monitor, uint32_t now_ms) {
    if (monitor == NULL) {
        return;
    }

    monitor->last_valid_frame_ms = now_ms;
    monitor->timeout_reported = 0U;
}

uint8_t CommRxLogic_LinkCheckTimeout(comm_rx_link_monitor_t* monitor, uint32_t now_ms, uint32_t timeout_ms) {
    if (monitor == NULL || monitor->timeout_reported != 0U || timeout_ms == 0U) {
        return 0U;
    }

    if ((uint32_t)(now_ms - monitor->last_valid_frame_ms) < timeout_ms) {
        return 0U;
    }

    monitor->timeout_reported = 1U;
    return 1U;
}

static comm_rx_decode_result_t CommRxLogic_DecodeCommonCommand(const uart_frame_t* frame, uint32_t now_ms,
                                                               comm_rx_decoded_command_t* decoded) {
    if (UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(frame->command, frame->length) == 0U) {
        return COMM_RX_DECODE_INVALID_PAYLOAD;
    }

    switch (frame->command) {
        case UART_CMD_PING:
            decoded->destination = COMM_RX_DESTINATION_LOCAL_ACK;
            return COMM_RX_DECODE_ACCEPTED;

        case UART_CMD_GET_STATUS:
            decoded->destination = COMM_RX_DESTINATION_CONTROL;
            decoded->control_command.type = APP_CONTROL_COMMAND_STATUS_REQUEST;
            CommRxLogic_SetControlMetadata(&decoded->control_command, frame, now_ms);
            return COMM_RX_DECODE_ACCEPTED;

        case UART_CMD_RESET_DEVICE:
            decoded->destination = COMM_RX_DESTINATION_SAFETY;
            decoded->safety_event.type = APP_SAFETY_EVENT_RESET_REQUEST;
            decoded->safety_event.reason = LINETRACER_STOP_REASON_NONE;
            decoded->safety_event.active = 0U;
            CommRxLogic_SetSafetyMetadata(&decoded->safety_event, frame, now_ms);
            return COMM_RX_DECODE_ACCEPTED;

        case UART_CMD_EMERGENCY_STOP:
            decoded->destination = COMM_RX_DESTINATION_SAFETY;
            decoded->safety_event.type = APP_SAFETY_EVENT_EMERGENCY_STOP;
            decoded->safety_event.reason = LINETRACER_STOP_REASON_EMERGENCY;
            decoded->safety_event.active = 1U;
            CommRxLogic_SetSafetyMetadata(&decoded->safety_event, frame, now_ms);
            return COMM_RX_DECODE_ACCEPTED;

        default:
            return COMM_RX_DECODE_UNSUPPORTED_COMMAND;
    }
}

static comm_rx_decode_result_t CommRxLogic_DecodeLineTracerCommand(const uart_frame_t* frame, uint32_t now_ms,
                                                                   comm_rx_decoded_command_t* decoded) {
    app_control_command_t* command = &decoded->control_command;

    if (UART_IS_VALID_LINETRACER_COMMAND(frame->command) == 0U) {
        return COMM_RX_DECODE_UNSUPPORTED_COMMAND;
    }

    if (UART_IS_VALID_LINETRACER_PAYLOAD(frame->command, frame->payload, frame->length) == 0U) {
        return COMM_RX_DECODE_INVALID_PAYLOAD;
    }

    if (frame->command == UART_CMD_LINETRACER_RESET_SYSTEM) {
        decoded->destination = COMM_RX_DESTINATION_SAFETY;
        decoded->safety_event.type = APP_SAFETY_EVENT_RESET_REQUEST;
        decoded->safety_event.reason = LINETRACER_STOP_REASON_NONE;
        decoded->safety_event.active = 0U;
        CommRxLogic_SetSafetyMetadata(&decoded->safety_event, frame, now_ms);
        return COMM_RX_DECODE_ACCEPTED;
    }

    decoded->destination = COMM_RX_DESTINATION_CONTROL;
    CommRxLogic_SetControlMetadata(command, frame, now_ms);

    switch (frame->command) {
        case UART_CMD_LINETRACER_ASSIGN_ROUTE:
            command->type = APP_CONTROL_COMMAND_ASSIGN_ROUTE;
            command->job_id = uart_linetracer_start_job_id(frame->payload);
            command->route_id = (uart_linetracer_route_t)frame->payload[UART_LINETRACER_START_ROUTE_ID_INDEX];
            break;

        case UART_CMD_LINETRACER_STOP_DRIVE:
            command->type = APP_CONTROL_COMMAND_STOP_DRIVE;
            command->job_id = uart_linetracer_stop_job_id(frame->payload);
            break;

        case UART_CMD_LINETRACER_STATUS_REQUEST:
            command->type = APP_CONTROL_COMMAND_STATUS_REQUEST;
            break;

        case UART_CMD_LINETRACER_SET_CURRENT_POSITION:
            command->type = APP_CONTROL_COMMAND_SET_CURRENT_POSITION;
            command->position = (uart_linetracer_position_t)frame->payload[UART_LINETRACER_SET_POSITION_ID_INDEX];
            break;

        case UART_CMD_LINETRACER_RESUME_DRIVE:
            command->type = APP_CONTROL_COMMAND_RESUME_DRIVE;
            break;

        case UART_CMD_LINETRACER_MANUAL_UNLOAD:
            command->type = APP_CONTROL_COMMAND_MANUAL_UNLOAD;
            break;

        default:
            return COMM_RX_DECODE_UNSUPPORTED_COMMAND;
    }

    return COMM_RX_DECODE_ACCEPTED;
}

comm_rx_decode_result_t CommRxLogic_DecodeFrame(const comm_rx_sequence_history_t* history, const uart_frame_t* frame,
                                                uint32_t now_ms, comm_rx_decoded_command_t* decoded) {
    if (history == NULL || frame == NULL || decoded == NULL) {
        return COMM_RX_DECODE_INVALID_FRAME;
    }

    memset(decoded, 0, sizeof(*decoded));
    if (frame->version != UART_PROTOCOL_VERSION || UART_IS_VALID_PAYLOAD_LENGTH(frame->length) == 0U) {
        return COMM_RX_DECODE_INVALID_FRAME;
    }

    if (CommRxLogic_IsDuplicate(history, frame->sequence, now_ms) != 0U) {
        return COMM_RX_DECODE_DUPLICATE;
    }

    if (frame->command >= UART_CMD_LINETRACER_MIN && frame->command <= UART_CMD_LINETRACER_MAX) {
        return CommRxLogic_DecodeLineTracerCommand(frame, now_ms, decoded);
    }

    return CommRxLogic_DecodeCommonCommand(frame, now_ms, decoded);
}
