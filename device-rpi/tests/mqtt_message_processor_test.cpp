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

[[nodiscard]] std::string MakeCommandPayload(std::string target_device_id) {
    const mqtt::MqttMessage command{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-COMMAND-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "REQ-COMMAND-01",
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
    const device::MqttMessageProcessor processor("PI-01");
    const auto decoded = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), MakeCommandPayload("PI-01"));

    assert(decoded.IsSuccess());
    assert(decoded.message.message_type == mqtt::MessageType::kControlCommand);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(decoded.message);
    assert(command != nullptr);
    assert(command->target_device_id == "PI-01");

    const auto wrong_device = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-02"), MakeCommandPayload("PI-02"));
    assert(!wrong_device.IsSuccess());

    const auto malformed = processor.DecodeCommand(mqtt::DeviceCommandTopic("PI-01"), "{");
    assert(!malformed.IsSuccess());
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

}  // namespace

int main() {
    TestCommandDecoding();
    TestHeartbeatEncoding();
    TestRegistrationAndOnlineStatusEncoding();
    return 0;
}
