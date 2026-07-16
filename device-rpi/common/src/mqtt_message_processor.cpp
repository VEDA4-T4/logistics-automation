#include "logistics/device/mqtt_message_processor.hpp"

#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::device {

namespace {

[[nodiscard]] contracts::mqtt::EncodeResult ValidateAndSerialize(std::string_view topic,
                                                                 const contracts::mqtt::MqttMessage& message) {
    namespace mqtt = contracts::mqtt;

    const auto validation = mqtt::ValidateTopicMessage(topic, message);
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

}  // namespace

namespace mqtt = contracts::mqtt;

MqttMessageProcessor::MqttMessageProcessor(std::string device_id) : device_id_(std::move(device_id)) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("device ID must be one non-wildcard MQTT topic level");
    }
}

IncomingMqttMessage MqttMessageProcessor::DecodeCommand(std::string_view topic, std::string_view payload) {
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

    const auto canonical = mqtt::SerializeMessage(decoded.value);
    if (!canonical.IsSuccess()) {
        return {
            .message = {},
            .error = "unable to normalize MQTT command payload",
        };
    }

    {
        std::lock_guard lock(recent_messages_mutex_);
        const auto existing = recent_messages_.find(decoded.value.message_id);
        if (existing != recent_messages_.end()) {
            if (existing->second != canonical.payload) {
                return {
                    .message = {},
                    .error = "messageId was reused with a different MQTT command payload",
                };
            }
            return {
                .message = std::move(decoded.value),
                .error = {},
                .duplicate = true,
            };
        }

        recent_message_order_.push_back(decoded.value.message_id);
        recent_messages_.emplace(decoded.value.message_id, canonical.payload);
        if (recent_message_order_.size() > kRecentMessageLimit) {
            recent_messages_.erase(recent_message_order_.front());
            recent_message_order_.pop_front();
        }
    }

    return {
        .message = std::move(decoded.value),
        .error = {},
        .duplicate = false,
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

    return ValidateAndSerialize(mqtt::DeviceHeartbeatTopic(device_id_), message);
}

mqtt::EncodeResult MqttMessageProcessor::EncodeDeviceRegistration(std::string message_id, std::string timestamp,
                                                                  std::string device_type, std::string node_name,
                                                                  std::string ip_address, bool uart_connected) const {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kDeviceRegister,
        .source_id = device_id_,
        .timestamp = std::move(timestamp),
        .data =
            mqtt::DeviceRegisterPayload{
                .device_type = std::move(device_type),
                .node_name = std::move(node_name),
                .status = mqtt::ConnectionState::kOnline,
                .ip_address = std::move(ip_address),
                .uart_connected = uart_connected,
            },
    };

    return ValidateAndSerialize(mqtt::DeviceRegisterTopic(device_id_), message);
}

mqtt::EncodeResult MqttMessageProcessor::EncodeOnlineStatus(std::string message_id, std::string timestamp,
                                                            std::string current_state) const {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = device_id_,
        .timestamp = std::move(timestamp),
        .data =
            mqtt::DeviceStatusPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .job_id = std::nullopt,
                .error_code = std::nullopt,
            },
    };

    return ValidateAndSerialize(mqtt::DeviceStatusTopic(device_id_), message);
}

mqtt::EncodeResult MqttMessageProcessor::EncodeOfflineStatus(std::string message_id, std::string timestamp) const {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = device_id_,
        .timestamp = std::move(timestamp),
        .data =
            mqtt::DeviceStatusPayload{
                .status = mqtt::ConnectionState::kOffline,
                .current_state = "DISCONNECTED",
                .job_id = std::nullopt,
                .error_code = std::string("ERR-MQTT-DISCONNECTED"),
            },
    };

    return ValidateAndSerialize(mqtt::DeviceStatusTopic(device_id_), message);
}

}  // namespace logistics::device
