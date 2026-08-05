#include "logistics/device/input_node.hpp"

#include <cassert>
#include <chrono>
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
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.back().data);
    assert(status != nullptr && status->current_state == "RUNNING");
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

    const InputCommandResult result = fixture.node->HandleMqttCommand(
        MakeControlCommand(mqtt::ControlCommand::kStatusRequest, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS);
}

void TestControllerStatusKeepAliveHasNoCommandResponse() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, UART_INPUT_CONVEYOR_STOPPED, 50U) };
    };

    const InputCommandResult result = fixture.node->RequestControllerStatus();

    assert(result.Succeeded());
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS);
    assert(fixture.LastResponse() == nullptr);
    assert(fixture.reports.size() == 1U);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr && status->current_state == "STOPPED");
}

// The keepalive runs every two seconds purely to prove the UART answers. Without
// change detection each reply published an identical DEVICE_STATUS, so the bus
// carried one redundant status every two seconds for as long as the node idled.
void TestControllerStatusKeepAliveReportsOnlyOnChange() {
    Fixture fixture;
    std::uint8_t conveyor_state = UART_INPUT_CONVEYOR_STOPPED;
    fixture.backend->responder = [&conveyor_state](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, conveyor_state, 50U) };
    };

    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.reports.size() == 1U);

    // Same state on the next three polls: nothing new to say.
    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.reports.size() == 1U);

    conveyor_state = UART_INPUT_CONVEYOR_RUNNING;
    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.reports.size() == 2U);
    const auto* running = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.back().data);
    assert(running != nullptr && running->current_state == "RUNNING");

    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.reports.size() == 2U);
}

// An MQTT status request is an answer owed to the server, so it always reports
// even when the state is unchanged, and it refreshes what the keepalive knows.
void TestStatusRequestAlwaysReportsAndSyncsKeepAlive() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, UART_INPUT_CONVEYOR_STOPPED, 50U) };
    };

    assert(fixture.node->RequestControllerStatus().Succeeded());
    const std::size_t after_keepalive = fixture.reports.size();

    assert(fixture.node
               ->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStatusRequest, std::string(kDeviceId)))
               .Succeeded());
    // Status report plus the command response the server is waiting on.
    assert(fixture.reports.size() > after_keepalive);
    const std::size_t after_request = fixture.reports.size();

    // The request refreshed the record, so the next keepalive stays quiet.
    assert(fixture.node->RequestControllerStatus().Succeeded());
    assert(fixture.reports.size() == after_request);
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
    assert(response->result == mqtt::CommandResult::kProcessing);
}

void TestControllerFailure() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_ERROR, UART_ERROR_MOTOR) };
    };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kControllerFailure);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kFailed);
    assert(response->error_code.has_value() && *response->error_code == "ERR-MOTOR");
}

void TestControllerPolicyRejection() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_ERROR,
                                                              UART_ERROR_EMERGENCY_STOP) };
    };

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kStart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kRejected);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kRejected);
    assert(response->error_code.has_value() && *response->error_code == "ERR-EMERGENCY-STOP");
}

void TestRestartMapsToStart() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputCommandResult result =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRestart, std::string(kDeviceId)));

    assert(result.status == InputCommandStatus::kSuccess);
    assert(fixture.backend->write_calls == 1);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_START);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr);
    assert(response->result == mqtt::CommandResult::kSuccess);
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
    assert(response->result == mqtt::CommandResult::kProcessing);
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
    assert(response->result == mqtt::CommandResult::kProcessing);
}

void TestPendingSafetyCommandCannotBeOverwritten() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };
    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId))).Succeeded());

    const auto recovery =
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRecovery, std::string(kDeviceId)));
    const auto duplicate_estop = fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId)));

    assert(recovery.status == InputCommandStatus::kRejected);
    assert(duplicate_estop.status == InputCommandStatus::kRejected);
    assert(fixture.backend->write_calls == 1);
    assert(fixture.node->HasPendingSafetyCommand());
}

void TestSafetyCommandsCompleteWithOriginalRequestId() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };

    static_cast<void>(fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId))));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 1U, 0U));
    const auto* estop = fixture.LastResponse();
    assert(estop != nullptr);
    assert(estop->request_id == "req-e");
    assert(estop->result == mqtt::CommandResult::kSuccess);

    static_cast<void>(
        fixture.node->HandleMqttCommand(MakeControlCommand(mqtt::ControlCommand::kRecovery, std::string(kDeviceId))));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 2U, 0U));
    const auto* recovery = fixture.LastResponse();
    assert(recovery != nullptr);
    assert(recovery->request_id == "req-1");
    assert(recovery->result == mqtt::CommandResult::kSuccess);
}

void TestSafetyAndHeartbeatTimeoutsAreReported() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };
    static_cast<void>(fixture.node->HandleMqttCommand(MakeEmergencyStop(std::string(kDeviceId))));
    fixture.node->Tick(mqtt::kEmergencyStopConfirmationTimeout);
    const auto* response = fixture.LastResponse();
    assert(response != nullptr && response->result == mqtt::CommandResult::kTimeout);

    fixture.reports.clear();
    fixture.node->ResetControllerHeartbeatMonitor();
    fixture.node->Tick(std::chrono::seconds{ 3 });
    assert(fixture.reports.size() == 2U);
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    const auto* recovered = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.back().data);
    assert(recovered != nullptr && recovered->current_state == "RUNNING");
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
    // Sensor readings are telemetry: SENSOR_STATUS on the event channel, carrying
    // the sensor id and distance, not a device-state report.
    assert(fixture.reports.front().channel == InputReportChannel::kEvent);
    assert(fixture.reports.front().message_type == mqtt::MessageType::kSensorStatus);
    const auto* sensor = std::get_if<mqtt::SensorStatusPayload>(&fixture.reports.front().data);
    assert(sensor != nullptr);
    assert(sensor->sensor_id == UART_INPUT_SENSOR_ID_1);
    assert(sensor->measurement_status == "DETECTED");
    assert(sensor->distance_cm == 15);
    assert(sensor->IsValid());
}

void TestSensorClearReportsMeasurementStatusClear() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_CLEAR, 40U));

    assert(fixture.reports.size() == 1);
    const auto* sensor = std::get_if<mqtt::SensorStatusPayload>(&fixture.reports.front().data);
    assert(sensor != nullptr);
    assert(sensor->measurement_status == "CLEAR");
    assert(sensor->distance_cm == 40);
    assert(sensor->IsValid());
}

void TestSensorFaultReport() {
    Fixture fixture;
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_FAULT, 7U));

    // A fault still raises the operator-facing error, alongside the telemetry event.
    assert(fixture.reports.size() == 2);
    const auto* sensor = std::get_if<mqtt::SensorStatusPayload>(&fixture.reports.front().data);
    assert(sensor != nullptr);
    assert(sensor->measurement_status == "FAULT");

    assert(fixture.reports.back().channel == InputReportChannel::kError);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.back().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-SENSOR");
    assert(error->distance.has_value() && *error->distance == 7);
}

void TestEverySensorMeasurementIsPublished() {
    Fixture fixture;
    // Distance changes even while the state does not, so every measurement is sent.
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 15U));
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 14U));
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_CLEAR, 40U));

    assert(fixture.reports.size() == 3);
    for (const auto& report : fixture.reports) {
        assert(report.channel == InputReportChannel::kEvent);
        assert(std::holds_alternative<mqtt::SensorStatusPayload>(report.data));
    }
}

void TestSensorActivityDoesNotOverwriteDeviceState() {
    Fixture fixture;
    // The controller heartbeat establishes the operational state...
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 1);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr && status->current_state == "READY");

    // ...and sensor traffic must not replace it with a sensor state.
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 15U));
    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_CLEAR, 40U));
    for (std::size_t index = 1; index < fixture.reports.size(); ++index) {
        assert(!std::holds_alternative<mqtt::DeviceStatusPayload>(fixture.reports[index].data));
    }
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

    // A sensor-only change must NOT trigger a heartbeat report: HandleSensorStatus
    // (a separate UART_CMD_SENSOR_STATUS frame) already reports sensor transitions
    // with the correct SENSOR_CLEAR/OBJECT_DETECTED naming, so re-reporting the
    // unchanged device_state here would just be a misleading duplicate.
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_STOPPED, UART_ERROR_NONE, UART_SENSOR_DETECTED, 5U));
    assert(fixture.reports.size() == 2);
}

void TestHeartbeatDoesNotDuplicateSensorTransition() {
    // Reproduces the real-hardware sequence: a dedicated SENSOR_STATUS frame reports
    // the transition, then the next ~1 Hz heartbeat carries the same new sensor
    // reading. The heartbeat must not add a redundant device-status report.
    Fixture fixture;
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 1);

    fixture.node->HandleUartFrame(MakeSensorStatus(UART_SENSOR_DETECTED, 15U));
    assert(fixture.reports.size() == 2);  // the SENSOR_STATUS telemetry event

    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE, UART_SENSOR_DETECTED));
    assert(fixture.reports.size() == 2);  // no extra "READY" report from the heartbeat
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

void TestOppositeUartChannelTimeoutIsIgnored() {
    Fixture fixture;
    // The input Pi can only receive a timeout report for the opposite sorting
    // channel because the timed-out channel itself is not writable.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 1U, 1U));

    assert(fixture.reports.empty());
}

void TestHealthEventIncludesSensorId() {
    Fixture fixture;
    // APP_EVENT_HEALTH=0x04, kind=3 (SENSOR_STALE), cause=0 (input channel), sensorId=1.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U, 1U));

    assert(fixture.reports.size() == 1);
    const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.front().data);
    assert(error != nullptr);
    assert(error->error_code == "ERR-HEALTH-SENSOR-STALE");
    assert(error->message.find("sensorId=1") != std::string::npos);

    // A different sensorId on the same (kind, cause) must not be deduplicated away -
    // it is a distinct sensor, not a repeat of the same condition.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U, 2U));
    assert(fixture.reports.size() == 2);
    const auto* second = std::get_if<mqtt::ErrorOccurredPayload>(&fixture.reports.back().data);
    assert(second != nullptr);
    assert(second->message.find("sensorId=2") != std::string::npos);

    // The same sensorId repeated is still deduplicated.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U, 2U));
    assert(fixture.reports.size() == 2);
}

void TestSafetyEventIsDecoded() {
    Fixture fixture;
    // APP_EVENT_SAFETY=0x03, kind=1 (ESTOP_LATCHED).
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 1U, 0U));

    assert(fixture.reports.size() == 1);
    const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(status != nullptr);
    assert(status->current_state == "EMERGENCY_STOP");
    assert(status->status == mqtt::ConnectionState::kOnline);
    assert(!status->error_code.has_value());

    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 1);
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
    assert(status->current_state == "STOPPED");
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
    const auto* emergency = std::get_if<mqtt::DeviceStatusPayload>(&fixture.reports.front().data);
    assert(emergency != nullptr);
    assert(emergency->current_state == "EMERGENCY_STOP");
    assert(emergency->status == mqtt::ConnectionState::kOnline);
    assert(!emergency->error_code.has_value());

    // SAFETY RESET_COMPLETE reports the recovery.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x03U, 2U, 0U));
    assert(fixture.reports.size() == 2);

    // The next heartbeat carries the same STOPPED/NONE state; it must not re-report.
    fixture.node->HandleUartFrame(
        input_test::MakeControllerHeartbeat(UART_DEVICE_STOPPED, UART_ERROR_NONE, UART_SENSOR_CLEAR));
    assert(fixture.reports.size() == 2);
}

void TestRepeatedControllerEventIsDeduplicated() {
    Fixture fixture;
    // The same latched health condition re-emitted repeatedly must report once.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 3U, 0U));
    assert(fixture.reports.size() == 1);

    // A timeout for the opposite UART is intentionally ignored; it describes
    // another Pi and must not change this node's state.
    fixture.node->HandleUartFrame(input_test::MakeControllerEvent(0x04U, 1U, 1U));
    assert(fixture.reports.size() == 1);
}

}  // namespace

int main() {
    TestStartSuccess();
    TestStartWithSpeed();
    TestStop();
    TestStatusRequest();
    TestControllerStatusKeepAliveHasNoCommandResponse();
    TestControllerStatusKeepAliveReportsOnlyOnChange();
    TestStatusRequestAlwaysReportsAndSyncsKeepAlive();
    TestReset();
    TestRecoveryReleasesLatchFireAndForget();
    TestControllerFailure();
    TestControllerPolicyRejection();
    TestRestartMapsToStart();
    TestEmergencyStop();
    TestEmergencyStopDoesNotWaitForReply();
    TestPendingSafetyCommandCannotBeOverwritten();
    TestSafetyCommandsCompleteWithOriginalRequestId();
    TestSafetyAndHeartbeatTimeoutsAreReported();
    TestInvalidTarget();
    TestControllerTimeout();
    TestSensorDetectedReport();
    TestSensorClearReportsMeasurementStatusClear();
    TestSensorFaultReport();
    TestEverySensorMeasurementIsPublished();
    TestSensorActivityDoesNotOverwriteDeviceState();
    TestDeviceStatusReport();
    TestMalformedHeartbeatIsIgnored();
    TestHeartbeatIsDecodedToDeviceStatus();
    TestHeartbeatErrorIsSurfaced();
    TestHeartbeatReportsOnlyOnChange();
    TestHeartbeatDoesNotDuplicateSensorTransition();
    TestHealthEventIsDecoded();
    TestOppositeUartChannelTimeoutIsIgnored();
    TestHealthEventIncludesSensorId();
    TestSafetyEventIsDecoded();
    TestSafetyResetCompleteIsReportedAsStatus();
    TestSafetyResetRejectedIsStillAnError();
    TestSafetyResetCompleteSuppressesFollowingHeartbeatDuplicate();
    TestRepeatedControllerEventIsDeduplicated();
    return 0;
}
