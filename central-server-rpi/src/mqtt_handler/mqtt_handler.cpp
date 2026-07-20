#include "logistics/central_server/mqtt_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] constexpr bool IsDeviceRegistryMessage(mqtt::MessageType type) noexcept {
    return type == mqtt::MessageType::kDeviceRegister || type == mqtt::MessageType::kHeartbeat ||
           type == mqtt::MessageType::kDeviceStatus || type == mqtt::MessageType::kErrorOccurred;
}

void DefaultLog(MqttHandlerLogLevel level, std::string_view message) {
    const bool is_error = level == MqttHandlerLogLevel::kError;
    std::ostream& output = is_error ? std::cerr : std::clog;
    output << "[server][" << (is_error ? "ERROR" : "INFO") << "] " << message << '\n';
}

[[nodiscard]] constexpr bool IsQtProductEvent(mqtt::MessageType type) noexcept {
    return type == mqtt::MessageType::kBarcodeDetected || type == mqtt::MessageType::kProductImage ||
           type == mqtt::MessageType::kProductInfo || type == mqtt::MessageType::kDestinationSet ||
           type == mqtt::MessageType::kWorkCompleted;
}

[[nodiscard]] EventPayload MakeEventPayload(const mqtt::MqttMessage& message, std::string details_json) {
    EventPayload output;
    output.details_json = std::move(details_json);
    if (const auto* value = mqtt::GetPayload<mqtt::DeviceRegisterPayload>(message)) {
        output.device_role = value->device_type;
        output.connection_state = std::string(mqtt::ToString(value->status));
    } else if (const auto* value = mqtt::GetPayload<mqtt::HeartbeatPayload>(message)) {
        output.connection_state = std::string(mqtt::ToString(value->status));
        output.process_state = value->current_state;
    } else if (const auto* value = mqtt::GetPayload<mqtt::WorkCreatedPayload>(message)) {
        output.work_id = value->work_id;
    } else if (const auto* value = mqtt::GetPayload<mqtt::WorkCompletedPayload>(message)) {
        output.work_id = value->work_id;
        output.process_state = value->result;
    } else if (const auto* value = mqtt::GetPayload<mqtt::PositionDetectedPayload>(message)) {
        output.work_id = value->work_id;
        output.process_state = value->position_status;
    } else if (const auto* value = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(message)) {
        output.work_id = value->work_id;
        output.barcode = value->barcode;
        output.process_state = value->recognition_status;
    } else if (const auto* value = mqtt::GetPayload<mqtt::ProductImagePayload>(message)) {
        output.work_id = value->work_id;
        output.image_id = value->image_id;
        output.image_path = value->image_path;
        output.image_checksum = value->checksum;
        output.image_upload_status = value->upload_status;
    } else if (const auto* value = mqtt::GetPayload<mqtt::ProductInfoPayload>(message)) {
        output.work_id = value->work_id;
        output.barcode = value->barcode;
        output.product_name = value->product_name;
        output.destination = value->destination;
        output.process_state = value->recognition_status;
    } else if (const auto* value = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        output.work_id = value->work_id;
        output.destination = value->destination;
    } else if (const auto* value = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message)) {
        output.connection_state = std::string(mqtt::ToString(value->status));
        output.process_state = value->current_state;
    } else if (const auto* value = mqtt::GetPayload<mqtt::ErrorOccurredPayload>(message)) {
        if (value->job_id && mqtt::IsValidUuid(*value->job_id)) {
            output.work_id = value->job_id;
        }
        output.error_code = value->error_code;
        output.severity = value->error_level;
        output.error_message = value->message;
        output.process_state = value->current_state;
    }
    return output;
}

}  // namespace

MqttHandler::MqttHandler(DeviceManager& device_manager, Logger logger, PersistenceService* persistence_service)
    : device_manager_(device_manager),
      logger_(logger ? std::move(logger) : Logger(DefaultLog)),
      persistence_service_(persistence_service) {}

void MqttHandler::SetWorkCreatedHandler(WorkCreatedHandler handler) {
    work_created_handler_ = std::move(handler);
}

void MqttHandler::SetQtEventHandler(QtEventHandler handler) {
    qt_event_handler_ = std::move(handler);
}

void MqttHandler::SetCommandRouteHandler(MessageRouteHandler handler) {
    command_route_handler_ = std::move(handler);
}

void MqttHandler::SetQtResponseHandler(MessageRouteHandler handler) {
    qt_response_handler_ = std::move(handler);
}

void MqttHandler::SetQtStatusHandler(MessageRouteHandler handler) {
    qt_status_handler_ = std::move(handler);
}

void MqttHandler::SetQtErrorHandler(MessageRouteHandler handler) {
    qt_error_handler_ = std::move(handler);
}

bool MqttHandler::Handle(std::string_view topic, std::string_view payload, std::string_view received_at) {
    const auto decoded = mqtt::DeserializeMessage(payload);
    if (!decoded.IsSuccess()) {
        Log(MqttHandlerLogLevel::kError,
            "invalid MQTT JSON; error=" + std::string(mqtt::ToString(decoded.status.error)) +
                "; field=" + decoded.status.field + "; message=" + decoded.status.message);
        return false;
    }

    const auto validation = mqtt::ValidateTopicMessage(topic, decoded.value);
    if (!validation.IsSuccess()) {
        Log(MqttHandlerLogLevel::kError,
            "MQTT topic/message mismatch; error=" + std::string(mqtt::ToString(validation.error)) +
                "; message=" + validation.message);
        return false;
    }

    const auto parsed_topic = mqtt::ParseTopic(topic);
    if (persistence_service_ != nullptr) {
        const auto root = mqtt::Json::parse(payload.begin(), payload.end());
        const std::string details_json = root.at(std::string(mqtt::kDataField)).dump();
        const mqtt::EnvelopeView envelope{
            .protocol_version = decoded.value.protocol_version,
            .message_id = decoded.value.message_id,
            .message_type = decoded.value.message_type,
            .source_id = decoded.value.source_id,
            .timestamp = decoded.value.timestamp,
            .data_json = details_json,
        };
        const TransportMetadata transport{
            .topic = std::string(topic),
            .qos = 1,
            .retained = false,
            .received_at_ms = CurrentUnixTimeMilliseconds(),
            .source_address = {},
            .raw_payload = std::string(payload),
        };
        const auto result = persistence_service_->PersistValidatedEvent(
            envelope, MakeEventPayload(decoded.value, details_json), transport);
        if (!result.ok()) {
            Log(MqttHandlerLogLevel::kError, "MQTT persistence failed: " + result.message);
            return false;
        }
        if (decoded.value.message_type == mqtt::MessageType::kBoxDetected && result.work_id && work_created_handler_ &&
            !work_created_handler_(decoded.value.source_id, *result.work_id)) {
            Log(MqttHandlerLogLevel::kError, "WORK_CREATED publish failed for workId=" + *result.work_id);
            return false;
        }
        if (IsQtProductEvent(decoded.value.message_type) && qt_event_handler_ && !qt_event_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "Qt product event publish failed");
            return false;
        }
        bool route_succeeded = true;
        if (parsed_topic.kind == mqtt::TopicKind::kQtRequest && command_route_handler_) {
            route_succeeded = command_route_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceResponse && qt_response_handler_) {
            route_succeeded = qt_response_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceStatus && qt_status_handler_) {
            route_succeeded = qt_status_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceError && qt_error_handler_) {
            route_succeeded = qt_error_handler_(decoded.value);
        }
        if (!route_succeeded) {
            Log(MqttHandlerLogLevel::kError, "MQTT message routing failed");
            return false;
        }
    }
    if (IsDeviceRegistryMessage(decoded.value.message_type)) {
        const std::string effective_received_at =
            received_at.empty() ? CurrentIso8601Timestamp() : std::string(received_at);
        if (!device_manager_.HandleMessage(parsed_topic, decoded.value, effective_received_at)) {
            Log(MqttHandlerLogLevel::kError, "device registry update failed: " + device_manager_.LastError());
            return false;
        }
    }

    Log(MqttHandlerLogLevel::kInfo, "MQTT message received: " + std::string(topic));
    if (decoded.value.message_type == mqtt::MessageType::kDeviceRegister) {
        Log(MqttHandlerLogLevel::kInfo,
            "device registered: " + std::string(parsed_topic.endpoint_id) +
                "; registered devices=" + std::to_string(device_manager_.RegisteredDeviceCount()));
    }
    return true;
}

void MqttHandler::Log(MqttHandlerLogLevel level, std::string_view message) const {
    if (logger_) {
        logger_(level, message);
    }
}

}  // namespace logistics::central_server
