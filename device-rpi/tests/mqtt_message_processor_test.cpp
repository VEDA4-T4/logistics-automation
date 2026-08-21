#include "logistics/device/mqtt_message_processor.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace {

namespace device = logistics::device;
namespace mqtt = logistics::contracts::mqtt;

[[nodiscard]] std::string MakeCommandPayload(std::string target_device_id, std::string request_id = "REQ-COMMAND-01") {
    const mqtt::MqttMessage command{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-COMMAND-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = std::move(request_id),
                .command = mqtt::ControlCommand::kStatusRequest,
                .target_device_id = std::move(target_device_id),
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };

    const auto encoded = mqtt::SerializeMessage(command);
    assert(encoded.IsSuccess());
    return encoded.payload;
}

void TestCommandDecoding() {
    device::MqttMessageProcessor processor("PI-01");
    const auto decoded = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01"));

    assert(decoded.IsSuccess());
    assert(decoded.message.message_type == mqtt::MessageType::kControlCommand);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(decoded.message);
    assert(command != nullptr);
    assert(command->target_device_id == "PI-01");

    const auto duplicate = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01"));
    assert(duplicate.IsSuccess());
    assert(duplicate.duplicate);

    const auto conflicting_id =
        processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01", "REQ-COMMAND-02"));
    assert(!conflicting_id.IsSuccess());
    assert(conflicting_id.error.find("reused") != std::string::npos);

    const auto wrong_device = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-02"), MakeCommandPayload("PI-02"));
    assert(!wrong_device.IsSuccess());

    const auto malformed = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), "{");
    assert(!malformed.IsSuccess());
}

void TestDuplicateCommandFindsCachedResponse() {
    device::MqttMessageProcessor processor("PI-01");
    const auto first = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01"));
    assert(first.IsSuccess());
    assert(!processor.CachedCommandResponse(first.message).has_value());

    const mqtt::MqttMessage response{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-RESPONSE-01",
        .message_type = mqtt::MessageType::kCommandResponse,
        .source_id = "PI-01",
        .timestamp = "2026-07-15T17:30:01+09:00",
        .data =
            mqtt::CommandResponsePayload{
                .request_id = "REQ-COMMAND-01",
                .command = mqtt::ControlCommand::kStatusRequest,
                .result = mqtt::CommandResult::kSuccess,
                .error_code = std::nullopt,
                .message = "status returned",
            },
    };
    processor.RememberCommandResponse(response);

    const auto duplicate = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01"));
    assert(duplicate.IsSuccess());
    assert(duplicate.duplicate);
    const auto cached = processor.CachedCommandResponse(duplicate.message);
    assert(cached.has_value());
    assert(cached->message_id == response.message_id);
    const auto* payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(*cached);
    assert(payload != nullptr);
    assert(payload->result == mqtt::CommandResult::kSuccess);
}

void TestHeartbeatEncoding() {
    const device::MqttMessageProcessor processor("PI-01");
    const auto encoded = processor.EncodeHeartbeat("MSG-HEARTBEAT-01", "2026-07-15T17:30:00+09:00", "IDLE", 42,
                                                   std::nullopt, std::nullopt);

    assert(encoded.IsSuccess());
    const auto decoded = mqtt::DeserializeMessage(encoded.payload);
    assert(decoded.IsSuccess());
    assert(decoded.value.source_id == "PI-01");

    const auto* heartbeat = mqtt::GetPayload<mqtt::HeartbeatPayload>(decoded.value);
    assert(heartbeat != nullptr);
    assert(heartbeat->uptime == 42);
    assert(heartbeat->current_state == "IDLE");

    const auto offline = processor.EncodeOfflineStatus("MSG-WILL-01", "2026-07-15T17:30:00+09:00");
    assert(offline.IsSuccess());
    const auto decoded_offline = mqtt::DeserializeMessage(offline.payload);
    assert(decoded_offline.IsSuccess());
    const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(decoded_offline.value);
    assert(status != nullptr);
    assert(status->status == mqtt::ConnectionState::kOffline);
}

void TestWorkCreatedCommandDecoding() {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "WORK-22a194c3-3e3c-410c-a329-7e8c4ebcac83",
        .message_type = mqtt::MessageType::kWorkCreated,
        .source_id = "central-server",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data = mqtt::WorkCreatedPayload{ .work_id = "22a194c3-3e3c-410c-a329-7e8c4ebcac83" },
    };
    const auto encoded = mqtt::SerializeMessage(message);
    assert(encoded.IsSuccess());

    device::MqttMessageProcessor processor("PI-01");
    const auto decoded = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), encoded.payload);
    assert(decoded.IsSuccess());
    const auto* work = mqtt::GetPayload<mqtt::WorkCreatedPayload>(decoded.message);
    assert(work != nullptr);
    assert(work->work_id == "22a194c3-3e3c-410c-a329-7e8c4ebcac83");
}

void TestRegistrationAndOnlineStatusEncoding() {
    const device::MqttMessageProcessor processor("PI-01");
    const auto registration = processor.EncodeDeviceRegistration("MSG-REGISTER-01", "2026-07-15T17:30:00+09:00",
                                                                 "vision", "vision-node-01", "192.168.0.21", false);

    assert(registration.IsSuccess());
    const auto decoded_registration = mqtt::DeserializeMessage(registration.payload);
    assert(decoded_registration.IsSuccess());
    assert(decoded_registration.value.message_type == mqtt::MessageType::kDeviceRegister);
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceRegisterTopic("PI-01"), decoded_registration.value).IsSuccess());

    const auto* registration_payload = mqtt::GetPayload<mqtt::DeviceRegisterPayload>(decoded_registration.value);
    assert(registration_payload != nullptr);
    assert(registration_payload->device_type == "vision");
    assert(registration_payload->node_name == "vision-node-01");
    assert(registration_payload->ip_address == "192.168.0.21");
    assert(registration_payload->status == mqtt::ConnectionState::kOnline);
    assert(!registration_payload->uart_connected);

    const auto online = processor.EncodeOnlineStatus("MSG-STATUS-01", "2026-07-15T17:30:00+09:00", "IDLE");
    assert(online.IsSuccess());
    const auto decoded_online = mqtt::DeserializeMessage(online.payload);
    assert(decoded_online.IsSuccess());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceStatusTopic("PI-01"), decoded_online.value).IsSuccess());

    const auto* online_payload = mqtt::GetPayload<mqtt::DeviceStatusPayload>(decoded_online.value);
    assert(online_payload != nullptr);
    assert(online_payload->status == mqtt::ConnectionState::kOnline);
    assert(online_payload->current_state == "IDLE");
    assert(!online_payload->error_code.has_value());
}

void TestDeviceEventAndErrorEncoding() {
    const device::MqttMessageProcessor processor("PI-VISION-01");
    const mqtt::MqttMessage event{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-BOX-01",
        .message_type = mqtt::MessageType::kBoxDetected,
        .source_id = "PI-VISION-01",
        .timestamp = "2026-07-21T10:00:00+09:00",
        .data = mqtt::BoxDetectedPayload{ .detected = true, .image_name = "capture-01.jpg" },
    };
    assert(processor.EncodeDeviceEvent(event).IsSuccess());

    auto wrong_source = event;
    wrong_source.source_id = "PI-VISION-02";
    assert(!processor.EncodeDeviceEvent(wrong_source).IsSuccess());

    const mqtt::MqttMessage error{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ERROR-01",
        .message_type = mqtt::MessageType::kErrorOccurred,
        .source_id = "PI-VISION-01",
        .timestamp = "2026-07-21T10:00:01+09:00",
        .data =
            mqtt::ErrorOccurredPayload{
                .job_id = std::nullopt,
                .error_code = "CAMERA_DISCONNECTED",
                .error_level = "ERROR",
                .current_state = "CAMERA_ERROR",
                .message = "camera stream is unavailable",
                .distance = std::nullopt,
            },
    };
    assert(processor.EncodeDeviceError(error).IsSuccess());
    assert(!processor.EncodeDeviceEvent(error).IsSuccess());
}

}  // namespace

int main() {
    TestCommandDecoding();
    TestDuplicateCommandFindsCachedResponse();
    TestHeartbeatEncoding();
    TestWorkCreatedCommandDecoding();
    TestRegistrationAndOnlineStatusEncoding();
    TestDeviceEventAndErrorEncoding();
    return 0;
}
