#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::contracts::mqtt {

enum class TopicMessageError : std::uint8_t {
    kNone,
    kInvalidMessage,
    kInvalidTopic,
    kUnexpectedMessageType,
    kSourceIdMismatch,
    kTargetDeviceIdMismatch,
};

struct TopicMessageStatus {
    TopicMessageError error{ TopicMessageError::kNone };
    std::string message;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error == TopicMessageError::kNone;
    }
};

[[nodiscard]] constexpr std::string_view ToString(TopicMessageError error) noexcept {
    switch (error) {
        case TopicMessageError::kNone:
            return "NONE";
        case TopicMessageError::kInvalidMessage:
            return "INVALID_MESSAGE";
        case TopicMessageError::kInvalidTopic:
            return "INVALID_TOPIC";
        case TopicMessageError::kUnexpectedMessageType:
            return "UNEXPECTED_MESSAGE_TYPE";
        case TopicMessageError::kSourceIdMismatch:
            return "SOURCE_ID_MISMATCH";
        case TopicMessageError::kTargetDeviceIdMismatch:
            return "TARGET_DEVICE_ID_MISMATCH";
    }

    return "UNKNOWN_TOPIC_MESSAGE_ERROR";
}

namespace validation_detail {

[[nodiscard]] constexpr bool IsDeviceEventMessage(MessageType type) noexcept {
    return type == MessageType::kBoxDetected || type == MessageType::kWorkCompleted ||
           type == MessageType::kPositionDetected || type == MessageType::kBarcodeDetected ||
           type == MessageType::kProductImage;
}

[[nodiscard]] constexpr bool IsMessageTypeAllowed(TopicKind kind, MessageType type) noexcept {
    switch (kind) {
        case TopicKind::kQtRequest:
            return type == MessageType::kControlCommand || type == MessageType::kEmergencyStop;
        case TopicKind::kQtResponse:
            return type == MessageType::kCommandResponse;
        case TopicKind::kQtStatus:
            return type == MessageType::kProductInfo || type == MessageType::kDeviceStatus;
        case TopicKind::kQtEvent:
        case TopicKind::kDeviceEvent:
            return IsDeviceEventMessage(type);
        case TopicKind::kQtError:
        case TopicKind::kDeviceError:
            return type == MessageType::kErrorOccurred;
        case TopicKind::kDeviceRegister:
            return type == MessageType::kDeviceRegister;
        case TopicKind::kDeviceCommand:
            return type == MessageType::kControlCommand || type == MessageType::kDestinationSet ||
                   type == MessageType::kEmergencyStop;
        case TopicKind::kDeviceResponse:
            return type == MessageType::kCommandResponse;
        case TopicKind::kDeviceStatus:
        case TopicKind::kServerStatus:
            return type == MessageType::kDeviceStatus;
        case TopicKind::kDeviceHeartbeat:
        case TopicKind::kServerHeartbeat:
            return type == MessageType::kHeartbeat;
        case TopicKind::kSystemBroadcastCommand:
            return type == MessageType::kEmergencyStop;
        case TopicKind::kUnknown:
            return false;
    }

    return false;
}

[[nodiscard]] constexpr bool TopicIdentifiesSource(TopicKind kind) noexcept {
    return kind == TopicKind::kQtRequest || kind == TopicKind::kDeviceRegister || kind == TopicKind::kDeviceResponse ||
           kind == TopicKind::kDeviceStatus || kind == TopicKind::kDeviceEvent || kind == TopicKind::kDeviceError ||
           kind == TopicKind::kDeviceHeartbeat;
}

[[nodiscard]] inline std::string_view TargetDeviceId(const MqttMessage& message) noexcept {
    if (const auto* payload = GetPayload<ControlCommandPayload>(message); payload != nullptr) {
        return payload->target_device_id;
    }
    if (const auto* payload = GetPayload<DestinationSetPayload>(message); payload != nullptr) {
        return payload->target_device_id;
    }
    if (const auto* payload = GetPayload<EmergencyStopPayload>(message); payload != nullptr) {
        return payload->target_device_id;
    }
    return {};
}

[[nodiscard]] inline TopicMessageStatus MakeStatus(TopicMessageError error, std::string message) {
    return {
        .error = error,
        .message = std::move(message),
    };
}

}  // namespace validation_detail

[[nodiscard]] inline TopicMessageStatus ValidateTopicMessage(std::string_view topic, const MqttMessage& message) {
    if (!message.IsValid()) {
        return validation_detail::MakeStatus(TopicMessageError::kInvalidMessage,
                                             "MQTT message failed contract validation");
    }

    const auto parsed_topic = ParseTopic(topic);
    if (!parsed_topic.IsValid()) {
        return validation_detail::MakeStatus(TopicMessageError::kInvalidTopic,
                                             "MQTT topic is not part of the contract");
    }

    if (!validation_detail::IsMessageTypeAllowed(parsed_topic.kind, message.message_type)) {
        return validation_detail::MakeStatus(TopicMessageError::kUnexpectedMessageType,
                                             "messageType is not allowed on the MQTT topic");
    }

    if (validation_detail::TopicIdentifiesSource(parsed_topic.kind) && parsed_topic.endpoint_id != message.source_id) {
        return validation_detail::MakeStatus(TopicMessageError::kSourceIdMismatch,
                                             "sourceId does not match the MQTT topic endpoint");
    }

    if (parsed_topic.kind == TopicKind::kDeviceCommand &&
        validation_detail::TargetDeviceId(message) != parsed_topic.endpoint_id) {
        return validation_detail::MakeStatus(TopicMessageError::kTargetDeviceIdMismatch,
                                             "targetDeviceId does not match the device command topic");
    }

    if (parsed_topic.kind == TopicKind::kSystemBroadcastCommand &&
        validation_detail::TargetDeviceId(message) != "ALL") {
        return validation_detail::MakeStatus(TopicMessageError::kTargetDeviceIdMismatch,
                                             "broadcast command targetDeviceId must be ALL");
    }

    return {};
}

}  // namespace logistics::contracts::mqtt
