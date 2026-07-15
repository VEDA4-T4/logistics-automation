#include "logistics/device/mqtt_message_processor.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

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

}  // namespace

int main() {
    TestCommandDecoding();
    TestHeartbeatEncoding();
    return 0;
}
