#include "logistics/device/gripper_node.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The scripted UART backend in this header is protocol agnostic; only its frame
// builders are input specific, so the gripper frames are built locally below.
#include "fake_input_uart_backend.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/gripper_commands.h"
#include "logistics/contracts/uart_protocol.h"
#include "logistics/device/gripper_pose_config.hpp"
#include "logistics/device/input_uart_session.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;

using input_test::AutoResponderBackend;
using logistics::device::GripperCommandResult;
using logistics::device::GripperCommandStatus;
using logistics::device::GripperCycleStep;
using logistics::device::GripperNode;
using logistics::device::GripperPoseConfig;
using logistics::device::GripperReport;
using logistics::device::GripperReportChannel;
using logistics::device::InputUartSession;
using logistics::device::ParseGripperPoseConfig;

constexpr std::string_view kDeviceId = "PI-GRIPPER-01";
constexpr std::string_view kWorkId = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";
constexpr std::string_view kOtherWorkId = "3f2504e0-4f89-11d3-9a0c-0305e82c3302";

[[nodiscard]] uart_frame_t MakeGripperResponse(std::uint8_t sequence, std::uint8_t original_command,
                                               std::uint8_t status = UART_STATUS_SUCCESS,
                                               std::uint8_t error = UART_ERROR_NONE) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_RESPONSE;
    frame.length = UART_RESPONSE_HEADER_SIZE;
    frame.payload[UART_RESPONSE_STATUS_INDEX] = status;
    frame.payload[UART_RESPONSE_COMMAND_INDEX] = original_command;
    frame.payload[UART_RESPONSE_ERROR_INDEX] = error;
    return frame;
}

[[nodiscard]] uart_frame_t MakeStatusResponse(std::uint8_t sequence, std::uint8_t state, bool homed) {
    uart_frame_t frame = MakeGripperResponse(sequence, UART_CMD_GRIPPER_GET_STATUS);
    frame.length = UART_GRIPPER_STATUS_PAYLOAD_SIZE;
    frame.payload[UART_GRIPPER_STATUS_STATE_INDEX] = state;
    frame.payload[UART_GRIPPER_STATUS_HOMED_INDEX] = homed ? 1U : 0U;
    return frame;
}

// MOTION_COMPLETE shares event ID 0x01 with the controller heartbeat and is told
// apart only by its payload length; the builders below keep that explicit.
[[nodiscard]] uart_frame_t MakeMotionComplete(std::uint16_t motion_id, std::uint8_t motion_type) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 210U;
    frame.command = UART_CMD_EVENT;
    frame.length = UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE;
    frame.payload[UART_EVENT_ID_INDEX] = UART_GRIPPER_EVENT_MOTION_COMPLETE;
    frame.payload[UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX] = static_cast<std::uint8_t>(motion_id & 0xFFU);
    frame.payload[UART_GRIPPER_EVENT_MOTION_ID_HIGH_INDEX] = static_cast<std::uint8_t>((motion_id >> 8U) & 0xFFU);
    frame.payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX] = motion_type;
    return frame;
}

[[nodiscard]] uart_frame_t MakeMotionFault(std::uint16_t motion_id, std::uint8_t motion_type, std::uint8_t error) {
    uart_frame_t frame = MakeMotionComplete(motion_id, motion_type);
    frame.length = UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE;
    frame.payload[UART_EVENT_ID_INDEX] = UART_GRIPPER_EVENT_FAULT;
    frame.payload[UART_GRIPPER_FAULT_EVENT_ERROR_INDEX] = error;
    return frame;
}

// APP_EVENT_SAFETY: event_id + latched + cause, 3 bytes.
[[nodiscard]] uart_frame_t MakeSafetyEvent(bool latched, std::uint8_t cause) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 211U;
    frame.command = UART_CMD_EVENT;
    frame.length = 3U;
    frame.payload[UART_EVENT_ID_INDEX] = 0x02U;
    frame.payload[1] = latched ? 1U : 0U;
    frame.payload[2] = cause;
    return frame;
}

// APP_EVENT_HEARTBEAT: event_id + state + error + uptime(u32), 7 bytes.
[[nodiscard]] uart_frame_t MakeControllerHeartbeat(std::uint8_t device_state, std::uint8_t error_code) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 212U;
    frame.command = UART_CMD_EVENT;
    frame.length = 7U;
    frame.payload[UART_EVENT_ID_INDEX] = 0x01U;
    frame.payload[1] = device_state;
    frame.payload[2] = error_code;
    return frame;
}

[[nodiscard]] mqtt::MqttMessage MakeControlCommand(mqtt::ControlCommand command, std::string request_id,
                                                   std::string component = "",
                                                   mqtt::Json params = mqtt::Json::object()) {
    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-" + request_id,
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "central-server",
        .timestamp = "2026-07-28T00:00:00Z",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = std::move(request_id),
                .command = command,
                .target_device_id = std::string(kDeviceId),
                .component_id = std::move(component),
                .params = std::move(params),
            },
    };
}

[[nodiscard]] mqtt::MqttMessage MakeExecuteCommand(std::string request_id, std::string_view work_id) {
    return MakeControlCommand(mqtt::ControlCommand::kExecute, std::move(request_id), "gripper",
                              mqtt::Json{ { "workId", work_id }, { "destination", "1" } });
}

/*
 * EXECUTE carrying a Cartesian target instead of relying on the taught waypoints.
 * The default reachable band is
 * narrow: the forearm is much longer than the upper arm, so a usable target sits well out along +X.
 */
[[nodiscard]] mqtt::MqttMessage MakePoseExecuteCommand(std::string request_id, std::string_view work_id, double x_mm,
                                                       double y_mm, double z_mm) {
    return MakeControlCommand(
        mqtt::ControlCommand::kExecute, std::move(request_id), "gripper",
        mqtt::Json{ { "workId", work_id },
                    { "destination", "1" },
                    { "targetPose", mqtt::Json{ { "x", x_mm }, { "y", y_mm }, { "z", z_mm } } } });
}

[[nodiscard]] mqtt::MqttMessage MakeEmergencyStop() {
    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-E",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "central-server",
        .timestamp = "2026-07-28T00:00:00Z",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "req-estop",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = std::string(kDeviceId),
            },
    };
}

struct Fixture {
    explicit Fixture(GripperPoseConfig config = GripperPoseConfig{}) {
        auto owned = std::make_unique<AutoResponderBackend>();
        backend = owned.get();
        backend->responder = [](const uart_frame_t& request) {
            if (request.command == UART_CMD_GRIPPER_GET_STATUS) {
                return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, UART_GRIPPER_STATE_IDLE, true) };
            }
            // MOVE_ARM/SET_GRIPPER/HOME are asynchronous motions the real
            // controller acknowledges with ACK, not SUCCESS -- completion
            // arrives later as a MOTION_COMPLETE event. RESET/STOP take effect
            // immediately and really do answer SUCCESS. Getting this wrong here
            // is exactly what let the ACK-treated-as-rejected bug through every
            // test in this file: real hardware answered correctly and every
            // motion command was still refused.
            const bool is_async_motion = request.command == UART_CMD_GRIPPER_MOVE_ARM ||
                                         request.command == UART_CMD_GRIPPER_SET_GRIPPER ||
                                         request.command == UART_CMD_GRIPPER_HOME;
            return std::vector<uart_frame_t>{ MakeGripperResponse(
                request.sequence, request.command, is_async_motion ? UART_STATUS_ACK : UART_STATUS_SUCCESS) };
        };
        session = std::make_unique<InputUartSession>(std::move(owned));
        assert(session->Open());
        node = std::make_unique<GripperNode>(std::string(kDeviceId), *session, std::move(config));
        node->SetReportHandler([this](const GripperReport& report) { reports.push_back(report); });
    }

    // Drives the arm to a homed state the way INITIALIZE does on real hardware.
    void Home() {
        const GripperCommandResult result =
            node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kInitialize, "req-init"));
        assert(result.status == GripperCommandStatus::kAccepted);
        node->HandleUartFrame(MakeMotionComplete(result.motion_id, UART_GRIPPER_MOTION_HOME));
        assert(node->IsHomed());
        reports.clear();
        backend->written_commands.clear();
    }

    // Answers the motion the node is currently waiting on.
    void CompleteCurrentMotion(std::uint16_t motion_id, std::uint8_t motion_type) {
        node->HandleUartFrame(MakeMotionComplete(motion_id, motion_type));
    }

    [[nodiscard]] const mqtt::CommandResponsePayload* LastResponse() const {
        for (auto iterator = reports.rbegin(); iterator != reports.rend(); ++iterator) {
            if (iterator->channel == GripperReportChannel::kResponse) {
                return std::get_if<mqtt::CommandResponsePayload>(&iterator->data);
            }
        }
        return nullptr;
    }

    [[nodiscard]] const mqtt::DeviceStatusPayload* LastStatus() const {
        for (auto iterator = reports.rbegin(); iterator != reports.rend(); ++iterator) {
            if (iterator->channel == GripperReportChannel::kStatus) {
                return std::get_if<mqtt::DeviceStatusPayload>(&iterator->data);
            }
        }
        return nullptr;
    }

    [[nodiscard]] const mqtt::ErrorOccurredPayload* LastError() const {
        for (auto iterator = reports.rbegin(); iterator != reports.rend(); ++iterator) {
            if (iterator->channel == GripperReportChannel::kError) {
                return std::get_if<mqtt::ErrorOccurredPayload>(&iterator->data);
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool AnyErrorEquals(std::string_view error_code) const {
        for (const GripperReport& report : reports) {
            if (report.channel != GripperReportChannel::kError) {
                continue;
            }
            const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&report.data);
            if (error != nullptr && error->error_code == error_code) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool AnyStatusEquals(std::string_view state) const {
        for (const GripperReport& report : reports) {
            if (report.channel != GripperReportChannel::kStatus) {
                continue;
            }
            const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data);
            if (status != nullptr && status->current_state == state) {
                return true;
            }
        }
        return false;
    }

    AutoResponderBackend* backend{};
    std::unique_ptr<InputUartSession> session;
    std::unique_ptr<GripperNode> node;
    std::vector<GripperReport> reports;
};

void test_start_homes_without_work_id() {
    Fixture fixture;

    const GripperCommandResult result = fixture.node->HandleMqttCommand(
        MakeControlCommand(mqtt::ControlCommand::kStart, "req-1", "gripper", mqtt::Json::object()));

    assert(result.status == GripperCommandStatus::kAccepted);
    assert(fixture.node->HasActiveCycle());
    assert(fixture.backend->written_commands.size() == 2U);
    assert(fixture.backend->written_commands[0] == UART_CMD_GRIPPER_RESET);
    assert(fixture.backend->written_commands[1] == UART_CMD_GRIPPER_HOME);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->result == mqtt::CommandResult::kProcessing);
    const auto* status = fixture.LastStatus();
    assert(status != nullptr && status->current_state == "HOMING" && !status->job_id.has_value());

    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_HOME);
    assert(fixture.node->IsHomed());
    assert(!fixture.node->HasActiveCycle());
    response = fixture.LastResponse();
    assert(response != nullptr && response->command == mqtt::ControlCommand::kStart &&
           response->result == mqtt::CommandResult::kSuccess);
    status = fixture.LastStatus();
    assert(status != nullptr && status->current_state == "RUNNING" && !status->job_id.has_value());

    fixture.reports.clear();
    fixture.node->HandleUartFrame(MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE));
    status = fixture.LastStatus();
    assert(status != nullptr && status->current_state == "RUNNING" && !status->job_id.has_value());
}

void test_execute_is_rejected_until_the_arm_is_homed() {
    Fixture fixture;

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));

    assert(result.status == GripperCommandStatus::kNotHomed);
    assert(!fixture.node->HasActiveCycle());
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->result == mqtt::CommandResult::kRejected);
    assert(response->error_code.has_value() && *response->error_code == "ERR-GRIPPER-NOT-HOMED");
}

void test_initialize_resets_then_homes() {
    Fixture fixture;

    const GripperCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kInitialize, "req-init"));

    assert(result.status == GripperCommandStatus::kAccepted);
    assert(fixture.backend->written_commands.size() == 2U);
    assert(fixture.backend->written_commands[0] == UART_CMD_GRIPPER_RESET);
    assert(fixture.backend->written_commands[1] == UART_CMD_GRIPPER_HOME);
    assert(!fixture.node->IsHomed());

    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_HOME);
    assert(fixture.node->IsHomed());
    assert(!fixture.node->HasActiveCycle());
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->command == mqtt::ControlCommand::kInitialize &&
           response->request_id == "req-init" && response->result == mqtt::CommandResult::kSuccess);
    const auto* status = fixture.LastStatus();
    assert(status != nullptr && status->current_state == "READY");
}

void test_full_cycle_walks_every_motion_and_reports_completion_once() {
    Fixture fixture;
    fixture.Home();

    GripperCommandResult result = fixture.node->HandleMqttCommand(
        MakeControlCommand(mqtt::ControlCommand::kExecute, "req-1", "gripper",
                           mqtt::Json{ { "workId", kWorkId }, { "destination", "1" } }));
    assert(result.status == GripperCommandStatus::kAccepted);
    assert(fixture.node->HasActiveCycle());
    assert(fixture.node->ActiveStep() == GripperCycleStep::kOpenClaw);

    const std::vector<std::pair<GripperCycleStep, std::uint8_t>> expected{
        { GripperCycleStep::kOpenClaw, UART_GRIPPER_MOTION_GRIPPER },
        { GripperCycleStep::kPickApproach, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kPickDescend, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kCloseClaw, UART_GRIPPER_MOTION_GRIPPER },
        { GripperCycleStep::kPickRetreat, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kTransfer, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kPlaceApproach, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kPlaceDescend, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kReleaseClaw, UART_GRIPPER_MOTION_GRIPPER },
        { GripperCycleStep::kPlaceRetreat, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kReturnTransfer, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kReturnPickSide, UART_GRIPPER_MOTION_ARM },
        { GripperCycleStep::kReturnHome, UART_GRIPPER_MOTION_HOME },
    };

    std::uint16_t motion_id = result.motion_id;
    for (const auto& [step, motion_type] : expected) {
        assert(fixture.node->ActiveStep() == step);
        fixture.CompleteCurrentMotion(motion_id, motion_type);
        ++motion_id;  // the node allocates motion IDs consecutively
    }

    assert(!fixture.node->HasActiveCycle());
    assert(fixture.node->IsHomed());
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->command == mqtt::ControlCommand::kExecute &&
           response->request_id == "req-1" && response->result == mqtt::CommandResult::kSuccess);

    // Exactly one COMPLETED status, carrying the job ID, is what advances the
    // server's work state machine.
    int completed_count = 0;
    for (const GripperReport& report : fixture.reports) {
        const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data);
        if (status != nullptr && status->current_state == "COMPLETED") {
            ++completed_count;
            assert(status->job_id.has_value() && *status->job_id == kWorkId);
        }
    }
    assert(completed_count == 1);
}

void test_progress_states_carry_the_job_id_but_never_report_ready() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_GRIPPER);

    // READY, COMPLETED and PLACED all mean "finished" to the orchestrator, so a
    // mid-cycle status must never use one of them.
    for (const GripperReport& report : fixture.reports) {
        const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data);
        if (status == nullptr || !status->job_id.has_value()) {
            continue;
        }
        assert(status->current_state != "READY");
        assert(status->current_state != "COMPLETED");
        assert(status->current_state != "PLACED");
    }
    assert(fixture.AnyStatusEquals("PICKING"));
}

void test_controller_heartbeat_does_not_overwrite_an_active_cycle_state() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(result.status == GripperCommandStatus::kAccepted);
    fixture.reports.clear();

    // A READY heartbeat mid-cycle would otherwise be republished and read by the
    // server as the transfer having finished.
    fixture.node->HandleUartFrame(MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE));

    assert(!fixture.AnyStatusEquals("READY"));
    assert(fixture.node->HasActiveCycle());
}

// Exact MQTT message resends (same message_id) never reach the node at all:
// MqttNodeClient recognizes them and replays its own cached CommandResponse
// before the command handler is called (see HandleMessage in
// mqtt_node_client.cpp). A distinct new message for work that is already
// active does reach here, and req-2 below exercises that the node itself
// still refuses to re-run the motion for it.
void test_duplicate_work_is_idempotent_and_a_second_work_conflicts() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult first = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(first.status == GripperCommandStatus::kAccepted);
    const int writes_after_first = fixture.backend->write_calls;

    const GripperCommandResult repeat = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-2", kWorkId));
    assert(repeat.status == GripperCommandStatus::kDuplicate);
    assert(fixture.backend->write_calls == writes_after_first);
    const auto* repeat_response = fixture.LastResponse();
    assert(repeat_response != nullptr && repeat_response->result == mqtt::CommandResult::kDuplicated);

    const GripperCommandResult other = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-3", kOtherWorkId));
    assert(other.status == GripperCommandStatus::kActiveCycleConflict);
    assert(fixture.backend->write_calls == writes_after_first);
    assert(fixture.node->ActiveWorkId() == kWorkId);
}

void test_motion_fault_aborts_the_cycle_and_reports_an_error() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    fixture.node->HandleUartFrame(MakeMotionFault(result.motion_id, UART_GRIPPER_MOTION_GRIPPER, UART_ERROR_SERVO));

    assert(!fixture.node->HasActiveCycle());
    const auto* error = fixture.LastError();
    assert(error != nullptr && error->error_code == "ERR-GRIPPER-SERVO");
    assert(error->job_id.has_value() && *error->job_id == kWorkId);
}

void test_a_stale_motion_completion_does_not_advance_the_cycle() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    const GripperCycleStep step_before = fixture.node->ActiveStep();

    // A completion left over from a cancelled motion must not move the sequence on.
    fixture.CompleteCurrentMotion(static_cast<std::uint16_t>(result.motion_id + 500U), UART_GRIPPER_MOTION_GRIPPER);

    assert(fixture.node->ActiveStep() == step_before);
    assert(fixture.node->HasActiveCycle());
}

void test_emergency_stop_aborts_the_cycle_and_clears_the_home_reference() {
    Fixture fixture;
    fixture.Home();

    static_cast<void>(fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId)));
    assert(fixture.node->HasActiveCycle());

    static_cast<void>(fixture.node->HandleMqttCommand(MakeEmergencyStop()));
    fixture.node->HandleUartFrame(MakeSafetyEvent(true, UART_CMD_EMERGENCY_STOP));

    assert(!fixture.node->HasActiveCycle());
    // The controller forgets its home reference on E-Stop, so motions must stay
    // refused until an explicit, operator-confirmed HOME succeeds.
    assert(!fixture.node->IsHomed());
    assert(fixture.AnyStatusEquals("EMERGENCY_STOP"));

    const GripperCommandResult blocked = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-2", kWorkId));
    assert(blocked.status == GripperCommandStatus::kRejected);
}

void test_requested_emergency_stop_is_status_not_device_error() {
    Fixture fixture;
    fixture.Home();

    static_cast<void>(fixture.node->HandleMqttCommand(MakeEmergencyStop()));
    fixture.node->HandleUartFrame(MakeSafetyEvent(true, UART_CMD_EMERGENCY_STOP));
    fixture.node->HandleUartFrame(MakeSafetyEvent(true, UART_CMD_EMERGENCY_STOP));

    assert(!fixture.AnyErrorEquals("ERR-EMERGENCY-STOP"));
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->command == mqtt::ControlCommand::kEmergencyStop &&
           response->result == mqtt::CommandResult::kSuccess);
    const auto* status = fixture.LastStatus();
    assert(status != nullptr && status->current_state == "EMERGENCY_STOP" && !status->error_code.has_value());
}

void test_unrequested_emergency_stop_reports_actual_hex_cause() {
    Fixture fixture;
    fixture.Home();

    fixture.node->HandleUartFrame(MakeSafetyEvent(true, UART_CMD_EMERGENCY_STOP));

    const auto* error = fixture.LastError();
    assert(error != nullptr && error->error_code == "ERR-EMERGENCY-STOP");
    assert(error->message == "controller latched an emergency stop (cause 0xF0)");
    const auto* status = fixture.LastStatus();
    assert(status != nullptr && status->error_code == "ERR-EMERGENCY-STOP");
}

void test_recovery_releases_safety_without_component() {
    Fixture fixture;
    fixture.Home();
    fixture.node->HandleUartFrame(MakeSafetyEvent(true, UART_CMD_EMERGENCY_STOP));
    fixture.reports.clear();
    fixture.backend->written_commands.clear();

    static_cast<void>(fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRecovery, "req-safety")));
    assert(fixture.backend->written_commands.size() == 1U);
    assert(fixture.backend->written_commands[0] == UART_CMD_RESET_DEVICE);
    fixture.node->HandleUartFrame(MakeSafetyEvent(false, UART_CMD_RESET_DEVICE));

    // Releasing the latch does not home the arm, so reporting READY here would
    // invite the server to dispatch work the controller would refuse.
    assert(!fixture.AnyStatusEquals("READY"));
    assert(fixture.AnyStatusEquals("STOPPED"));
    assert(!fixture.node->IsHomed());
}

void test_missing_completion_event_times_out_the_cycle() {
    Fixture fixture;
    fixture.Home();

    static_cast<void>(fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId)));
    assert(fixture.node->HasActiveCycle());

    // Well past the claw duration plus the controller scheduling slack. This same
    // tick also trips the controller heartbeat monitor, so the motion timeout is
    // looked up by code rather than by being the most recent error.
    fixture.node->Tick(std::chrono::milliseconds{ 10000 });

    assert(!fixture.node->HasActiveCycle());
    assert(fixture.AnyErrorEquals("ERR-GRIPPER-MOTION-TIMEOUT"));
}

// Regression: a real-device MQTT test caught StartCycle publishing two
// responses for the same requestId when the very first motion write failed
// (one from AbortCycle, hardcoded to command=START, and one from the caller's
// own EmitCommandResponse) - only the second was actually correct. No prior
// unit test exercised this because the fake backend always succeeded a write;
// forcing a write failure reproduces it.
void test_start_cycle_dispatch_failure_reports_exactly_one_response() {
    Fixture fixture;
    fixture.Home();
    fixture.backend->fail_write = true;

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));

    assert(result.status == GripperCommandStatus::kUartError);
    assert(!fixture.node->HasActiveCycle());
    int response_count = 0;
    for (const GripperReport& report : fixture.reports) {
        if (report.channel != GripperReportChannel::kResponse) {
            continue;
        }
        ++response_count;
        const auto* response = std::get_if<mqtt::CommandResponsePayload>(&report.data);
        assert(response != nullptr && response->command == mqtt::ControlCommand::kExecute &&
               response->request_id == "req-1");
    }
    assert(response_count == 1);
}

void test_stop_cancels_the_active_cycle() {
    Fixture fixture;
    fixture.Home();
    static_cast<void>(fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId)));

    const GripperCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStop, "req-stop"));

    assert(result.status == GripperCommandStatus::kSuccess);
    assert(!fixture.node->HasActiveCycle());
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_STOP);
}

void test_status_request_reports_the_controller_state() {
    Fixture fixture;

    const GripperCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStatusRequest, "req-status"));

    assert(result.status == GripperCommandStatus::kSuccess);
    // GET_STATUS is what re-establishes the home reference after a reconnect.
    assert(fixture.node->IsHomed());
    assert(fixture.AnyStatusEquals("IDLE"));
}

void test_commands_for_another_device_are_ignored() {
    Fixture fixture;
    mqtt::MqttMessage message = MakeExecuteCommand("req-1", kWorkId);
    std::get<mqtt::ControlCommandPayload>(message.data).target_device_id = "PI-INPUT-01";

    const GripperCommandResult result = fixture.node->HandleMqttCommand(message);

    assert(result.status == GripperCommandStatus::kInvalidTarget);
    assert(fixture.backend->write_calls == 0);
}

void test_pick_phase_stops_with_the_box_held() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(
        MakeControlCommand(mqtt::ControlCommand::kExecute, "req-pick", "pick",
                           mqtt::Json{ { "workId", kWorkId }, { "destination", "1" } }));
    assert(result.status == GripperCommandStatus::kAccepted);

    std::uint16_t motion_id = result.motion_id;
    for (const std::uint8_t motion_type : { UART_GRIPPER_MOTION_GRIPPER, UART_GRIPPER_MOTION_ARM,
                                            UART_GRIPPER_MOTION_ARM, UART_GRIPPER_MOTION_GRIPPER }) {
        fixture.CompleteCurrentMotion(motion_id, motion_type);
        ++motion_id;
    }

    // Pick alone ends holding the box; it must not have driven on to the placing
    // waypoints or returned home.
    assert(!fixture.node->HasActiveCycle());
    assert(!fixture.AnyStatusEquals("PLACING"));
}

void test_full_cycle_uses_the_taught_clearance_poses_after_gripping() {
    GripperPoseConfig config;
    config.pick_lift = { 1000U, 500U, 1500U };
    config.transfer = { 1800U, 500U, 1500U };
    Fixture fixture{ config };
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(result.status == GripperCommandStatus::kAccepted);

    std::uint16_t motion_id = result.motion_id;
    fixture.CompleteCurrentMotion(motion_id++, UART_GRIPPER_MOTION_GRIPPER);
    fixture.CompleteCurrentMotion(motion_id++, UART_GRIPPER_MOTION_ARM);
    fixture.CompleteCurrentMotion(motion_id++, UART_GRIPPER_MOTION_ARM);
    fixture.CompleteCurrentMotion(motion_id, UART_GRIPPER_MOTION_GRIPPER);

    assert(fixture.node->ActiveStep() == GripperCycleStep::kPickRetreat);
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_MOVE_ARM);
    assert(uart_gripper_move_base_angle(fixture.backend->last_written.payload) == 1000U);
    assert(uart_gripper_move_shoulder_angle(fixture.backend->last_written.payload) == 500U);
    assert(uart_gripper_move_elbow_angle(fixture.backend->last_written.payload) == 1500U);

    fixture.CompleteCurrentMotion(++motion_id, UART_GRIPPER_MOTION_ARM);
    assert(fixture.node->ActiveStep() == GripperCycleStep::kTransfer);
    assert(uart_gripper_move_base_angle(fixture.backend->last_written.payload) == 1800U);
    assert(uart_gripper_move_shoulder_angle(fixture.backend->last_written.payload) == 500U);
    assert(uart_gripper_move_elbow_angle(fixture.backend->last_written.payload) == 1500U);
}

void test_vision_offset_shifts_the_pick_pose_within_its_limit() {
    GripperPoseConfig config;
    config.pick = { 900U, 1100U, 700U };
    config.base_deci_deg_per_pixel = 0.5;
    config.max_base_correction_deci_deg = 100U;

    assert(config.PickPoseForOffset(0).base_deci_deg == 900U);
    assert(config.PickPoseForOffset(40).base_deci_deg == 920U);
    assert(config.PickPoseForOffset(-40).base_deci_deg == 880U);
    // A wild offset is clamped instead of swinging the arm into the frame.
    assert(config.PickPoseForOffset(100000).base_deci_deg == 1000U);
    assert(config.PickPoseForOffset(-100000).base_deci_deg == 800U);

    // Disabled by default so an uncalibrated camera cannot move the arm at all.
    GripperPoseConfig uncalibrated;
    uncalibrated.pick = { 900U, 1100U, 700U };
    assert(uncalibrated.PickPoseForOffset(500).base_deci_deg == 900U);
}

// ---------------------------------------------------------------------------
// Cartesian targets
// ---------------------------------------------------------------------------

/*
 * A target well out along +X: reachable with the published link lengths and
 * inside the firmware's joint window both at the target and at the approach pose
 * above it.
 */
// Reach with the measured link lengths (85 + 175 mm) is 90 to 260 mm; this and
// its approach pose (60 mm higher, the default approach_height_mm) both stay
// inside that band and within the firmware's shoulder window.
constexpr double kReachableX = 220.0;
constexpr double kReachableZ = 20.0;

[[nodiscard]] logistics::device::JointAngles ArmAnglesOf(const uart_frame_t& frame) {
    return logistics::device::JointAngles{ static_cast<std::uint16_t>(uart_gripper_move_base_angle(frame.payload)),
                                           static_cast<std::uint16_t>(uart_gripper_move_shoulder_angle(frame.payload)),
                                           static_cast<std::uint16_t>(uart_gripper_move_elbow_angle(frame.payload)) };
}

void test_cartesian_pick_pose_drives_the_arm_through_kinematics() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result =
        fixture.node->HandleMqttCommand(MakePoseExecuteCommand("req-1", kWorkId, kReachableX, 0.0, kReachableZ));
    assert(result.status == GripperCommandStatus::kAccepted);

    // Step 1 opens the claw; the arm moves on the step after it.
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_SET_GRIPPER);
    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_GRIPPER);
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_MOVE_ARM);

    const GripperPoseConfig defaults{};
    const logistics::device::JointAngles approach = ArmAnglesOf(fixture.backend->last_written);

    // The taught waypoint is what would have been sent without a targetPose, so a
    // match here would mean the coordinates were silently ignored.
    assert(approach.base_deci_deg != defaults.pick_approach.base_deci_deg ||
           approach.shoulder_deci_deg != defaults.pick_approach.shoulder_deci_deg ||
           approach.elbow_deci_deg != defaults.pick_approach.elbow_deci_deg);

    /*
     * Checking the angles against the requested point rather than against
     * hard-coded numbers: this stays meaningful if the geometry defaults are
     * retuned, and it is what actually matters -- that the claw ends up where the
     * server asked, one approach height above the target.
     */
    const auto reached = logistics::device::SolveForwardKinematics(defaults.geometry, approach);
    assert(reached.has_value());
    assert(std::fabs(reached->x_mm - kReachableX) < 1.0);
    assert(std::fabs(reached->y_mm) < 1.0);
    assert(std::fabs(reached->z_mm - (kReachableZ + defaults.approach_height_mm)) < 1.0);

    // The descent then lands on the target itself.
    const std::uint16_t approach_motion = uart_gripper_move_motion_id(fixture.backend->last_written.payload);
    fixture.CompleteCurrentMotion(approach_motion, UART_GRIPPER_MOTION_ARM);
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_MOVE_ARM);

    const auto descended =
        logistics::device::SolveForwardKinematics(defaults.geometry, ArmAnglesOf(fixture.backend->last_written));
    assert(descended.has_value());
    assert(std::fabs(descended->z_mm - kReachableZ) < 1.0);
}

void test_unreachable_pick_pose_is_rejected_without_moving() {
    Fixture fixture;
    fixture.Home();

    // Beyond the 380 mm the two links can span.
    const GripperCommandResult far_result =
        fixture.node->HandleMqttCommand(MakePoseExecuteCommand("req-far", kWorkId, 600.0, 0.0, kReachableZ));
    assert(far_result.status == GripperCommandStatus::kUnreachablePose);
    assert(!fixture.node->HasActiveCycle());
    // Nothing may be written: a rejected command must leave the arm where it is.
    assert(fixture.backend->written_commands.empty());

    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kRejected);
    assert(response->error_code.has_value());
    assert(*response->error_code == "ERR-GRIPPER-UNREACHABLE-FAR");

    // Inside the fold radius the links cannot close past.
    const GripperCommandResult near_result =
        fixture.node->HandleMqttCommand(MakePoseExecuteCommand("req-near", kOtherWorkId, 50.0, 0.0, kReachableZ));
    assert(near_result.status == GripperCommandStatus::kUnreachablePose);
    const auto* near_response = fixture.LastResponse();
    assert(near_response != nullptr);
    assert(*near_response->error_code == "ERR-GRIPPER-UNREACHABLE-NEAR");
}

void test_target_below_the_plate_is_rejected() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result =
        fixture.node->HandleMqttCommand(MakePoseExecuteCommand("req-low", kWorkId, kReachableX, 0.0, -20.0));
    assert(result.status == GripperCommandStatus::kUnreachablePose);
    assert(!fixture.node->HasActiveCycle());

    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(*response->error_code == "ERR-GRIPPER-POSE-BELOW-PLATE");
}

void test_malformed_pick_pose_is_rejected_rather_than_ignored() {
    Fixture fixture;
    fixture.Home();

    // A targetPose missing z must not silently fall back to the taught waypoints:
    // that would move the arm somewhere the server did not ask for.
    const GripperCommandResult missing = fixture.node->HandleMqttCommand(MakeControlCommand(
        mqtt::ControlCommand::kExecute, "req-bad", "gripper",
        mqtt::Json{ { "workId", kWorkId }, { "targetPose", mqtt::Json{ { "x", kReachableX }, { "y", 0.0 } } } }));
    assert(missing.status == GripperCommandStatus::kUnreachablePose);
    assert(fixture.backend->written_commands.empty());

    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(*response->error_code == "ERR-GRIPPER-POSE-MALFORMED");

    // Coordinates in metres rather than millimetres are caught as malformed too,
    // instead of being reported as an unreachable 0.35 mm target.
    const GripperCommandResult wrong_units = fixture.node->HandleMqttCommand(MakeControlCommand(
        mqtt::ControlCommand::kExecute, "req-units", "gripper",
        mqtt::Json{ { "workId", kWorkId },
                    { "targetPose", mqtt::Json{ { "x", 0.352 }, { "y", 0.0 }, { "z", 99999.0 } } } }));
    assert(wrong_units.status == GripperCommandStatus::kUnreachablePose);
}

void test_a_command_without_a_pick_pose_still_uses_the_taught_waypoints() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(result.status == GripperCommandStatus::kAccepted);
    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_GRIPPER);

    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_MOVE_ARM);
    const GripperPoseConfig defaults{};
    const logistics::device::JointAngles sent = ArmAnglesOf(fixture.backend->last_written);
    assert(sent.base_deci_deg == defaults.pick_approach.base_deci_deg);
    assert(sent.shoulder_deci_deg == defaults.pick_approach.shoulder_deci_deg);
    assert(sent.elbow_deci_deg == defaults.pick_approach.elbow_deci_deg);
}

/*
 * Regression: this mirrors ProcessOrchestrator::MakeGripperCommand's real output
 * byte-for-byte (see central-server-rpi/src/process_manager/process_orchestrator.cpp)
 * rather than the trimmed-down params the other tests use. The server's homography
 * transform sends x/y/z alongside rollDeg/pitchDeg/yawDeg and a handful of other
 * fields this node has no use for; the params key contract test coverage above
 * would not have caught a mismatch against the server's actual field names
 * (targetPose vs. an earlier assumption of pickPose) because it only ever
 * constructed its own params from scratch.
 */
void test_full_server_target_pose_payload_is_accepted() {
    Fixture fixture;
    fixture.Home();

    const mqtt::MqttMessage command =
        MakeControlCommand(mqtt::ControlCommand::kExecute, "req-1", "gripper",
                           mqtt::Json{
                               { "workId", kWorkId },
                               { "destination", "1" },
                               { "action", "PICK" },
                               { "coordinateFrame", "PI-GRIPPER-01_BASE" },
                               { "unit", "mm" },
                               { "targetPose", mqtt::Json{ { "x", kReachableX },
                                                           { "y", 0.0 },
                                                           { "z", kReachableZ },
                                                           { "rollDeg", 180.0 },
                                                           { "pitchDeg", 0.0 },
                                                           { "yawDeg", 37.5 } } },
                               { "box", mqtt::Json{ { "length", 200.0 }, { "width", 150.0 }, { "height", 90.0 } } },
                               { "calibrationVersion", 1 },
                           });

    const GripperCommandResult result = fixture.node->HandleMqttCommand(command);
    assert(result.status == GripperCommandStatus::kAccepted);
    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_SET_GRIPPER);
}

// ---------------------------------------------------------------------------
// Duration
// ---------------------------------------------------------------------------

/*
 * Regression: the controller does not reject a duration that is too short for its
 * speed limits, it runs the motion for max(requested, its own minimum) without
 * saying so. The node used to send its nominal arm_duration_ms and then time out
 * at that value plus 2 s of slack, so any move whose real travel time exceeded
 * the budget was aborted as a lost completion event while the arm was still
 * moving correctly.
 *
 * The default taught waypoints happen to stay under the old budget, which is why
 * this needs a waypoint with a long shoulder travel to reproduce: 800
 * deci-degrees at the shoulder's 120
 * deci-degrees per second is 6.667 s, well past
 * the old 3.5 s.
 */
void test_motion_duration_follows_the_controller_speed_limits() {
    GripperPoseConfig config;
    config.pick_approach = logistics::device::GripperPose{ 900U, 1500U, 900U };
    Fixture fixture{ config };
    fixture.Home();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(result.status == GripperCommandStatus::kAccepted);
    fixture.CompleteCurrentMotion(result.motion_id, UART_GRIPPER_MOTION_GRIPPER);

    assert(fixture.backend->last_written.command == UART_CMD_GRIPPER_MOVE_ARM);
    // Homing leaves the shoulder at 700, so this move travels 800 deci-degrees.
    assert(uart_gripper_move_duration_ms(fixture.backend->last_written.payload) == 6667U);

    // Past the old budget but inside the real travel time: the cycle must survive.
    fixture.node->Tick(std::chrono::milliseconds{ 6000 });
    assert(fixture.node->HasActiveCycle());
    assert(!fixture.AnyErrorEquals("ERR-GRIPPER-MOTION-TIMEOUT"));

    // Past the real travel time plus slack: now it is genuinely a lost event.
    fixture.node->Tick(std::chrono::milliseconds{ 3000 });
    assert(!fixture.node->HasActiveCycle());
    assert(fixture.AnyErrorEquals("ERR-GRIPPER-MOTION-TIMEOUT"));
}

/*
 * After an abort the arm sits part-way through an interpolation at a pose neither
 * side can name, so the next move's travel cannot be predicted. Assuming the last
 * commanded target would under-predict it and time out a correct motion, so the
 * node has to fall back to the worst case the joint limits allow.
 */
void test_an_aborted_cycle_widens_the_next_motion_budget() {
    Fixture fixture;
    fixture.Home();

    const GripperCommandResult first = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(first.status == GripperCommandStatus::kAccepted);
    fixture.node->HandleUartFrame(MakeMotionFault(first.motion_id, UART_GRIPPER_MOTION_GRIPPER, UART_ERROR_SERVO));
    assert(!fixture.node->HasActiveCycle());

    // GET_STATUS is what re-anchors the pose, so without one the next cycle must
    // budget for the longest motion the limits permit.
    const GripperCommandResult second = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-2", kOtherWorkId));
    assert(second.status == GripperCommandStatus::kAccepted);
    fixture.node->Tick(std::chrono::milliseconds{ 9000 });
    assert(fixture.node->HasActiveCycle());

    fixture.node->Tick(std::chrono::milliseconds{ 4000 });
    assert(!fixture.node->HasActiveCycle());
}

/*
 * GET_STATUS is the only frame reporting where the arm actually is, so it is what
 * restores predictable budgets after a fault.
 */
void test_status_request_reanchors_the_motion_budget() {
    Fixture fixture;
    fixture.Home();
    fixture.node->HandleUartFrame(MakeMotionFault(1U, UART_GRIPPER_MOTION_ARM, UART_ERROR_SERVO));

    // The fixture's responder answers GET_STATUS as IDLE with all angles at zero.
    static_cast<void>(
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStatusRequest, "req-status")));
    fixture.reports.clear();

    const GripperCommandResult result = fixture.node->HandleMqttCommand(MakeExecuteCommand("req-1", kWorkId));
    assert(result.status == GripperCommandStatus::kAccepted);

    /*
     * Claw travel is now known: the status reported 0 percent and the cycle opens
     * to 60, which at 40 percent per second is 1500 ms rather than the worst-case
     * 10 s the previous test saw.
     */
    assert(uart_gripper_set_duration_ms(fixture.backend->last_written.payload) == 1500U);
}

void test_pose_config_parses_and_rejects_impossible_claw_travel() {
    const GripperPoseConfig parsed = ParseGripperPoseConfig(R"(
[device]
device_id=PI-GRIPPER-01

[gripper]
home_pose=1000,700,1200
pick_pose=600,1100,700
pick_lift_pose=1000,500,1500
transfer_pose=1800,500,1500
open_position_percent=90
closed_position_percent=20
arm_duration_ms=1200
)");
    assert(parsed.pick.base_deci_deg == 600U);
    assert(parsed.pick.shoulder_deci_deg == 1100U);
    assert(parsed.pick_lift.base_deci_deg == 1000U);
    assert(parsed.pick_lift.shoulder_deci_deg == 500U);
    assert(parsed.pick_lift.elbow_deci_deg == 1500U);
    assert(parsed.transfer.base_deci_deg == 1800U);
    assert(parsed.transfer.shoulder_deci_deg == 500U);
    assert(parsed.transfer.elbow_deci_deg == 1500U);
    assert(parsed.open_position_percent == 90U);
    assert(parsed.arm_duration_ms == 1200U);

    bool threw = false;
    try {
        // A claw that closes no further than it opens would never hold a box.
        static_cast<void>(ParseGripperPoseConfig("[gripper]\nopen_position_percent=20\nclosed_position_percent=80\n"));
    } catch (const logistics::device::GripperConfigError&) {
        threw = true;
    }
    assert(threw);
}

}  // namespace

int main() {
    test_start_homes_without_work_id();
    test_execute_is_rejected_until_the_arm_is_homed();
    test_initialize_resets_then_homes();
    test_full_cycle_walks_every_motion_and_reports_completion_once();
    test_progress_states_carry_the_job_id_but_never_report_ready();
    test_controller_heartbeat_does_not_overwrite_an_active_cycle_state();
    test_duplicate_work_is_idempotent_and_a_second_work_conflicts();
    test_motion_fault_aborts_the_cycle_and_reports_an_error();
    test_a_stale_motion_completion_does_not_advance_the_cycle();
    test_emergency_stop_aborts_the_cycle_and_clears_the_home_reference();
    test_requested_emergency_stop_is_status_not_device_error();
    test_unrequested_emergency_stop_reports_actual_hex_cause();
    test_recovery_releases_safety_without_component();
    test_missing_completion_event_times_out_the_cycle();
    test_start_cycle_dispatch_failure_reports_exactly_one_response();
    test_stop_cancels_the_active_cycle();
    test_status_request_reports_the_controller_state();
    test_commands_for_another_device_are_ignored();
    test_pick_phase_stops_with_the_box_held();
    test_full_cycle_uses_the_taught_clearance_poses_after_gripping();
    test_vision_offset_shifts_the_pick_pose_within_its_limit();
    test_pose_config_parses_and_rejects_impossible_claw_travel();
    test_cartesian_pick_pose_drives_the_arm_through_kinematics();
    test_unreachable_pick_pose_is_rejected_without_moving();
    test_target_below_the_plate_is_rejected();
    test_malformed_pick_pose_is_rejected_rather_than_ignored();
    test_a_command_without_a_pick_pose_still_uses_the_taught_waypoints();
    test_full_server_target_pose_payload_is_accepted();
    test_motion_duration_follows_the_controller_speed_limits();
    test_an_aborted_cycle_widens_the_next_motion_budget();
    test_status_request_reanchors_the_motion_budget();
    std::printf("gripper node tests passed\n");
    return 0;
}
