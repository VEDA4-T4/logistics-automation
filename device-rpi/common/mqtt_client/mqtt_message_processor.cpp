#include "logistics/device/mqtt_message_processor.hpp"

#include <algorithm>
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

[[nodiscard]] std::string_view RequestIdFromCommand(const contracts::mqtt::MqttMessage& message) noexcept {
    namespace mqtt = contracts::mqtt;

    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message); command != nullptr) {
        return command->request_id;
    }
    if (const auto* command = mqtt::GetPayload<mqtt::DestinationSetPayload>(message); command != nullptr) {
        return command->request_id;
    }
    if (const auto* command = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message); command != nullptr) {
        return command->request_id;
    }
    return {};
}

[[nodiscard]] bool EstablishesProcessEpoch(const contracts::mqtt::MqttMessage& message) noexcept {
    const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message);
    return command != nullptr && (command->command == contracts::mqtt::ControlCommand::kStart ||
                                  command->command == contracts::mqtt::ControlCommand::kRestart ||
                                  command->command == contracts::mqtt::ControlCommand::kInitialize);
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

    bool duplicate = false;
    {
        std::lock_guard recent_lock(recent_messages_mutex_);
        const auto existing = recent_messages_.find(decoded.value.message_id);
        if (existing != recent_messages_.end()) {
            if (existing->second != canonical.payload) {
                return {
                    .message = {},
                    .error = "messageId was reused with a different MQTT command payload",
                };
            }
            duplicate = true;
        }

        if (decoded.value.process_epoch.has_value()) {
            std::lock_guard epoch_lock(epoch_mutex_);
            if (decoded.value.message_type == mqtt::MessageType::kWorkCreated) {
                if (active_process_epoch_.has_value() && *active_process_epoch_ != *decoded.value.process_epoch) {
                    return {
                        .message = {},
                        .error = "WORK_CREATED processEpoch conflicts with the active process",
                    };
                }
                active_process_epoch_ = decoded.value.process_epoch;
            } else if (EstablishesProcessEpoch(decoded.value)) {
                active_process_epoch_ = decoded.value.process_epoch;
            }
            const auto request_id = RequestIdFromCommand(decoded.value);
            if (!request_id.empty()) {
                const std::string key(request_id);
                if (!request_process_epochs_.contains(key)) {
                    request_epoch_order_.push_back(key);
                }
                request_process_epochs_.insert_or_assign(key, *decoded.value.process_epoch);
                if (request_epoch_order_.size() > kRecentMessageLimit) {
                    request_process_epochs_.erase(request_epoch_order_.front());
                    request_epoch_order_.pop_front();
                }
            }
        }

        if (!duplicate) {
            recent_message_order_.push_back(decoded.value.message_id);
            recent_messages_.emplace(decoded.value.message_id, canonical.payload);
            if (recent_message_order_.size() > kRecentMessageLimit) {
                recent_messages_.erase(recent_message_order_.front());
                recent_message_order_.pop_front();
            }
        }
    }

    return {
        .message = std::move(decoded.value),
        .error = {},
        .duplicate = duplicate,
    };
}

void MqttMessageProcessor::ForgetCommand(std::string_view message_id) {
    std::lock_guard lock(recent_messages_mutex_);
    recent_messages_.erase(std::string(message_id));
    recent_message_order_.erase(std::remove(recent_message_order_.begin(), recent_message_order_.end(), message_id),
                                recent_message_order_.end());
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
                .departure_position = std::nullopt,
                .target_position = std::nullopt,
                .confirmed_position = std::nullopt,
                .movement_state = std::nullopt,
                .position_reset = false,
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
                .departure_position = std::nullopt,
                .target_position = std::nullopt,
                .confirmed_position = std::nullopt,
                .movement_state = std::nullopt,
                .position_reset = false,
            },
    };

    return ValidateAndSerialize(mqtt::DeviceStatusTopic(device_id_), message);
}

mqtt::EncodeResult MqttMessageProcessor::EncodeDeviceEvent(const mqtt::MqttMessage& message) const {
    const auto prepared = PrepareOutboundMessage(message);
    return prepared.has_value()
               ? ValidateAndSerialize(mqtt::DeviceEventTopic(device_id_), *prepared)
               : mqtt::EncodeResult{ .payload = {},
                                     .status = { .error = mqtt::CodecError::kInvalidEnvelope,
                                                 .field = std::string(mqtt::kProcessEpochField),
                                                 .message =
                                                     "outbound processEpoch conflicts with the active process" } };
}

mqtt::EncodeResult MqttMessageProcessor::EncodeDeviceError(const mqtt::MqttMessage& message) const {
    const auto prepared = PrepareOutboundMessage(message);
    return prepared.has_value()
               ? ValidateAndSerialize(mqtt::DeviceErrorTopic(device_id_), *prepared)
               : mqtt::EncodeResult{ .payload = {},
                                     .status = { .error = mqtt::CodecError::kInvalidEnvelope,
                                                 .field = std::string(mqtt::kProcessEpochField),
                                                 .message =
                                                     "outbound processEpoch conflicts with the active process" } };
}

std::optional<mqtt::MqttMessage> MqttMessageProcessor::PrepareOutboundMessage(const mqtt::MqttMessage& message) const {
    auto prepared = message;
    std::optional<std::string> epoch;
    {
        std::lock_guard lock(epoch_mutex_);
        if (mqtt::IsProcessScopedMessage(message)) {
            epoch = active_process_epoch_;
        }
        if (!epoch.has_value()) {
            if (const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message); response != nullptr) {
                const auto request = request_process_epochs_.find(response->request_id);
                if (request != request_process_epochs_.end()) {
                    epoch = request->second;
                }
            }
        }
    }
    if (prepared.process_epoch.has_value() && epoch.has_value() && prepared.process_epoch != epoch) {
        return std::nullopt;
    }
    if (!prepared.process_epoch.has_value()) {
        prepared.process_epoch = std::move(epoch);
    }
    return prepared;
}

void MqttMessageProcessor::RememberCommandResponse(const mqtt::MqttMessage& message) {
    const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message);
    if (!message.IsValid() || message.message_type != mqtt::MessageType::kCommandResponse || response == nullptr ||
        response->request_id.empty()) {
        return;
    }

    std::lock_guard lock(response_cache_mutex_);
    const auto existing = response_cache_.find(response->request_id);
    if (existing != response_cache_.end()) {
        existing->second = message;
        return;
    }

    response_cache_order_.push_back(response->request_id);
    response_cache_.emplace(response->request_id, message);
    if (response_cache_order_.size() > kRecentMessageLimit) {
        response_cache_.erase(response_cache_order_.front());
        response_cache_order_.pop_front();
    }
}

std::optional<mqtt::MqttMessage> MqttMessageProcessor::CachedCommandResponse(const mqtt::MqttMessage& command) const {
    const std::string_view request_id = RequestIdFromCommand(command);
    if (request_id.empty()) {
        return std::nullopt;
    }

    std::lock_guard lock(response_cache_mutex_);
    const auto cached = response_cache_.find(std::string(request_id));
    return cached == response_cache_.end() ? std::nullopt : std::optional<mqtt::MqttMessage>{ cached->second };
}

}  // namespace logistics::device
