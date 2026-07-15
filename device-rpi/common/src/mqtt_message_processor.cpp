#include "logistics/device/mqtt_message_processor.hpp"

#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::device {

namespace mqtt = contracts::mqtt;

MqttMessageProcessor::MqttMessageProcessor(std::string device_id) : device_id_(std::move(device_id)) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("device ID must be one non-wildcard MQTT topic level");
    }
}

IncomingMqttMessage MqttMessageProcessor::DecodeCommand(std::string_view topic, std::string_view payload) const {
    auto decoded = mqtt::DeserializeMessage(payload);
    if (!decoded.IsSuccess()) {
        return {
            .message = {},
            .error = "invalid MQTT JSON; error=" + std::string(mqtt::ToString(decoded.status.error)) +
                     "; field=" + decoded.status.field + "; message=" + decoded.status.message,
        };
    }

    const auto validation = mqtt::ValidateTopicMessage(topic, decoded.value);
    if (!validation.IsSuccess()) {
        return {
            .message = {},
            .error = "MQTT topic/message mismatch; error=" + std::string(mqtt::ToString(validation.error)) +
                     "; message=" + validation.message,
        };
    }

    const auto parsed_topic = mqtt::ParseTopic(topic);
    const bool is_device_command =
        parsed_topic.kind == mqtt::TopicKind::kDeviceCommand && parsed_topic.endpoint_id == device_id_;
    const bool is_broadcast = parsed_topic.kind == mqtt::TopicKind::kSystemBroadcastCommand;
    if (!is_device_command && !is_broadcast) {
        return {
            .message = {},
            .error = "MQTT command is not addressed to this device",
        };
    }

    return {
        .message = std::move(decoded.value),
        .error = {},
    };
}

mqtt::EncodeResult MqttMessageProcessor::EncodeHeartbeat(std::string message_id, std::string timestamp,
                                                         std::string current_state, std::uint64_t uptime,
                                                         std::optional<std::string> job_id,
                                                         std::optional<std::string> error_code) const {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kHeartbeat,
        .source_id = device_id_,
        .timestamp = std::move(timestamp),
        .data =
            mqtt::HeartbeatPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .uptime = uptime,
                .job_id = std::move(job_id),
                .error_code = std::move(error_code),
            },
    };

    const auto validation = mqtt::ValidateTopicMessage(mqtt::DeviceHeartbeatTopic(device_id_), message);
    if (!validation.IsSuccess()) {
        return {
            .payload = {},
            .status = {
                .error = mqtt::CodecError::kInvalidPayload,
                .field = std::string(mqtt::kDataField),
                .message = validation.message,
            },
        };
    }
    return mqtt::SerializeMessage(message);
}

}  // namespace logistics::device
