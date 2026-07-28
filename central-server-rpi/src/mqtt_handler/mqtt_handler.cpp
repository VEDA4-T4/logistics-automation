#include "logistics/central_server/mqtt_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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
           type == mqtt::MessageType::kWorkCompleted || type == mqtt::MessageType::kSensorStatus;
}

[[nodiscard]] std::string RejectedMessageContext(std::string_view topic, std::string_view payload) {
    std::string context = "; topic=" + std::string(topic);
    const auto root = mqtt::Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (!root.is_object()) {
        return context;
    }
    const auto message_type = root.find(std::string(mqtt::kMessageTypeField));
    if (message_type != root.end() && message_type->is_string()) {
        context += "; receivedMessageType=" + message_type->get<std::string>();
    }
    return context;
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
        output.product_id = value->product_id;
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

void MqttHandler::SetProcessMessageGuard(ProcessMessageHandler handler) {
    process_message_guard_ = std::move(handler);
}

void MqttHandler::SetProcessMessageHandler(ProcessMessageHandler handler) {
    process_message_handler_ = std::move(handler);
}

bool MqttHandler::Handle(std::string_view topic, std::string_view payload, std::string_view received_at) {
    const auto decoded = mqtt::DeserializeMessage(payload);
    if (!decoded.IsSuccess()) {
        Log(MqttHandlerLogLevel::kError,
            "invalid MQTT JSON; error=" + std::string(mqtt::ToString(decoded.status.error)) + "; field=" +
                decoded.status.field + "; message=" + decoded.status.message + RejectedMessageContext(topic, payload));
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
    if (process_message_guard_ && !process_message_guard_(decoded.value)) {
        Log(MqttHandlerLogLevel::kError, "MQTT process transition rejected before persistence");
        return false;
    }
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
        PersistenceResult result;
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = persistence_service_->PersistValidatedEvent(
                envelope, MakeEventPayload(decoded.value, details_json), transport);
            if (result.ok() || result.status != PersistenceStatus::kRetryableError) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
        }
        if (!result.ok()) {
            Log(MqttHandlerLogLevel::kError, "MQTT persistence failed: " + result.message);
            return false;
        }
        if (process_message_handler_ && !process_message_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "MQTT process transition commit failed");
            return false;
        }

        std::optional<mqtt::MqttMessage> catalog_product_message;
        if (const auto* barcode = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(decoded.value);
            barcode != nullptr && barcode->recognition_status == "SUCCESS") {
            std::optional<CatalogProduct> catalog_product;
            const auto lookup_status =
                persistence_service_->FindActiveProductByBarcode(barcode->barcode, catalog_product);
            if (!lookup_status.ok()) {
                Log(MqttHandlerLogLevel::kError, "product catalog lookup failed: " + lookup_status.message);
                return false;
            }
            if (catalog_product) {
                catalog_product_message = mqtt::MqttMessage{
                    .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                    .message_id = "CATALOG-" + decoded.value.message_id,
                    .message_type = mqtt::MessageType::kProductInfo,
                    .source_id = "central-server",
                    .timestamp = decoded.value.timestamp,
                    .data =
                        mqtt::ProductInfoPayload{
                            .work_id = barcode->work_id,
                            .recognition_status = "SUCCESS",
                            .barcode = catalog_product->barcode,
                            .product_id = catalog_product->product_id,
                            .product_name = catalog_product->product_name,
                            .destination = catalog_product->destination,
                            .image = nullptr,
                            .confidence = barcode->confidence,
                            .message = std::nullopt,
                        },
                };
                if (process_message_guard_ && !process_message_guard_(*catalog_product_message)) {
                    Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO process transition rejected");
                    return false;
                }
                const auto encoded_product = mqtt::SerializeMessage(*catalog_product_message);
                if (!encoded_product.IsSuccess()) {
                    Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO serialization failed");
                    return false;
                }
                const auto product_root = mqtt::Json::parse(encoded_product.payload);
                const std::string product_details = product_root.at(std::string(mqtt::kDataField)).dump();
                const mqtt::EnvelopeView product_envelope{
                    .protocol_version = catalog_product_message->protocol_version,
                    .message_id = catalog_product_message->message_id,
                    .message_type = catalog_product_message->message_type,
                    .source_id = catalog_product_message->source_id,
                    .timestamp = catalog_product_message->timestamp,
                    .data_json = product_details,
                };
                const TransportMetadata product_transport{
                    .topic = mqtt::DeviceEventTopic(catalog_product_message->source_id),
                    .qos = 1,
                    .retained = false,
                    .received_at_ms = CurrentUnixTimeMilliseconds(),
                    .source_address = "central-server",
                    .raw_payload = encoded_product.payload,
                };
                const auto product_result = persistence_service_->PersistValidatedEvent(
                    product_envelope, MakeEventPayload(*catalog_product_message, product_details), product_transport);
                if (!product_result.ok()) {
                    Log(MqttHandlerLogLevel::kError,
                        "catalog PRODUCT_INFO persistence failed: " + product_result.message);
                    return false;
                }
                if (process_message_handler_ && !process_message_handler_(*catalog_product_message)) {
                    Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO process transition commit failed");
                    return false;
                }
            } else {
                Log(MqttHandlerLogLevel::kInfo, "product catalog miss for barcode=" + barcode->barcode);
            }
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
        if (catalog_product_message && qt_event_handler_ && !qt_event_handler_(*catalog_product_message)) {
            Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO publish failed");
            return false;
        }
        bool route_succeeded = true;
        if (parsed_topic.kind == mqtt::TopicKind::kQtRequest && command_route_handler_) {
            route_succeeded = command_route_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceResponse && qt_response_handler_) {
            route_succeeded = qt_response_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceStatus && qt_status_handler_) {
            route_succeeded = qt_status_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceHeartbeat && qt_status_handler_) {
            const auto* heartbeat = mqtt::GetPayload<mqtt::HeartbeatPayload>(decoded.value);
            if (heartbeat == nullptr) {
                route_succeeded = false;
            } else {
                const mqtt::MqttMessage status_message{
                    .protocol_version = decoded.value.protocol_version,
                    .message_id = decoded.value.message_id,
                    .message_type = mqtt::MessageType::kDeviceStatus,
                    .source_id = decoded.value.source_id,
                    .timestamp = decoded.value.timestamp,
                    .data =
                        mqtt::DeviceStatusPayload{
                            .status = heartbeat->status,
                            .current_state = heartbeat->current_state,
                            .job_id = heartbeat->job_id,
                            .error_code = heartbeat->error_code,
                        },
                };
                route_succeeded = qt_status_handler_(status_message);
            }
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceError && qt_error_handler_) {
            route_succeeded = qt_error_handler_(decoded.value);
        }
        if (!route_succeeded) {
            Log(MqttHandlerLogLevel::kError, "MQTT message routing failed");
            return false;
        }
    } else if (process_message_handler_ && !process_message_handler_(decoded.value)) {
        Log(MqttHandlerLogLevel::kError, "MQTT process transition commit failed");
        return false;
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

bool MqttHandler::CheckHeartbeatTimeouts(std::string_view checked_at) {
    const std::string effective_checked_at = checked_at.empty() ? CurrentIso8601Timestamp() : std::string(checked_at);
    const auto changes = device_manager_.CheckHeartbeatTimeouts(effective_checked_at);
    if (!device_manager_.LastError().empty()) {
        Log(MqttHandlerLogLevel::kError, "device registry timeout update failed: " + device_manager_.LastError());
    }

    bool published = true;
    for (const auto& device : changes) {
        const mqtt::MqttMessage status_message{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "HEARTBEAT-" + std::string(mqtt::ToString(device.connection_state)) + "-" + device.device_id +
                          "-" + std::to_string(++timeout_message_sequence_),
            .message_type = mqtt::MessageType::kDeviceStatus,
            .source_id = device.device_id,
            .timestamp = effective_checked_at,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = device.connection_state,
                    .current_state = device.current_state.empty() ? "UNKNOWN" : device.current_state,
                    .job_id = device.job_id,
                    .error_code = device.connection_state == mqtt::ConnectionState::kOffline
                                      ? std::optional<std::string>("ERR-HEARTBEAT-TIMEOUT")
                                      : device.error_code,
                },
        };
        if (process_message_guard_ && !process_message_guard_(status_message)) {
            published = false;
            Log(MqttHandlerLogLevel::kError, "heartbeat timeout process transition rejected for " + device.device_id);
            continue;
        }
        if (process_message_handler_ && !process_message_handler_(status_message)) {
            published = false;
            Log(MqttHandlerLogLevel::kError,
                "heartbeat timeout process transition commit failed for " + device.device_id);
            continue;
        }
        if (qt_status_handler_ && !qt_status_handler_(status_message)) {
            published = false;
            Log(MqttHandlerLogLevel::kError, "heartbeat timeout status publish failed for " + device.device_id);
            continue;
        }
        Log(MqttHandlerLogLevel::kInfo, "device heartbeat state changed: " + device.device_id +
                                            "; status=" + std::string(mqtt::ToString(device.connection_state)));
    }
    return published;
}

void MqttHandler::Log(MqttHandlerLogLevel level, std::string_view message) const {
    if (logger_) {
        logger_(level, message);
    }
}

}  // namespace logistics::central_server
