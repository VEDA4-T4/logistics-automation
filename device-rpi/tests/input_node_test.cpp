#include "logistics/device/input_node.hpp"

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
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_START);
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
            std::vector<std::uint8_t>{ UART_CMD_INPUT_CONVEYOR_SET_SPEED, UART_CMD_INPUT_CONVEYOR_START }));
}

void TestStop() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStop, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_STOP);
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

}  // namespace

int main() {
    TestStartSuccess();
    TestStartWithSpeed();
    TestStop();
    TestStatusRequest();
    TestReset();
    TestControllerRejection();
    TestUnsupportedCommand();
    TestEmergencyStop();
    TestInvalidTarget();
    TestControllerTimeout();
    TestSensorDetectedReport();
    TestSensorFaultReport();
    TestSensorReportsOnlyOnChange();
    TestDeviceStatusReport();
    return 0;
}
