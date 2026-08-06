#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "comm_tx_logic.h"
#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/contracts/uart_codec.h"
}

namespace {

uart_frame_t Decode(const std::array<std::uint8_t, UART_MAX_FRAME_SIZE>& encoded, std::size_t length) {
    uart_frame_t frame{};
    assert(uart_decode_frame(encoded.data(), length, &frame) == UART_CODEC_OK);
    return frame;
}

app_tx_event_t MakeJobEvent(app_tx_event_type_t type) {
    app_tx_event_t event{};
    event.type = type;
    event.job_id = 0x1234U;
    event.route_id = UART_LINETRACER_ROUTE_B;
    event.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    event.load_state = UART_LINETRACER_LOAD_PRESENT;
    event.status = UART_STATUS_SUCCESS;
    event.error_code = UART_ERROR_NONE;
    return event;
}

void TestAckEchoesRequestSequenceAndMetadata() {
    comm_tx_logic_t logic{};
    app_tx_event_t event{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    CommTxLogic_Init(&logic);
    event.type = APP_TX_EVENT_COMMAND_ACK;
    event.request_sequence = 0x42U;
    event.status = UART_STATUS_ACK;
    event.original_command = UART_CMD_LINETRACER_ASSIGN_ROUTE;
    event.original_payload_length = UART_LINETRACER_START_PAYLOAD_SIZE;
    event.original_payload_crc = 0xBEEFU;

    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.sequence == 0x42U);
    assert(frame.command == UART_CMD_ACK);
    assert(frame.payload[UART_ACK_STATUS_INDEX] == UART_STATUS_ACK);
    assert(frame.payload[UART_ACK_COMMAND_INDEX] == UART_CMD_LINETRACER_ASSIGN_ROUTE);
    assert(frame.payload[UART_ACK_LENGTH_INDEX] == UART_LINETRACER_START_PAYLOAD_SIZE);
    assert(frame.payload[UART_ACK_CRC_LOW_INDEX] == 0xEFU);
    assert(frame.payload[UART_ACK_CRC_HIGH_INDEX] == 0xBEU);
    assert(logic.next_sequence == 0U);
}

void TestStatusResponseContainsCurrentState() {
    comm_tx_logic_t logic{};
    auto event = MakeJobEvent(APP_TX_EVENT_STATUS);
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    event.request_sequence = 0x11U;
    event.original_command = UART_CMD_LINETRACER_STATUS_REQUEST;
    event.status = UART_STATUS_ACK;

    CommTxLogic_Init(&logic);
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.sequence == 0x11U);
    assert(frame.command == UART_CMD_RESPONSE);
    assert(frame.length == UART_LINETRACER_STATUS_PAYLOAD_SIZE);
    assert(frame.payload[UART_LINETRACER_STATUS_STATE_INDEX] == UART_LINETRACER_STATE_FOLLOWING_LINE);
    assert(uart_linetracer_read_job_id(frame.payload, UART_LINETRACER_STATUS_JOB_ID_LOW_INDEX,
                                       UART_LINETRACER_STATUS_JOB_ID_HIGH_INDEX) == 0x1234U);
    assert(frame.payload[UART_LINETRACER_STATUS_ROUTE_ID_INDEX] == UART_LINETRACER_ROUTE_B);
    assert(frame.payload[UART_LINETRACER_STATUS_LOAD_STATE_INDEX] == UART_LINETRACER_LOAD_PRESENT);
}

void TestStartedEventUsesAsyncSequence() {
    comm_tx_logic_t logic{};
    auto event = MakeJobEvent(APP_TX_EVENT_STARTED);
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    CommTxLogic_Init(&logic);
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.sequence == 0U);
    assert(frame.command == UART_CMD_EVENT);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_STARTED);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) != 0U);
    assert(logic.next_sequence == 1U);
}

void TestFaultMayBeReportedWithoutActiveJob() {
    comm_tx_logic_t logic{};
    app_tx_event_t event{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    event.type = APP_TX_EVENT_FAULT;
    event.job_id = UART_LINETRACER_JOB_ID_NONE;
    event.route_id = UART_LINETRACER_ROUTE_NONE;
    event.error_code = UART_ERROR_EMERGENCY_STOP;

    CommTxLogic_Init(&logic);
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_FAULT);
    assert(frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) != 0U);
}

void TestObstacleStateChangeEventsContainDirectionDistanceAndMotionState() {
    comm_tx_logic_t logic{};
    auto event = MakeJobEvent(APP_TX_EVENT_OBSTACLE_DETECTED);
    comm_tx_observed_state_t observed{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    event.state = UART_LINETRACER_STATE_STOPPED;
    event.obstacle_direction_mask = UART_LINETRACER_OBSTACLE_DIRECTION_FRONT | UART_LINETRACER_OBSTACLE_DIRECTION_LEFT;
    event.minimum_distance_mm = 43U;
    CommTxLogic_Init(&logic);
    CommTxLogic_InitObservedState(&observed);
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    auto frame = Decode(encoded, length);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_OBSTACLE_DETECTED);
    assert(frame.payload[UART_LINETRACER_OBSTACLE_EVENT_DIRECTION_INDEX] == event.obstacle_direction_mask);
    assert(uart_linetracer_read_job_id(frame.payload, UART_LINETRACER_OBSTACLE_EVENT_DISTANCE_LOW_INDEX,
                                       UART_LINETRACER_OBSTACLE_EVENT_DISTANCE_HIGH_INDEX) == 43U);
    assert(frame.payload[UART_LINETRACER_OBSTACLE_EVENT_STATE_INDEX] == UART_LINETRACER_STATE_STOPPED);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) != 0U);
    CommTxLogic_ObserveEvent(&observed, &event);
    CommTxLogic_ObserveEvent(&observed, &event);
    assert((observed.sensor_flags & UART_LINETRACER_FLAG_OBSTACLE_DETECTED) != 0U);
    assert(observed.error_code == UART_ERROR_SENSOR);

    event.type = APP_TX_EVENT_OBSTACLE_CLEARED;
    event.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    event.obstacle_direction_mask = 0U;
    event.minimum_distance_mm = 0U;
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    frame = Decode(encoded, length);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_OBSTACLE_CLEARED);
    assert(frame.payload[UART_LINETRACER_OBSTACLE_EVENT_DIRECTION_INDEX] == 0U);
    assert(frame.payload[UART_LINETRACER_OBSTACLE_EVENT_STATE_INDEX] == UART_LINETRACER_STATE_FOLLOWING_LINE);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) != 0U);
    CommTxLogic_ObserveEvent(&observed, &event);
    CommTxLogic_ObserveEvent(&observed, &event);
    assert((observed.sensor_flags & UART_LINETRACER_FLAG_OBSTACLE_DETECTED) == 0U);
    assert(observed.error_code == UART_ERROR_NONE);
}

void TestHeartbeatContainsUptimeStateSensorsAndError() {
    comm_tx_logic_t logic{};
    comm_tx_heartbeat_t heartbeat{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    heartbeat.uptime_ms = 0x78563412UL;
    heartbeat.job_id = 0x1234U;
    heartbeat.route_id = UART_LINETRACER_ROUTE_C;
    heartbeat.state = UART_LINETRACER_STATE_EMERGENCY_STOP;
    heartbeat.load_state = UART_LINETRACER_LOAD_PRESENT;
    heartbeat.sensor_flags =
        UART_LINETRACER_FLAG_LINE_DETECTED | UART_LINETRACER_FLAG_LOAD_PRESENT | UART_LINETRACER_FLAG_ROUTE_ACTIVE;
    heartbeat.error_code = UART_ERROR_EMERGENCY_STOP;

    CommTxLogic_Init(&logic);
    assert(CommTxLogic_EncodeHeartbeat(&logic, &heartbeat, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.command == UART_CMD_EVENT);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_HEARTBEAT);
    assert(frame.payload[UART_LINETRACER_HEARTBEAT_STATE_INDEX] == UART_LINETRACER_STATE_EMERGENCY_STOP);
    assert(frame.payload[UART_LINETRACER_HEARTBEAT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    assert(frame.payload[UART_LINETRACER_HEARTBEAT_FLAGS_INDEX] == heartbeat.sensor_flags);
    assert(frame.payload[UART_LINETRACER_HEARTBEAT_LOAD_STATE_INDEX] == UART_LINETRACER_LOAD_PRESENT);
    assert(uart_linetracer_heartbeat_uptime_ms(frame.payload) == 0x78563412UL);
    assert(uart_linetracer_read_job_id(frame.payload, UART_LINETRACER_HEARTBEAT_JOB_ID_LOW_INDEX,
                                       UART_LINETRACER_HEARTBEAT_JOB_ID_HIGH_INDEX) == 0x1234U);
    assert(frame.payload[UART_LINETRACER_HEARTBEAT_ROUTE_ID_INDEX] == UART_LINETRACER_ROUTE_C);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) != 0U);
}

void TestHeartbeatAllowsIdleAndRejectsUnknownFlags() {
    comm_tx_logic_t logic{};
    comm_tx_heartbeat_t heartbeat{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    heartbeat.state = UART_LINETRACER_STATE_IDLE;
    heartbeat.load_state = UART_LINETRACER_LOAD_EMPTY;
    heartbeat.error_code = UART_ERROR_NONE;

    CommTxLogic_Init(&logic);
    assert(CommTxLogic_EncodeHeartbeat(&logic, &heartbeat, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);

    heartbeat.sensor_flags = 0x80U;
    assert(CommTxLogic_EncodeHeartbeat(&logic, &heartbeat, encoded.data(), encoded.size(), &length) ==
           UART_CODEC_INVALID_ARGUMENT);
}

void TestAsyncSequenceWraps() {
    comm_tx_logic_t logic{};
    auto event = MakeJobEvent(APP_TX_EVENT_ARRIVED);
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length{};

    logic.next_sequence = 0xFFU;
    assert(CommTxLogic_EncodeEvent(&logic, &event, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    const auto frame = Decode(encoded, length);
    assert(frame.sequence == 0xFFU);
    assert(logic.next_sequence == 0U);
}

void TestEmergencyEventPriorityPrecedesRegularEvents() {
    assert(APP_TX_PRIORITY_SAFETY > APP_TX_PRIORITY_EVENT);
    assert(APP_TX_PRIORITY_EVENT > APP_TX_PRIORITY_TELEMETRY);
    assert(app_tx_event_is_emergency(APP_TX_EVENT_FAULT) != 0U);
    assert(app_tx_event_is_emergency(APP_TX_EVENT_OBSTACLE_DETECTED) != 0U);
    assert(app_tx_event_is_emergency(APP_TX_EVENT_OBSTACLE_CLEARED) != 0U);
    assert(app_tx_event_is_emergency(APP_TX_EVENT_STARTED) == 0U);
    assert(app_tx_event_priority(APP_TX_EVENT_FAULT) == APP_TX_PRIORITY_SAFETY);
    assert(app_tx_event_priority(APP_TX_EVENT_OBSTACLE_DETECTED) == APP_TX_PRIORITY_SAFETY);
    assert(app_tx_event_priority(APP_TX_EVENT_OBSTACLE_CLEARED) == APP_TX_PRIORITY_SAFETY);
    assert(app_tx_event_priority(APP_TX_EVENT_STARTED) == APP_TX_PRIORITY_EVENT);
    assert(app_tx_event_priority(APP_TX_EVENT_ARRIVED) == APP_TX_PRIORITY_EVENT);
}

void TestControlSnapshotOverridesBestEffortObservedState() {
    comm_tx_observed_state_t state{};
    app_control_snapshot_t snapshot{};
    comm_tx_heartbeat_t heartbeat{};

    CommTxLogic_InitObservedState(&state);
    snapshot.job_id = 0x1234U;
    snapshot.route_id = UART_LINETRACER_ROUTE_B;
    snapshot.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    snapshot.load_state = UART_LINETRACER_LOAD_PRESENT;
    snapshot.error_code = UART_ERROR_EMERGENCY_STOP;
    snapshot.safety_latched = 1U;

    CommTxLogic_ObserveControl(&state, &snapshot);
    CommTxLogic_MakeHeartbeat(&state, 1000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.job_id == 0x1234U);
    assert(heartbeat.route_id == UART_LINETRACER_ROUTE_B);
    assert(heartbeat.state == UART_LINETRACER_STATE_FOLLOWING_LINE);
    assert(heartbeat.load_state == UART_LINETRACER_LOAD_PRESENT);
    assert(heartbeat.error_code == UART_ERROR_EMERGENCY_STOP);
    assert((state.sensor_flags & UART_LINETRACER_FLAG_ROUTE_ACTIVE) != 0U);

    snapshot.job_id = UART_LINETRACER_JOB_ID_NONE;
    snapshot.route_id = UART_LINETRACER_ROUTE_NONE;
    snapshot.state = UART_LINETRACER_STATE_IDLE;
    snapshot.load_state = UART_LINETRACER_LOAD_EMPTY;
    snapshot.error_code = UART_ERROR_NONE;
    snapshot.safety_latched = 0U;
    CommTxLogic_ObserveControl(&state, &snapshot);
    CommTxLogic_MakeHeartbeat(&state, 2000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.error_code == UART_ERROR_NONE);
    assert((state.sensor_flags & UART_LINETRACER_FLAG_ROUTE_ACTIVE) == 0U);
}

void TestExistingSensorSnapshotUpdatesBestEffortHeartbeatFlags() {
    comm_tx_observed_state_t state{};
    app_sensor_snapshot_t snapshot{};
    comm_tx_heartbeat_t heartbeat{};

    CommTxLogic_InitObservedState(&state);
    snapshot.line_state = LINETRACER_LINE_CENTERED;
    snapshot.load_state = UART_LINETRACER_LOAD_PRESENT;
    snapshot.ultrasonic_front_mm = 43U;
    snapshot.ultrasonic_rear_mm = 500U;
    snapshot.ultrasonic_left_mm = 500U;
    snapshot.ultrasonic_right_mm = 500U;
    snapshot.event_flags = APP_SENSOR_EVENT_OBSTACLE;

    CommTxLogic_ObserveSensor(&state, &snapshot);
    CommTxLogic_MakeHeartbeat(&state, 5000U, UART_ERROR_NONE, &heartbeat);
    assert((heartbeat.sensor_flags & UART_LINETRACER_FLAG_LINE_DETECTED) != 0U);
    assert((heartbeat.sensor_flags & UART_LINETRACER_FLAG_OBSTACLE_DETECTED) != 0U);
    assert((heartbeat.sensor_flags & UART_LINETRACER_FLAG_LOAD_PRESENT) != 0U);
    assert(heartbeat.load_state == UART_LINETRACER_LOAD_PRESENT);
    assert(heartbeat.error_code == UART_ERROR_SENSOR);
}

void TestObservedFaultPersistsUntilSuccessfulReset() {
    comm_tx_observed_state_t state{};
    app_tx_event_t fault{};
    app_tx_event_t reset{};
    comm_tx_heartbeat_t heartbeat{};

    CommTxLogic_InitObservedState(&state);
    fault.type = APP_TX_EVENT_FAULT;
    fault.state = UART_LINETRACER_STATE_FAULT;
    fault.load_state = UART_LINETRACER_LOAD_EMPTY;
    fault.error_code = UART_ERROR_SENSOR;
    CommTxLogic_ObserveEvent(&state, &fault);
    CommTxLogic_MakeHeartbeat(&state, 1000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.state == UART_LINETRACER_STATE_FAULT);
    assert(heartbeat.error_code == UART_ERROR_SENSOR);

    CommTxLogic_MakeHeartbeat(&state, 2000U, UART_ERROR_TIMEOUT, &heartbeat);
    assert(heartbeat.error_code == UART_ERROR_TIMEOUT);

    reset.type = APP_TX_EVENT_COMMAND_ACK;
    reset.original_command = UART_CMD_LINETRACER_RESET_SYSTEM;
    reset.status = UART_STATUS_ACK;
    reset.state = UART_LINETRACER_STATE_IDLE;
    reset.load_state = UART_LINETRACER_LOAD_EMPTY;
    CommTxLogic_ObserveEvent(&state, &reset);
    CommTxLogic_MakeHeartbeat(&state, 3000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.error_code == UART_ERROR_NONE);
}

void TestRecoveryEventsClearObservedFault() {
    comm_tx_observed_state_t state{};
    app_tx_event_t event{};

    CommTxLogic_InitObservedState(&state);
    state.error_code = UART_ERROR_SENSOR;
    event.type = APP_TX_EVENT_COMMAND_ACK;
    event.original_command = UART_CMD_LINETRACER_RESET_SYSTEM;
    event.status = UART_STATUS_ACK;
    event.state = UART_LINETRACER_STATE_STOPPED;
    event.load_state = UART_LINETRACER_LOAD_EMPTY;
    CommTxLogic_ObserveEvent(&state, &event);
    assert(state.error_code == UART_ERROR_NONE);

    state.error_code = UART_ERROR_SENSOR;
    event.type = APP_TX_EVENT_STATE_CHANGED;
    event.status = UART_STATUS_SUCCESS;
    event.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    CommTxLogic_ObserveEvent(&state, &event);
    assert(state.error_code == UART_ERROR_NONE);
}

void TestUnloadCompletionClearsObservedActiveRoute() {
    comm_tx_observed_state_t state{};
    auto event = MakeJobEvent(APP_TX_EVENT_COMMAND_ACK);

    CommTxLogic_InitObservedState(&state);
    CommTxLogic_ObserveEvent(&state, &event);
    assert((state.sensor_flags & UART_LINETRACER_FLAG_ROUTE_ACTIVE) != 0U);

    event.type = APP_TX_EVENT_UNLOAD_COMPLETE;
    event.state = UART_LINETRACER_STATE_IDLE;
    event.load_state = UART_LINETRACER_LOAD_EMPTY;
    CommTxLogic_ObserveEvent(&state, &event);
    assert(state.job_id == UART_LINETRACER_JOB_ID_NONE);
    assert(state.route_id == UART_LINETRACER_ROUTE_NONE);
    assert((state.sensor_flags & UART_LINETRACER_FLAG_ROUTE_ACTIVE) == 0U);
}

}  // namespace

int main() {
    TestAckEchoesRequestSequenceAndMetadata();
    TestStatusResponseContainsCurrentState();
    TestStartedEventUsesAsyncSequence();
    TestFaultMayBeReportedWithoutActiveJob();
    TestObstacleStateChangeEventsContainDirectionDistanceAndMotionState();
    TestHeartbeatContainsUptimeStateSensorsAndError();
    TestHeartbeatAllowsIdleAndRejectsUnknownFlags();
    TestAsyncSequenceWraps();
    TestEmergencyEventPriorityPrecedesRegularEvents();
    TestControlSnapshotOverridesBestEffortObservedState();
    TestExistingSensorSnapshotUpdatesBestEffortHeartbeatFlags();
    TestObservedFaultPersistsUntilSuccessfulReset();
    TestRecoveryEventsClearObservedFault();
    TestUnloadCompletionClearsObservedActiveRoute();
    return 0;
}
