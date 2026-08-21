#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "comm_rx_config.h"
#include "comm_rx_dispatch.h"
#include "comm_rx_logic.h"
#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_parser.h"
}

namespace {

enum class PortCall : std::uint8_t {
    NotifyEmergency = 1U,
    PutSafety = 2U,
};

struct FakePort {
    bool control_ok{ true };
    bool safety_ok{ true };
    bool emergency_notify_ok{ true };
    bool response_ok{ true };
    std::uint32_t control_calls{};
    std::uint32_t safety_calls{};
    std::uint32_t emergency_notify_calls{};
    std::uint32_t response_calls{};
    std::uint32_t health_calls{};
    std::uint8_t last_safety_priority{};
    app_control_command_t last_control{};
    app_safety_event_t last_safety{};
    app_tx_event_t last_response{};
    app_health_event_type_t last_health_type{};
    std::uint32_t last_health_detail{};
    std::array<PortCall, 8U> call_order{};
    std::size_t call_order_size{};
};

std::uint8_t PutControl(void* context, const app_control_command_t* command) {
    auto& fake = *static_cast<FakePort*>(context);
    ++fake.control_calls;
    fake.last_control = *command;
    return fake.control_ok ? 1U : 0U;
}

std::uint8_t PutSafety(void* context, const app_safety_event_t* event, std::uint8_t priority) {
    auto& fake = *static_cast<FakePort*>(context);
    ++fake.safety_calls;
    fake.last_safety = *event;
    fake.last_safety_priority = priority;
    fake.call_order[fake.call_order_size++] = PortCall::PutSafety;
    return fake.safety_ok ? 1U : 0U;
}

std::uint8_t NotifyEmergency(void* context) {
    auto& fake = *static_cast<FakePort*>(context);
    ++fake.emergency_notify_calls;
    fake.call_order[fake.call_order_size++] = PortCall::NotifyEmergency;
    return fake.emergency_notify_ok ? 1U : 0U;
}

std::uint8_t PutResponse(void* context, const app_tx_event_t* event) {
    auto& fake = *static_cast<FakePort*>(context);
    ++fake.response_calls;
    fake.last_response = *event;
    return fake.response_ok ? 1U : 0U;
}

void ReportHealth(void* context, app_health_event_type_t type, std::uint32_t detail, std::uint32_t) {
    auto& fake = *static_cast<FakePort*>(context);
    ++fake.health_calls;
    fake.last_health_type = type;
    fake.last_health_detail = detail;
}

comm_rx_dispatch_port_t MakePort(FakePort& fake) {
    return {
        &fake, PutControl, PutSafety, NotifyEmergency, PutResponse, ReportHealth,
    };
}

uart_frame_t MakeAssignRoute(std::uint8_t sequence, std::uint16_t job_id, std::uint8_t route_id) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_LINETRACER_ASSIGN_ROUTE;
    frame.length = UART_LINETRACER_START_PAYLOAD_SIZE;
    frame.payload[UART_LINETRACER_START_JOB_ID_LOW_INDEX] = static_cast<std::uint8_t>(job_id & 0xFFU);
    frame.payload[UART_LINETRACER_START_JOB_ID_HIGH_INDEX] = static_cast<std::uint8_t>(job_id >> 8U);
    frame.payload[UART_LINETRACER_START_ROUTE_ID_INDEX] = route_id;
    return frame;
}

uart_frame_t MakeStop(std::uint8_t sequence, std::uint16_t job_id) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_LINETRACER_STOP_DRIVE;
    frame.length = UART_LINETRACER_STOP_PAYLOAD_SIZE;
    frame.payload[UART_LINETRACER_STOP_JOB_ID_LOW_INDEX] = static_cast<std::uint8_t>(job_id & 0xFFU);
    frame.payload[UART_LINETRACER_STOP_JOB_ID_HIGH_INDEX] = static_cast<std::uint8_t>(job_id >> 8U);
    return frame;
}

uart_frame_t MakePosition(std::uint8_t sequence, std::uint8_t position) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_LINETRACER_SET_CURRENT_POSITION;
    frame.length = UART_LINETRACER_SET_POSITION_PAYLOAD_SIZE;
    frame.payload[UART_LINETRACER_SET_POSITION_ID_INDEX] = position;
    return frame;
}

uart_frame_t MakeEmptyCommand(std::uint8_t sequence, std::uint8_t command) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = command;
    frame.length = 0U;
    return frame;
}

void ExpectDecode(const comm_rx_sequence_history_t& history, const uart_frame_t& frame,
                  comm_rx_destination_t destination, app_control_command_type_t control_type) {
    comm_rx_decoded_command_t decoded{};

    assert(CommRxLogic_DecodeFrame(&history, &frame, 100U, &decoded) == COMM_RX_DECODE_ACCEPTED);
    assert(decoded.destination == destination);
    if (destination == COMM_RX_DESTINATION_CONTROL) {
        assert(decoded.control_command.type == control_type);
        assert(decoded.control_command.original_command == frame.command);
        assert(decoded.control_command.sequence == frame.sequence);
    }
}

void ExpectInvalid(const comm_rx_sequence_history_t& history, const uart_frame_t& frame) {
    comm_rx_decoded_command_t decoded{};
    assert(CommRxLogic_DecodeFrame(&history, &frame, 200U, &decoded) == COMM_RX_DECODE_INVALID_PAYLOAD);
}

void TestAllLineTracerCommandMappings() {
    comm_rx_sequence_history_t history{};
    comm_rx_decoded_command_t decoded{};

    CommRxLogic_Init(&history);

    const auto assign = MakeAssignRoute(0x10U, 0x1234U, UART_LINETRACER_ROUTE_B);
    assert(CommRxLogic_DecodeFrame(&history, &assign, 100U, &decoded) == COMM_RX_DECODE_ACCEPTED);
    assert(decoded.destination == COMM_RX_DESTINATION_CONTROL);
    assert(decoded.control_command.type == APP_CONTROL_COMMAND_ASSIGN_ROUTE);
    assert(decoded.control_command.job_id == 0x1234U);
    assert(decoded.control_command.route_id == UART_LINETRACER_ROUTE_B);

    const auto stop = MakeStop(0x11U, 0x5678U);
    assert(CommRxLogic_DecodeFrame(&history, &stop, 101U, &decoded) == COMM_RX_DECODE_ACCEPTED);
    assert(decoded.destination == COMM_RX_DESTINATION_CONTROL);
    assert(decoded.control_command.type == APP_CONTROL_COMMAND_STOP_DRIVE);
    assert(decoded.control_command.job_id == 0x5678U);

    ExpectDecode(history, MakeEmptyCommand(0x12U, UART_CMD_LINETRACER_STATUS_REQUEST), COMM_RX_DESTINATION_CONTROL,
                 APP_CONTROL_COMMAND_STATUS_REQUEST);

    const auto reset = MakeEmptyCommand(0x13U, UART_CMD_LINETRACER_RESET_SYSTEM);
    assert(CommRxLogic_DecodeFrame(&history, &reset, 103U, &decoded) == COMM_RX_DECODE_ACCEPTED);
    assert(decoded.destination == COMM_RX_DESTINATION_SAFETY);
    assert(decoded.safety_event.type == APP_SAFETY_EVENT_RESET_REQUEST);

    const auto position = MakePosition(0x14U, UART_LINETRACER_POSITION_DEST_C);
    assert(CommRxLogic_DecodeFrame(&history, &position, 104U, &decoded) == COMM_RX_DECODE_ACCEPTED);
    assert(decoded.destination == COMM_RX_DESTINATION_CONTROL);
    assert(decoded.control_command.type == APP_CONTROL_COMMAND_SET_CURRENT_POSITION);
    assert(decoded.control_command.position == UART_LINETRACER_POSITION_DEST_C);

    ExpectDecode(history, MakeEmptyCommand(0x15U, UART_CMD_LINETRACER_RESUME_DRIVE), COMM_RX_DESTINATION_CONTROL,
                 APP_CONTROL_COMMAND_RESUME_DRIVE);
    ExpectDecode(history, MakeEmptyCommand(0x16U, UART_CMD_LINETRACER_MANUAL_UNLOAD), COMM_RX_DESTINATION_CONTROL,
                 APP_CONTROL_COMMAND_MANUAL_UNLOAD);
}

void TestInvalidJobRouteAndPositionRejected() {
    comm_rx_sequence_history_t history{};

    CommRxLogic_Init(&history);
    ExpectInvalid(history, MakeAssignRoute(0x20U, UART_LINETRACER_JOB_ID_NONE, UART_LINETRACER_ROUTE_A));
    ExpectInvalid(history, MakeStop(0x21U, UART_LINETRACER_JOB_ID_NONE));
    ExpectInvalid(history, MakeAssignRoute(0x22U, 1U, UART_LINETRACER_ROUTE_NONE));
    ExpectInvalid(history, MakeAssignRoute(0x23U, 1U, UART_LINETRACER_ROUTE_MAX + 1U));
    ExpectInvalid(history, MakePosition(0x24U, UART_LINETRACER_POSITION_NONE));
    ExpectInvalid(history, MakePosition(0x25U, UART_LINETRACER_POSITION_MAX + 1U));
}

void TestDuplicateSequenceDeliveredOnce() {
    comm_rx_dispatch_t dispatcher{};
    comm_rx_dispatch_effects_t effects{};
    FakePort fake{};
    const auto port = MakePort(fake);
    const auto frame = MakeAssignRoute(0x30U, 7U, UART_LINETRACER_ROUTE_A);

    CommRxDispatch_Init(&dispatcher);
    CommRxDispatch_Frame(&dispatcher, &port, &frame, 100U, &effects);
    assert(effects.control_commands == 1U);
    assert(fake.control_calls == 1U);

    CommRxDispatch_Frame(&dispatcher, &port, &frame, 101U, &effects);
    assert(effects.duplicate_sequences == 1U);
    assert(fake.control_calls == 1U);
    assert(fake.response_calls == 1U);
    assert(fake.last_response.status == UART_STATUS_NACK);
    assert(fake.last_response.error_code == UART_ERROR_SEQUENCE);

    CommRxDispatch_Frame(&dispatcher, &port, &frame, 100U + COMM_RX_SEQUENCE_REPLAY_WINDOW_MS + 1U, &effects);
    assert(effects.control_commands == 1U);
    assert(fake.control_calls == 2U);
}

void TestResponseRetriesThreeTimes() {
    comm_rx_dispatch_t dispatcher{};
    comm_rx_dispatch_effects_t effects{};
    FakePort fake{};
    fake.response_ok = false;
    const auto port = MakePort(fake);
    const auto frame = MakeEmptyCommand(0x40U, UART_CMD_PING);

    CommRxDispatch_Init(&dispatcher);
    CommRxDispatch_Frame(&dispatcher, &port, &frame, 0U, &effects);
    assert(fake.response_calls == 1U);
    assert(CommRxDispatch_PendingCount(&dispatcher) == 1U);

    CommRxDispatch_ProcessPending(&dispatcher, &port, 9U, &effects);
    assert(effects.response_retries == 0U);
    assert(fake.response_calls == 1U);

    CommRxDispatch_ProcessPending(&dispatcher, &port, 10U, &effects);
    assert(effects.response_retries == 1U);
    assert(effects.response_timeouts == 0U);
    assert(fake.response_calls == 2U);

    CommRxDispatch_ProcessPending(&dispatcher, &port, 20U, &effects);
    assert(effects.response_retries == 1U);
    assert(effects.response_timeouts == 0U);
    assert(fake.response_calls == 3U);

    CommRxDispatch_ProcessPending(&dispatcher, &port, 30U, &effects);
    assert(effects.response_retries == 1U);
    assert(effects.response_timeouts == 1U);
    assert(fake.response_calls == 4U);
    assert(fake.last_response.retry_count == COMM_RX_DISPATCH_RESPONSE_MAX_RETRIES);
    assert(CommRxDispatch_PendingCount(&dispatcher) == 0U);
    assert(fake.last_health_type == APP_HEALTH_EVENT_UART_TX_TIMEOUT);
}

void TestBrokenBytesDoNotPreventValidFrameTimeout() {
    comm_rx_link_monitor_t monitor{};
    uart_parser_t parser{};
    uart_frame_t frame{};

    CommRxLogic_LinkInit(&monitor, 0U);
    assert(CommRxLogic_LinkSetMonitoringRequired(&monitor, 1U, 0U) == 0U);
    uart_parser_init(&parser);
    for (std::uint32_t now_ms = 10U; now_ms < COMM_RX_LINK_TIMEOUT_MS; now_ms += 10U) {
        assert(uart_parser_feed(&parser, 0x55U, &frame) == UART_PARSER_NO_FRAME);
        CommRxLogic_LinkRecordRaw(&monitor, now_ms);
        assert(CommRxLogic_LinkCheckTimeout(&monitor, now_ms, COMM_RX_LINK_TIMEOUT_MS) == 0U);
    }

    CommRxLogic_LinkRecordRaw(&monitor, COMM_RX_LINK_TIMEOUT_MS);
    assert(monitor.last_raw_activity_ms == COMM_RX_LINK_TIMEOUT_MS);
    assert(monitor.last_valid_frame_ms == 0U);
    assert(CommRxLogic_LinkCheckTimeout(&monitor, COMM_RX_LINK_TIMEOUT_MS, COMM_RX_LINK_TIMEOUT_MS) == 1U);
    assert(CommRxLogic_LinkCheckTimeout(&monitor, COMM_RX_LINK_TIMEOUT_MS + 1U, COMM_RX_LINK_TIMEOUT_MS) == 0U);
}

void TestIdleLinkDoesNotTimeout() {
    comm_rx_link_monitor_t monitor{};

    CommRxLogic_LinkInit(&monitor, 0U);
    assert(CommRxLogic_LinkCheckTimeout(&monitor, COMM_RX_LINK_TIMEOUT_MS * 2U, COMM_RX_LINK_TIMEOUT_MS) == 0U);
    assert(monitor.timeout_reported == 0U);
}

void TestLinkMonitoringOnlyAppliesWhileVehicleIsMoving() {
    app_control_snapshot_t snapshot{};

    snapshot.job_id = 1U;
    snapshot.route_id = UART_LINETRACER_ROUTE_A;

    snapshot.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 1U);

    snapshot.state = UART_LINETRACER_STATE_CORRECTING;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 1U);

    snapshot.state = UART_LINETRACER_STATE_UNLOADING;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);

    snapshot.state = UART_LINETRACER_STATE_LOAD_WAIT;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);

    snapshot.state = UART_LINETRACER_STATE_STOPPED;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);

    snapshot.state = UART_LINETRACER_STATE_IDLE;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);

    snapshot.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    snapshot.job_id = UART_LINETRACER_JOB_ID_NONE;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);

    snapshot.job_id = 1U;
    snapshot.route_id = UART_LINETRACER_ROUTE_NONE;
    assert(CommRxLogic_LinkMonitoringRequired(&snapshot) == 0U);
    assert(CommRxLogic_LinkMonitoringRequired(nullptr) == 0U);
}

void TestDisablingLinkMonitoringClearsTimeout() {
    comm_rx_link_monitor_t monitor{};

    CommRxLogic_LinkInit(&monitor, 0U);
    assert(CommRxLogic_LinkSetMonitoringRequired(&monitor, 1U, 100U) == 0U);
    assert(CommRxLogic_LinkCheckTimeout(&monitor, 100U + COMM_RX_LINK_TIMEOUT_MS, COMM_RX_LINK_TIMEOUT_MS) == 1U);
    assert(CommRxLogic_LinkSetMonitoringRequired(&monitor, 0U, 200U + COMM_RX_LINK_TIMEOUT_MS) == 1U);
    assert(monitor.timeout_reported == 0U);
    assert(CommRxLogic_LinkCheckTimeout(&monitor, 300U + COMM_RX_LINK_TIMEOUT_MS, COMM_RX_LINK_TIMEOUT_MS) == 0U);
}

void TestPartialFrameTimesOutAt100Ms() {
    uart_parser_t parser{};
    uart_frame_t frame{};

    uart_parser_init(&parser);
    assert(uart_parser_feed(&parser, UART_SOF, &frame) == UART_PARSER_NO_FRAME);
    assert(uart_parser_feed(&parser, UART_PROTOCOL_VERSION, &frame) == UART_PARSER_NO_FRAME);
    assert(uart_parser_feed(&parser, 0x51U, &frame) == UART_PARSER_NO_FRAME);
    assert(uart_parser_tick(&parser, UART_PARSER_TIMEOUT_MS - 1U) == UART_PARSER_NO_FRAME);
    assert(uart_parser_tick(&parser, 1U) == UART_PARSER_TIMEOUT);
    assert(parser.state == UART_PARSER_WAIT_SOF);
}

void TestEmergencyNotificationUsesOneLowLatencyPath() {
    comm_rx_dispatch_t dispatcher{};
    comm_rx_dispatch_effects_t effects{};
    FakePort fake{};
    const auto port = MakePort(fake);
    const auto frame = MakeEmptyCommand(0x60U, UART_CMD_EMERGENCY_STOP);

    CommRxDispatch_Init(&dispatcher);
    CommRxDispatch_Frame(&dispatcher, &port, &frame, 500U, &effects);

    assert(effects.safety_commands == 1U);
    assert(fake.emergency_notify_calls == 1U);
    assert(fake.safety_calls == 0U);
    assert(fake.call_order_size == 1U);
    assert(fake.call_order[0] == PortCall::NotifyEmergency);
}

void TestEmergencyNotificationBypassesFullSafetyQueue() {
    comm_rx_dispatch_t dispatcher{};
    comm_rx_dispatch_effects_t effects{};
    FakePort fake{};
    fake.safety_ok = false;
    const auto port = MakePort(fake);
    const auto frame = MakeEmptyCommand(0x61U, UART_CMD_EMERGENCY_STOP);

    CommRxDispatch_Init(&dispatcher);
    CommRxDispatch_Frame(&dispatcher, &port, &frame, 600U, &effects);

    assert(fake.emergency_notify_calls == 1U);
    assert(fake.safety_calls == 0U);
    assert(effects.safety_commands == 1U);
    assert(effects.queue_drops == 0U);
    assert(fake.response_calls == 0U);

    CommRxDispatch_Frame(&dispatcher, &port, &frame, 601U, &effects);
    assert(effects.duplicate_sequences == 1U);
    assert(fake.emergency_notify_calls == 1U);
}

void TestEmergencyFallsBackToPrioritySafetyQueue() {
    comm_rx_dispatch_t dispatcher{};
    comm_rx_dispatch_effects_t effects{};
    FakePort fake{};
    fake.emergency_notify_ok = false;
    const auto port = MakePort(fake);
    const auto frame = MakeEmptyCommand(0x62U, UART_CMD_EMERGENCY_STOP);

    CommRxDispatch_Init(&dispatcher);
    CommRxDispatch_Frame(&dispatcher, &port, &frame, 700U, &effects);

    assert(fake.emergency_notify_calls == 1U);
    assert(fake.safety_calls == 1U);
    assert(effects.safety_commands == 1U);
    assert(effects.queue_drops == 0U);
    assert(fake.last_safety_priority == APP_TX_PRIORITY_SAFETY);
    assert(fake.last_safety.type == APP_SAFETY_EVENT_EMERGENCY_STOP);
    assert(fake.health_calls == 0U);
}

void TestPartialAndContinuousFrames() {
    comm_rx_sequence_history_t history{};
    comm_rx_decoded_command_t decoded{};
    uart_parser_t parser{};
    uart_frame_t parsed{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> first_encoded{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> second_encoded{};
    std::size_t first_length = 0U;
    std::size_t second_length = 0U;
    const auto first = MakeAssignRoute(0x70U, 7U, UART_LINETRACER_ROUTE_A);
    const auto second = MakeEmptyCommand(0x71U, UART_CMD_LINETRACER_STATUS_REQUEST);
    std::uint32_t accepted = 0U;

    assert(uart_encode_frame(&first, first_encoded.data(), first_encoded.size(), &first_length) == UART_CODEC_OK);
    assert(uart_encode_frame(&second, second_encoded.data(), second_encoded.size(), &second_length) == UART_CODEC_OK);
    CommRxLogic_Init(&history);
    uart_parser_init(&parser);

    for (std::size_t index = 0U; index < 3U; ++index) {
        assert(uart_parser_feed(&parser, first_encoded[index], &parsed) == UART_PARSER_NO_FRAME);
    }

    for (std::size_t index = 3U; index < first_length; ++index) {
        const auto result = uart_parser_feed(&parser, first_encoded[index], &parsed);
        if (result == UART_PARSER_FRAME_READY) {
            assert(CommRxLogic_DecodeFrame(&history, &parsed, 700U, &decoded) == COMM_RX_DECODE_ACCEPTED);
            CommRxLogic_CommitSequence(&history, parsed.sequence, 700U);
            ++accepted;
        }
    }

    for (std::size_t index = 0U; index < second_length; ++index) {
        const auto result = uart_parser_feed(&parser, second_encoded[index], &parsed);
        if (result == UART_PARSER_FRAME_READY) {
            assert(CommRxLogic_DecodeFrame(&history, &parsed, 701U, &decoded) == COMM_RX_DECODE_ACCEPTED);
            CommRxLogic_CommitSequence(&history, parsed.sequence, 701U);
            ++accepted;
        }
    }

    assert(accepted == 2U);
}

}  // namespace

int main() {
    TestAllLineTracerCommandMappings();
    TestInvalidJobRouteAndPositionRejected();
    TestDuplicateSequenceDeliveredOnce();
    TestResponseRetriesThreeTimes();
    TestBrokenBytesDoNotPreventValidFrameTimeout();
    TestIdleLinkDoesNotTimeout();
    TestLinkMonitoringOnlyAppliesWhileVehicleIsMoving();
    TestDisablingLinkMonitoringClearsTimeout();
    TestPartialFrameTimesOutAt100Ms();
    TestEmergencyNotificationUsesOneLowLatencyPath();
    TestEmergencyNotificationBypassesFullSafetyQueue();
    TestEmergencyFallsBackToPrioritySafetyQueue();
    TestPartialAndContinuousFrames();
    return 0;
}
