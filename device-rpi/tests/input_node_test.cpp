#include "logistics/device/input_node.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fake_input_uart_backend.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart_protocol.h"
#include "logistics/device/input_uart_session.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;

using input_test::AlwaysSucceed;
using input_test::AutoResponderBackend;
using input_test::MakeDeviceStatus;
using input_test::MakeOperationResult;
using input_test::MakeSensorStatus;
using input_test::MakeStatusResponse;
using input_test::SucceedWithConveyorState;
using logistics::device::InputCommandResult;
using logistics::device::InputCommandStatus;
using logistics::device::InputNode;
using logistics::device::InputReport;
using logistics::device::InputReportChannel;
using logistics::device::InputUartSession;

constexpr std::string_view kDeviceId = "PI-INPUT-01";

[[nodiscard]] mqtt::MqttMessage MakeControlCommand(mqtt::ControlCommand command, std::string target,
                                                   mqtt::Json params = mqtt::Json::object()) {
    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-1",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "central-server",
        .timestamp = "2026-07-23T00:00:00Z",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "req-1",
                .command = command,
                .target_device_id = std::move(target),
                .component_id = "",
                .params = std::move(params),
            },
    };
}

[[nodiscard]] mqtt::MqttMessage MakeEmergencyStop(std::string target) {
    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-E",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "central-server",
        .timestamp = "2026-07-23T00:00:00Z",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "req-e",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = std::move(target),
            },
    };
}

struct Fixture {
    Fixture() {
        auto owned = std::make_unique<AutoResponderBackend>();
        backend = owned.get();
        session = std::make_unique<InputUartSession>(std::move(owned));
        assert(session->Open());
        node = std::make_unique<InputNode>(std::string(kDeviceId), *session);
        node->SetReportHandler([this](const InputReport& report) { reports.push_back(report); });
    }

    [[nodiscard]] const mqtt::CommandResponsePayload* LastResponse() const {
        for (auto iterator = reports.rbegin(); iterator != reports.rend(); ++iterator) {
            if (iterator->channel == InputReportChannel::kResponse) {
                return std::get_if<mqtt::CommandResponsePayload>(&iterator->data);
            }
        }
        return nullptr;
    }

    AutoResponderBackend* backend{};
    std::unique_ptr<InputUartSession> session;
    std::unique_ptr<InputNode> node;
    std::vector<InputReport> reports;
};

void TestStartSuccess() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    // START is followed by a GET_STATUS read-back.
    assert((fixture.backend->written_commands ==
            std::vector<std::uint8_t>{ UART_CMD_INPUT_CONVEYOR_START, UART_CMD_INPUT_CONVEYOR_GET_STATUS }));
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->command == mqtt::ControlCommand::kStart);
    assert(response->result == mqtt::CommandResult::kSuccess);
}

void TestStartWithSpeed() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();
    mqtt::Json params = mqtt::Json::object();
    params["speed"] = 80;

    const InputCommandResult result = fixture.node->HandleMqttCommand(
        MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId), params));

    assert(result.status == InputCommandStatus::kSuccess);
    assert((fixture.backend->written_commands ==
            std::vector<std::uint8_t>{ UART_CMD_INPUT_CONVEYOR_SET_SPEED, UART_CMD_INPUT_CONVEYOR_START,
                                       UART_CMD_INPUT_CONVEYOR_GET_STATUS }));
}

void TestStartPublishesConveyorStatus() {
    Fixture fixture;
    fixture.backend->responder = SucceedWithConveyorState(UART_INPUT_CONVEYOR_RUNNING, 50U);

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    // The read-back state is published as a status report without the server polling.
    const auto status = std::find_if(fixture.reports.begin(), fixture.reports.end(), [](const InputReport& report) {
        return report.channel == InputReportChannel::kStatus;
    });
    assert(status != fixture.reports.end());
    const auto* payload = std::get_if<mqtt::DeviceStatusPayload>(&status->data);
    assert(payload != nullptr);
    assert(payload->current_state == "RUNNING");
}

void TestStartStillSucceedsWhenStatusReadBackFails() {
    Fixture fixture;
    // Answer the START but stay silent for the follow-up GET_STATUS.
    fixture.backend->responder = [](const uart_frame_t& request) {
        if (request.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS) {
            return std::vector<uart_frame_t>{};
        }
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_SUCCESS, UART_ERROR_NONE) };
    };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    // The command itself succeeded; only the best-effort status report is skipped.
    assert(result.status == InputCommandStatus::kSuccess);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kSuccess);
    const auto status = std::find_if(fixture.reports.begin(), fixture.reports.end(), [](const InputReport& report) {
        return report.channel == InputReportChannel::kStatus;
    });
    assert(status == fixture.reports.end());
}

void TestStop() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStop, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    // STOP is followed by a GET_STATUS read-back.
    assert((fixture.backend->written_commands ==
            std::vector<std::uint8_t>{ UART_CMD_INPUT_CONVEYOR_STOP, UART_CMD_INPUT_CONVEYOR_GET_STATUS }));
}

void TestStatusRequest() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, UART_INPUT_CONVEYOR_RUNNING, 60U) };
    };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStatusRequest, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS);
}

void TestReset() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kInitialize, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONTROL_RESET);
}

void TestRecoveryReleasesLatchFireAndForget() {
    // RECOVERY maps to RESET_DEVICE, which the controller's SafetyTask answers
    // with an asynchronous broadcast, not a sequence-matched reply. Confirm it
    // reports success from a single write with no responder (no retry storm).
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRecovery, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_RESET_DEVICE);
    assert(fixture.backend->write_calls == 1);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kSuccess);
}

void TestControllerRejection() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_ERROR, UART_ERROR_MOTOR) };
    };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kRejected);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kRejected);
    assert(response->error_code.has_value() && *response->error_code == "ERR-MOTOR");
}

void TestUnsupportedCommand() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRestart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kUnsupportedCommand);
    assert(fixture.backend->write_calls == 0);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kRejected);
}

void TestEmergencyStop() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result = fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_EMERGENCY_STOP);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->command == mqtt::ControlCommand::kEmergencyStop);
}

void TestEmergencyStopDoesNotWaitForReply() {
    // The controller answers EMERGENCY_STOP with an asynchronous EVENT/
    // DEVICE_STATUS broadcast, never a sequence-matched reply. Confirm the
    // node reports success from a single write instead of timing out and
    // retrying (which would re-trigger the controller's safety broadcast).
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };

    const InputCommandResult result = fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->write_calls == 1);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kSuccess);
}

void TestInvalidTarget() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, "PI-OTHER-99"));

    assert(result.status == InputCommandStatus::kInvalidTarget);
    assert(fixture.backend->write_calls == 0);
}

void TestControllerTimeout() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStop, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kTimeout);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kTimeout);
    assert(response->error_code.has_value() && *response->error_code == "ERR-UART-ACK-TIMEOUT");
}

void TestSensorDetectedReport() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 15U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kStatus);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->current_state == "OBJECT_DETECTED");
}

void TestSensorFaultReport() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_FAULT, 7U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kError);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.front().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-SENSOR");
    assert(error->distance.has_value() && *error->distance == 7);
}

void TestSensorReportsOnlyOnChange() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 15U));
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 14U));  // same state, ignored
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_CLEAR, 40U));

    assert(fixture.reports.size() == 2);
}

void TestDeviceStatusReport() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeDeviceStatus(UART_DEVICE_RUNNING, UART_ERROR_NONE));

    assert(fixture.reports.size() == 1);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->current_state == "RUNNING");
    assert(status->status == mqtt::ConnectionState::kOnline);
}

void TestMalformedHeartbeatIsIgnored() {
    Fixture fixture;
    // event_id=1 but without the full 9-byte APP_HEARTBEAT payload.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x01U));

    assert(fixture.reports.empty());
}

void TestHeartbeatIsDecodedToDeviceStatus() {
    Fixture fixture;
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE, UART_SENSOR_CLEAR, 42U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kStatus);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->current_state == "RUNNING");
    assert(status->status == mqtt::ConnectionState::kOnline);
    assert(!status->error_code.has_value());
}

void TestHeartbeatErrorIsSurfaced() {
    Fixture fixture;
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_ERROR, UART_ERROR_MOTOR, UART_SENSOR_CLEAR));

    assert(fixture.reports.size() == 1);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->status == mqtt::ConnectionState::kUartError);
    assert(status->error_code.has_value() && *status->error_code == "ERR-MOTOR");
}

void TestHeartbeatReportsOnlyOnChange() {
    Fixture fixture;
    // The controller emits this roughly once a second; only changes should be reported.
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE, UART_SENSOR_CLEAR, 1U));
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE, UART_SENSOR_CLEAR, 2U));
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE, UART_SENSOR_CLEAR, 3U));
    assert(fixture.reports.size() == 1);  // uptime alone is not a state change

    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_STOPPED, UART_ERROR_NONE, UART_SENSOR_CLEAR, 4U));
    assert(fixture.reports.size() == 2);

    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_STOPPED, UART_ERROR_NONE, UART_SENSOR_DETECTED, 5U));
    assert(fixture.reports.size() == 3);  // sensor state change is reported too
}

void TestHealthEventIsDecoded() {
    Fixture fixture;
    // APP_EVENT_HEALTH=0x04, kind=3 (SENSOR_STALE), cause=0 (input channel).
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kError);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.front().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-HEALTH-SENSOR-STALE");
}

void TestSafetyEventIsDecoded() {
    Fixture fixture;
    // APP_EVENT_SAFETY=0x03, kind=1 (ESTOP_LATCHED).
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 1U, 0U));

    assert(fixture.reports.size() == 1);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.front().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-SAFETY-ESTOP-LATCHED");
    assert(error->error_level == "ERROR");
}

void TestSafetyResetCompleteIsReportedAsStatus() {
    Fixture fixture;
    // APP_EVENT_SAFETY=0x03, kind=2 (RESET_COMPLETE) is a successful recovery, so it
    // must be a status report rather than an error.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 2U, 0U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kStatus);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->current_state == "READY");
    assert(status->status == mqtt::ConnectionState::kOnline);
    assert(!status->error_code.has_value());
}

void TestSafetyResetRejectedIsStillAnError() {
    Fixture fixture;
    // kind=3 (RESET_REJECTED) is a genuine failure and stays on the error channel.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 3U, 0U));

    assert(fixture.reports.size() == 1);
    assert(fixture.reports.front().channel == InputReportChannel::kError);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.front().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-SAFETY-RESET-REJECTED");
}

void TestSafetyResetCompleteSuppressesFollowingHeartbeatDuplicate() {
    Fixture fixture;
    // A heartbeat already established EMERGENCY_STOP before recovery.
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_EMERGENCY_STOP, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 1);

    // SAFETY RESET_COMPLETE reports the recovery.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 2U, 0U));
    assert(fixture.reports.size() == 2);

    // The next heartbeat carries the same READY/NONE state; it must not re-report.
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 2);
}

void TestRepeatedControllerEventIsDeduplicated() {
    Fixture fixture;
    // The same latched health condition re-emitted repeatedly must report once.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    assert(fixture.reports.size() == 1);

    // A different condition (kind or cause changes) is a new report.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 1U, 1U));
    assert(fixture.reports.size() == 2);
}

}  // namespace

int main() {
    TestStartSuccess();
    TestStartWithSpeed();
    TestStartPublishesConveyorStatus();
    TestStartStillSucceedsWhenStatusReadBackFails();
    TestStop();
    TestStatusRequest();
    TestReset();
    TestRecoveryReleasesLatchFireAndForget();
    TestControllerRejection();
    TestUnsupportedCommand();
    TestEmergencyStop();
    TestEmergencyStopDoesNotWaitForReply();
    TestInvalidTarget();
    TestControllerTimeout();
    TestSensorDetectedReport();
    TestSensorFaultReport();
    TestSensorReportsOnlyOnChange();
    TestDeviceStatusReport();
    TestMalformedHeartbeatIsIgnored();
    TestHeartbeatIsDecodedToDeviceStatus();
    TestHeartbeatErrorIsSurfaced();
    TestHeartbeatReportsOnlyOnChange();
    TestHealthEventIsDecoded();
    TestSafetyEventIsDecoded();
    TestSafetyResetCompleteIsReportedAsStatus();
    TestSafetyResetRejectedIsStillAnError();
    TestSafetyResetCompleteSuppressesFollowingHeartbeatDuplicate();
    TestRepeatedControllerEventIsDeduplicated();
    return 0;
}
