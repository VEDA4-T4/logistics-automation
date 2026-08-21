#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "comm_tx_logic.h"
#include "control_logic.h"
#include "logistics/contracts/uart_codec.h"
}

namespace {

app_control_command_t MakeCommand(app_control_command_type_t type, std::uint32_t now_ms, std::uint8_t sequence) {
    app_control_command_t command{};
    command.type = type;
    command.received_at_ms = now_ms;
    command.sequence = sequence;
    return command;
}

uart_frame_t Decode(const std::array<std::uint8_t, UART_MAX_FRAME_SIZE>& encoded, std::size_t length) {
    uart_frame_t frame{};
    assert(uart_decode_frame(encoded.data(), length, &frame) == UART_CODEC_OK);
    return frame;
}

void TestControlToCommTxLifecycleAndPersistentFault() {
    control_context_t control{};
    comm_tx_logic_t tx_logic{};
    comm_tx_observed_state_t observed{};
    app_control_snapshot_t control_snapshot{};
    app_tx_event_t tx_event{};
    comm_tx_heartbeat_t heartbeat{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length{};

    ControlLogic_Init(&control, 0U);
    CommTxLogic_Init(&tx_logic);
    CommTxLogic_InitObservedState(&observed);

    auto set_position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    set_position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&control, &set_position, 10U).accepted != 0U);

    auto assign_route = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign_route.job_id = 77U;
    assign_route.route_id = UART_LINETRACER_ROUTE_C;
    const auto assign_result = ControlLogic_HandleCommand(&control, &assign_route, 20U);
    assert(assign_result.accepted != 0U);
    assert(ControlLogic_BuildStartedEvent(&control, &assign_route, &assign_result, UART_LINETRACER_LOAD_EMPTY, 20U,
                                          &tx_event) != 0U);
    assert(tx_event.type == APP_TX_EVENT_STARTED);
    assert(tx_event.job_id == 77U);
    assert(tx_event.route_id == UART_LINETRACER_ROUTE_C);
    assert(CommTxLogic_EncodeEvent(&tx_logic, &tx_event, encoded.data(), encoded.size(), &encoded_length) ==
           UART_CODEC_OK);
    auto frame = Decode(encoded, encoded_length);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_STARTED);
    CommTxLogic_ObserveEvent(&observed, &tx_event);

    ControlLogic_MakeSnapshot(&control, UART_LINETRACER_LOAD_EMPTY, 20U, &control_snapshot);
    CommTxLogic_ObserveControl(&observed, &control_snapshot);
    CommTxLogic_MakeHeartbeat(&observed, 1000U, UART_ERROR_NONE, &heartbeat);
    const auto active_state = heartbeat.state;
    assert(heartbeat.job_id == 77U);
    assert(heartbeat.route_id == UART_LINETRACER_ROUTE_C);
    assert(heartbeat.error_code == UART_ERROR_NONE);

    app_control_safety_event_t emergency{};
    emergency.type = APP_CONTROL_SAFETY_LATCHED;
    emergency.reason = LINETRACER_STOP_REASON_EMERGENCY;
    emergency.error_code = UART_ERROR_EMERGENCY_STOP;
    assert(ControlLogic_ApplySafetyEvent(&control, &emergency, 30U) != 0U);
    assert(ControlLogic_BuildSafetyFaultEvent(&control, &emergency, UART_LINETRACER_LOAD_EMPTY, 30U, &tx_event) != 0U);
    assert(tx_event.type == APP_TX_EVENT_FAULT);
    assert(tx_event.error_code == UART_ERROR_EMERGENCY_STOP);
    assert(CommTxLogic_EncodeEvent(&tx_logic, &tx_event, encoded.data(), encoded.size(), &encoded_length) ==
           UART_CODEC_OK);
    frame = Decode(encoded, encoded_length);
    assert(frame.payload[UART_EVENT_ID_INDEX] == UART_LINETRACER_EVENT_FAULT);
    assert(frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP);
    CommTxLogic_ObserveEvent(&observed, &tx_event);

    ControlLogic_MakeSnapshot(&control, UART_LINETRACER_LOAD_EMPTY, 30U, &control_snapshot);
    CommTxLogic_ObserveControl(&observed, &control_snapshot);
    CommTxLogic_MakeHeartbeat(&observed, 2000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.state == UART_LINETRACER_STATE_EMERGENCY_STOP);
    assert(heartbeat.error_code == UART_ERROR_EMERGENCY_STOP);
    CommTxLogic_MakeHeartbeat(&observed, 3000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.error_code == UART_ERROR_EMERGENCY_STOP);

    app_control_safety_event_t reset{};
    reset.type = APP_CONTROL_SAFETY_RESET_APPROVED;
    assert(ControlLogic_ApplySafetyEvent(&control, &reset, 40U) != 0U);
    ControlLogic_MakeSnapshot(&control, UART_LINETRACER_LOAD_EMPTY, 40U, &control_snapshot);
    CommTxLogic_ObserveControl(&observed, &control_snapshot);
    CommTxLogic_MakeHeartbeat(&observed, 4000U, UART_ERROR_NONE, &heartbeat);
    assert(heartbeat.state == active_state);
    assert(heartbeat.job_id == 77U);
    assert(heartbeat.route_id == UART_LINETRACER_ROUTE_C);
    assert(heartbeat.error_code == UART_ERROR_NONE);
}

}  // namespace

int main() {
    TestControlToCommTxLifecycleAndPersistentFault();
    return 0;
}
