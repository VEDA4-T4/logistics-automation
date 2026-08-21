#include "comm_tx_logic.h"

#include <string.h>

#include "sensor_config.h"

static uint8_t CommTxLogic_JobRoutePairIsValid(uint16_t job_id, uart_linetracer_route_t route_id, uint8_t allow_none) {
    if (allow_none != 0U && job_id == UART_LINETRACER_JOB_ID_NONE && route_id == UART_LINETRACER_ROUTE_NONE) {
        return 1U;
    }

    return (uart_linetracer_job_id_is_valid(job_id) != 0U && uart_linetracer_route_is_valid(route_id) != 0U) ? 1U : 0U;
}

static void CommTxLogic_WriteUint16(uint8_t* payload, uint32_t low_index, uint16_t value) {
    payload[low_index] = (uint8_t)(value & 0xFFU);
    payload[low_index + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void CommTxLogic_WriteUint32(uint8_t* payload, uint32_t first_index, uint32_t value) {
    payload[first_index] = (uint8_t)(value & 0xFFU);
    payload[first_index + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    payload[first_index + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    payload[first_index + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void CommTxLogic_InitFrame(uart_frame_t* frame, uint8_t sequence, uint8_t command) {
    (void)memset(frame, 0, sizeof(*frame));
    frame->version = UART_PROTOCOL_VERSION;
    frame->sequence = sequence;
    frame->command = command;
}

static uart_codec_result_t CommTxLogic_EncodeAsyncFrame(comm_tx_logic_t* context, uart_frame_t* frame, uint8_t* output,
                                                        size_t output_capacity, size_t* output_length) {
    uart_codec_result_t result;

    frame->sequence = context->next_sequence;
    result = uart_encode_frame(frame, output, output_capacity, output_length);
    if (result == UART_CODEC_OK) {
        ++context->next_sequence;
    }
    return result;
}

static uart_codec_result_t CommTxLogic_BuildAck(const app_tx_event_t* event, uart_frame_t* frame) {
    CommTxLogic_InitFrame(frame, event->request_sequence, UART_CMD_ACK);
    frame->length = UART_ACK_PAYLOAD_SIZE;
    frame->payload[UART_ACK_STATUS_INDEX] = event->status;
    frame->payload[UART_ACK_COMMAND_INDEX] = event->original_command;
    frame->payload[UART_ACK_LENGTH_INDEX] = event->original_payload_length;
    frame->payload[UART_ACK_CRC_LOW_INDEX] = UART_CRC_LOW_BYTE(event->original_payload_crc);
    frame->payload[UART_ACK_CRC_HIGH_INDEX] = UART_CRC_HIGH_BYTE(event->original_payload_crc);
    return UART_CODEC_OK;
}

static uart_codec_result_t CommTxLogic_BuildStatus(const app_tx_event_t* event, uart_frame_t* frame) {
    if (uart_linetracer_status_route_is_valid(event->route_id) == 0U ||
        uart_linetracer_state_is_valid(event->state) == 0U ||
        uart_linetracer_load_state_is_valid(event->load_state) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    CommTxLogic_InitFrame(frame, event->request_sequence, UART_CMD_RESPONSE);
    frame->length = UART_LINETRACER_STATUS_PAYLOAD_SIZE;
    frame->payload[UART_RESPONSE_STATUS_INDEX] = event->status;
    frame->payload[UART_RESPONSE_COMMAND_INDEX] = event->original_command;
    frame->payload[UART_RESPONSE_ERROR_INDEX] = event->error_code;
    frame->payload[UART_LINETRACER_STATUS_STATE_INDEX] = (uint8_t)event->state;
    CommTxLogic_WriteUint16(frame->payload, UART_LINETRACER_STATUS_JOB_ID_LOW_INDEX, event->job_id);
    frame->payload[UART_LINETRACER_STATUS_ROUTE_ID_INDEX] = (uint8_t)event->route_id;
    frame->payload[UART_LINETRACER_STATUS_LOAD_STATE_INDEX] = (uint8_t)event->load_state;
    return UART_CODEC_OK;
}

static uart_codec_result_t CommTxLogic_BuildJobEvent(const app_tx_event_t* event, uint8_t event_id,
                                                     uart_frame_t* frame) {
    if (CommTxLogic_JobRoutePairIsValid(event->job_id, event->route_id, 0U) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    CommTxLogic_InitFrame(frame, 0U, UART_CMD_EVENT);
    frame->length = UART_LINETRACER_JOB_EVENT_PAYLOAD_SIZE;
    frame->payload[UART_EVENT_ID_INDEX] = event_id;
    CommTxLogic_WriteUint16(frame->payload, UART_LINETRACER_EVENT_JOB_ID_LOW_INDEX, event->job_id);
    frame->payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX] = (uint8_t)event->route_id;
    return UART_CODEC_OK;
}

static uart_codec_result_t CommTxLogic_BuildStateEvent(const app_tx_event_t* event, uart_frame_t* frame) {
    uart_codec_result_t result = CommTxLogic_BuildJobEvent(event, UART_LINETRACER_EVENT_STATE_CHANGED, frame);

    if (result != UART_CODEC_OK || uart_linetracer_state_is_valid(event->state) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }
    frame->length = UART_LINETRACER_STATE_EVENT_PAYLOAD_SIZE;
    frame->payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX] = (uint8_t)event->state;
    return UART_CODEC_OK;
}

static uart_codec_result_t CommTxLogic_BuildFaultEvent(const app_tx_event_t* event, uart_frame_t* frame) {
    if (CommTxLogic_JobRoutePairIsValid(event->job_id, event->route_id, 1U) == 0U ||
        uart_linetracer_fault_error_is_valid(event->error_code) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    CommTxLogic_InitFrame(frame, 0U, UART_CMD_EVENT);
    frame->length = UART_LINETRACER_FAULT_EVENT_PAYLOAD_SIZE;
    frame->payload[UART_EVENT_ID_INDEX] = UART_LINETRACER_EVENT_FAULT;
    CommTxLogic_WriteUint16(frame->payload, UART_LINETRACER_EVENT_JOB_ID_LOW_INDEX, event->job_id);
    frame->payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX] = (uint8_t)event->route_id;
    frame->payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX] = event->error_code;
    return UART_CODEC_OK;
}

static uart_codec_result_t CommTxLogic_BuildSensorStatusEvent(const app_tx_event_t* event, uart_frame_t* frame) {
    if (event->sensor_id == 0U || uart_sensor_state_is_valid(event->sensor_state) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    CommTxLogic_InitFrame(frame, 0U, UART_CMD_SENSOR_STATUS);
    frame->length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
    frame->payload[UART_SENSOR_ID_INDEX] = event->sensor_id;
    frame->payload[UART_SENSOR_STATE_INDEX] = event->sensor_state;
    CommTxLogic_WriteUint16(frame->payload, UART_SENSOR_DISTANCE_LOW_INDEX, event->sensor_distance_cm);
    return UART_CODEC_OK;
}

void CommTxLogic_Init(comm_tx_logic_t* context) {
    if (context != NULL) {
        context->next_sequence = 0U;
    }
}

void CommTxLogic_InitObservedState(comm_tx_observed_state_t* state) {
    if (state == NULL) {
        return;
    }

    (void)memset(state, 0, sizeof(*state));
    state->route_id = UART_LINETRACER_ROUTE_NONE;
    state->state = UART_LINETRACER_STATE_IDLE;
    state->load_state = UART_LINETRACER_LOAD_EMPTY;
    state->error_code = UART_ERROR_NONE;
}

void CommTxLogic_ObserveEvent(comm_tx_observed_state_t* state, const app_tx_event_t* event) {
    uint8_t job_route_valid;
    uint8_t job_route_none;

    if (state == NULL || event == NULL) {
        return;
    }

    if (uart_linetracer_state_is_valid(event->state) != 0U) {
        state->state = event->state;
    }
    if (uart_linetracer_load_state_is_valid(event->load_state) != 0U) {
        state->load_state = event->load_state;
    }

    job_route_valid =
        (uart_linetracer_job_id_is_valid(event->job_id) != 0U && uart_linetracer_route_is_valid(event->route_id) != 0U)
            ? 1U
            : 0U;
    job_route_none =
        (event->job_id == UART_LINETRACER_JOB_ID_NONE && event->route_id == UART_LINETRACER_ROUTE_NONE) ? 1U : 0U;

    if (event->type == APP_TX_EVENT_UNLOAD_COMPLETE) {
        state->job_id = UART_LINETRACER_JOB_ID_NONE;
        state->route_id = UART_LINETRACER_ROUTE_NONE;
        state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_ROUTE_ACTIVE);
    } else if (job_route_valid != 0U || job_route_none != 0U) {
        state->job_id = event->job_id;
        state->route_id = event->route_id;
        if (job_route_valid != 0U) {
            state->sensor_flags |= UART_LINETRACER_FLAG_ROUTE_ACTIVE;
        } else {
            state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_ROUTE_ACTIVE);
        }
    }

    if (event->type == APP_TX_EVENT_FAULT && uart_linetracer_fault_error_is_valid(event->error_code) != 0U) {
        state->error_code = event->error_code;
    } else if (event->type == APP_TX_EVENT_COMMAND_ACK && event->original_command == UART_CMD_LINETRACER_RESET_SYSTEM &&
               event->status == UART_STATUS_ACK && event->state == UART_LINETRACER_STATE_IDLE) {
        state->error_code = UART_ERROR_NONE;
    }
}

static uint8_t CommTxLogic_DistanceIsValid(uint16_t distance_mm) {
    return (distance_mm >= SENSOR_ULTRASONIC_MIN_MM && distance_mm <= SENSOR_ULTRASONIC_MAX_MM) ? 1U : 0U;
}

void CommTxLogic_ObserveSensor(comm_tx_observed_state_t* state, const app_sensor_snapshot_t* snapshot) {
    const uint16_t distances[] = { snapshot != NULL ? snapshot->ultrasonic_front_mm : 0U,
                                   snapshot != NULL ? snapshot->ultrasonic_left_mm : 0U,
                                   snapshot != NULL ? snapshot->ultrasonic_right_mm : 0U };
    uint8_t valid_count = 0U;
    uint8_t obstacle_detected = 0U;
    uint8_t all_clear = 1U;
    uint32_t index;

    if (state == NULL || snapshot == NULL) {
        return;
    }

    if (snapshot->line_state == LINETRACER_LINE_CENTERED || snapshot->line_state == LINETRACER_LINE_LEFT_ONLY ||
        snapshot->line_state == LINETRACER_LINE_RIGHT_ONLY) {
        state->sensor_flags |= UART_LINETRACER_FLAG_LINE_DETECTED;
    } else {
        state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_LINE_DETECTED);
    }

    if (uart_linetracer_load_state_is_valid(snapshot->load_state) != 0U) {
        state->load_state = snapshot->load_state;
    }
    if (snapshot->load_state != UART_LINETRACER_LOAD_EMPTY) {
        state->sensor_flags |= UART_LINETRACER_FLAG_LOAD_PRESENT;
    } else {
        state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_LOAD_PRESENT);
    }

    for (index = 0U; index < (sizeof(distances) / sizeof(distances[0])); ++index) {
        if (CommTxLogic_DistanceIsValid(distances[index]) == 0U) {
            all_clear = 0U;
            continue;
        }
        ++valid_count;
        if (distances[index] <= SENSOR_OBSTACLE_ON_MM) {
            obstacle_detected = 1U;
        }
        if (distances[index] < SENSOR_OBSTACLE_OFF_MM) {
            all_clear = 0U;
        }
    }

    if (obstacle_detected != 0U) {
        state->sensor_flags |= UART_LINETRACER_FLAG_OBSTACLE_DETECTED;
    } else if (valid_count == (sizeof(distances) / sizeof(distances[0])) && all_clear != 0U) {
        state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_OBSTACLE_DETECTED);
    }

    if ((snapshot->event_flags & (APP_SENSOR_EVENT_LINE_LOST | APP_SENSOR_EVENT_OVERLOAD)) != 0U ||
        obstacle_detected != 0U) {
        state->error_code = UART_ERROR_SENSOR;
    } else if (state->error_code == UART_ERROR_SENSOR) {
        state->error_code = UART_ERROR_NONE;
    }
}

void CommTxLogic_ObserveControl(comm_tx_observed_state_t* state, const app_control_snapshot_t* snapshot) {
    uint8_t job_route_valid;
    uint8_t job_route_none;

    if (state == NULL || snapshot == NULL) {
        return;
    }

    if (uart_linetracer_state_is_valid(snapshot->state) != 0U) {
        state->state = snapshot->state;
    }
    if (uart_linetracer_load_state_is_valid(snapshot->load_state) != 0U) {
        state->load_state = snapshot->load_state;
    }

    job_route_valid = (uart_linetracer_job_id_is_valid(snapshot->job_id) != 0U &&
                       uart_linetracer_route_is_valid(snapshot->route_id) != 0U)
                          ? 1U
                          : 0U;
    job_route_none =
        (snapshot->job_id == UART_LINETRACER_JOB_ID_NONE && snapshot->route_id == UART_LINETRACER_ROUTE_NONE) ? 1U : 0U;
    if (job_route_valid != 0U || job_route_none != 0U) {
        state->job_id = snapshot->job_id;
        state->route_id = snapshot->route_id;
        if (job_route_valid != 0U) {
            state->sensor_flags |= UART_LINETRACER_FLAG_ROUTE_ACTIVE;
        } else {
            state->sensor_flags &= (uint8_t)(~UART_LINETRACER_FLAG_ROUTE_ACTIVE);
        }
    }

    if (snapshot->error_code == UART_ERROR_NONE) {
        state->error_code = (snapshot->safety_latched != 0U) ? UART_ERROR_INTERNAL : UART_ERROR_NONE;
    } else if (uart_linetracer_fault_error_is_valid(snapshot->error_code) != 0U) {
        state->error_code = snapshot->error_code;
    } else {
        state->error_code = UART_ERROR_INTERNAL;
    }
}

void CommTxLogic_MakeHeartbeat(const comm_tx_observed_state_t* state, uint32_t uptime_ms, uint8_t local_error_code,
                               comm_tx_heartbeat_t* heartbeat) {
    if (state == NULL || heartbeat == NULL) {
        return;
    }

    (void)memset(heartbeat, 0, sizeof(*heartbeat));
    heartbeat->uptime_ms = uptime_ms;
    heartbeat->job_id = state->job_id;
    heartbeat->route_id = state->route_id;
    heartbeat->state = state->state;
    heartbeat->load_state = state->load_state;
    heartbeat->sensor_flags = state->sensor_flags;
    heartbeat->error_code = (local_error_code != UART_ERROR_NONE) ? local_error_code : state->error_code;

    if (heartbeat->error_code != UART_ERROR_NONE && uart_linetracer_fault_error_is_valid(heartbeat->error_code) == 0U) {
        heartbeat->error_code = UART_ERROR_INTERNAL;
    }
}

uart_codec_result_t CommTxLogic_EncodeEvent(comm_tx_logic_t* context, const app_tx_event_t* event, uint8_t* output,
                                            size_t output_capacity, size_t* output_length) {
    uart_frame_t frame;
    uart_codec_result_t result;
    uint8_t asynchronous = 1U;

    if (context == NULL || event == NULL || output == NULL || output_length == NULL) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    switch (event->type) {
        case APP_TX_EVENT_COMMAND_ACK:
            result = CommTxLogic_BuildAck(event, &frame);
            asynchronous = 0U;
            break;

        case APP_TX_EVENT_STATUS:
            result = CommTxLogic_BuildStatus(event, &frame);
            asynchronous = 0U;
            break;

        case APP_TX_EVENT_STARTED:
            result = CommTxLogic_BuildJobEvent(event, UART_LINETRACER_EVENT_STARTED, &frame);
            break;

        case APP_TX_EVENT_ARRIVED:
            result = CommTxLogic_BuildJobEvent(event, UART_LINETRACER_EVENT_ARRIVED, &frame);
            break;

        case APP_TX_EVENT_LOAD_DETECTED:
            result = CommTxLogic_BuildJobEvent(event, UART_LINETRACER_EVENT_LOAD_DETECTED, &frame);
            break;

        case APP_TX_EVENT_UNLOAD_COMPLETE:
            result = CommTxLogic_BuildJobEvent(event, UART_LINETRACER_EVENT_UNLOAD_COMPLETE, &frame);
            break;

        case APP_TX_EVENT_STATE_CHANGED:
            result = CommTxLogic_BuildStateEvent(event, &frame);
            break;

        case APP_TX_EVENT_SENSOR_STATUS:
            result = CommTxLogic_BuildSensorStatusEvent(event, &frame);
            break;

        case APP_TX_EVENT_FAULT:
            result = CommTxLogic_BuildFaultEvent(event, &frame);
            break;

        case APP_TX_EVENT_HEARTBEAT:
        case APP_TX_EVENT_NONE:
        default:
            return UART_CODEC_INVALID_ARGUMENT;
    }

    if (result != UART_CODEC_OK) {
        return result;
    }

    if (asynchronous != 0U) {
        return CommTxLogic_EncodeAsyncFrame(context, &frame, output, output_capacity, output_length);
    }
    return uart_encode_frame(&frame, output, output_capacity, output_length);
}

uart_codec_result_t CommTxLogic_EncodeHeartbeat(comm_tx_logic_t* context, const comm_tx_heartbeat_t* heartbeat,
                                                uint8_t* output, size_t output_capacity, size_t* output_length) {
    uart_frame_t frame;

    if (context == NULL || heartbeat == NULL || output == NULL || output_length == NULL ||
        CommTxLogic_JobRoutePairIsValid(heartbeat->job_id, heartbeat->route_id, 1U) == 0U ||
        uart_linetracer_state_is_valid(heartbeat->state) == 0U ||
        uart_linetracer_load_state_is_valid(heartbeat->load_state) == 0U ||
        (heartbeat->sensor_flags & (uint8_t)(~UART_LINETRACER_HEARTBEAT_KNOWN_FLAGS)) != 0U ||
        (heartbeat->error_code != UART_ERROR_NONE &&
         uart_linetracer_fault_error_is_valid(heartbeat->error_code) == 0U)) {
        return UART_CODEC_INVALID_ARGUMENT;
    }

    CommTxLogic_InitFrame(&frame, 0U, UART_CMD_EVENT);
    frame.length = UART_LINETRACER_HEARTBEAT_PAYLOAD_SIZE;
    frame.payload[UART_EVENT_ID_INDEX] = UART_LINETRACER_EVENT_HEARTBEAT;
    frame.payload[UART_LINETRACER_HEARTBEAT_STATE_INDEX] = (uint8_t)heartbeat->state;
    frame.payload[UART_LINETRACER_HEARTBEAT_ERROR_INDEX] = heartbeat->error_code;
    frame.payload[UART_LINETRACER_HEARTBEAT_FLAGS_INDEX] = heartbeat->sensor_flags;
    frame.payload[UART_LINETRACER_HEARTBEAT_LOAD_STATE_INDEX] = (uint8_t)heartbeat->load_state;
    CommTxLogic_WriteUint32(frame.payload, UART_LINETRACER_HEARTBEAT_UPTIME_0_INDEX, heartbeat->uptime_ms);
    CommTxLogic_WriteUint16(frame.payload, UART_LINETRACER_HEARTBEAT_JOB_ID_LOW_INDEX, heartbeat->job_id);
    frame.payload[UART_LINETRACER_HEARTBEAT_ROUTE_ID_INDEX] = (uint8_t)heartbeat->route_id;

    if (UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) == 0U) {
        return UART_CODEC_INVALID_ARGUMENT;
    }
    return CommTxLogic_EncodeAsyncFrame(context, &frame, output, output_capacity, output_length);
}
