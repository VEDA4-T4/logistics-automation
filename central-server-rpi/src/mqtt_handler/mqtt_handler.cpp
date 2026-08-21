#include "logistics/central_server/mqtt_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"
#include "logistics/contracts/uart/linetracer_commands.h"

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
    return type == mqtt::MessageType::kPositionDetected || type == mqtt::MessageType::kBarcodeDetected ||
           type == mqtt::MessageType::kProductImage || type == mqtt::MessageType::kProductInfo ||
           type == mqtt::MessageType::kDestinationSet || type == mqtt::MessageType::kWorkCompleted ||
           type == mqtt::MessageType::kSensorStatus;
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

[[nodiscard]] mqtt::DeviceStatusPayload MakeDeviceStatusPayload(const DeviceSnapshot& device) {
    return {
        .status = device.connection_state,
        .current_state = device.current_state.empty() ? "UNKNOWN" : device.current_state,
        .job_id = device.job_id,
        .error_code = device.error_code,
        .departure_position = device.departure_position,
        .target_position = device.target_position,
        .confirmed_position = device.confirmed_position,
        .movement_state = device.movement_state,
        .position_reset = device.position_reset,
    };
}

}  // namespace

MqttHandler::MqttHandler(DeviceManager& device_manager, Logger logger, PersistenceService* persistence_service,
                         std::string default_destination, SensorDetectionConfig sensor_detection)
    : device_manager_(device_manager),
      logger_(logger ? std::move(logger) : Logger(DefaultLog)),
      persistence_service_(persistence_service),
      default_destination_(std::move(default_destination)),
      sensor_detector_(std::move(sensor_detection)) {}

void MqttHandler::SetWorkCreatedHandler(WorkCreatedHandler handler) {
    work_created_handler_ = std::move(handler);
}

void MqttHandler::SetWorkCreationSourceGuard(WorkCreationSourceGuard guard) {
    work_creation_source_guard_ = std::move(guard);
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

void MqttHandler::SetProcessEpoch(std::string process_epoch, bool reject_legacy_work_messages) {
    if (!mqtt::IsValidUuid(process_epoch)) {
        throw std::invalid_argument("process epoch must be a UUID");
    }
    process_epoch_ = std::move(process_epoch);
    reject_legacy_work_messages_ = reject_legacy_work_messages;
}

bool MqttHandler::Handle(std::string_view topic, std::string_view payload, std::string_view received_at, int qos,
                         bool retained) {
    return HandleMessage(topic, payload, received_at, qos, retained, false);
}

bool MqttHandler::HandleMessage(std::string_view topic, std::string_view payload, std::string_view received_at, int qos,
                                bool retained, bool replaying) {
    auto decoded = mqtt::DeserializeMessage(payload);
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

    if (const auto* sensor = mqtt::GetPayload<mqtt::SensorStatusPayload>(decoded.value);
        sensor != nullptr && sensor->sensor_id == UART_LINETRACER_RETIRED_REAR_SENSOR_ID) {
        const auto device = device_manager_.FindDevice(decoded.value.source_id);
        const auto role = device.has_value() ? contracts::DeviceRoleFromString(device->device_type) : std::nullopt;
        if (role == contracts::DeviceRole::kLineTracer) {
            Log(MqttHandlerLogLevel::kInfo,
                "ignored retired rear ultrasonic sensor data from line tracer; source=" + decoded.value.source_id);
            return true;
        }
    }

    // The controller reports a distance and whether that reading is trustworthy;
    // it no longer decides whether a box is there. Derive that here, before
    // persistence and before any routing, so the stored event and every
    // downstream consumer agree on one value - and so retuning detection is a
    // server.ini edit rather than an STM32 reflash.
    bool detection_stamped = false;
    std::string replay_payload(payload);
    if (auto* sensor = mqtt::GetPayload<mqtt::SensorStatusPayload>(decoded.value);
        sensor != nullptr && (!replaying || !sensor->detection_status.has_value())) {
        auto detection = sensor_detector_.Evaluate(decoded.value.source_id, sensor->sensor_id,
                                                   sensor->measurement_status, sensor->distance_cm);
        if (detection.has_value()) {
            sensor->detection_status = std::move(detection);
            detection_stamped = true;
        }
    }

    const auto parsed_topic = mqtt::ParseTopic(topic);
    bool device_registry_updated = false;
    if (!process_epoch_.empty() && mqtt::IsProcessScopedMessage(decoded.value) &&
        ((!decoded.value.process_epoch.has_value() && reject_legacy_work_messages_) ||
         (decoded.value.process_epoch.has_value() && *decoded.value.process_epoch != process_epoch_))) {
        if (persistence_service_ != nullptr) {
            const auto root = mqtt::Json::parse(payload.begin(), payload.end());
            const std::string details_json = root.at(std::string(mqtt::kDataField)).dump();
            const mqtt::EnvelopeView envelope{
                .protocol_version = decoded.value.protocol_version,
                .message_id = decoded.value.message_id,
                .message_type = decoded.value.message_type,
                .source_id = decoded.value.source_id,
                .timestamp = decoded.value.timestamp,
                .process_epoch =
                    decoded.value.process_epoch ? std::string_view(*decoded.value.process_epoch) : std::string_view{},
                .data_json = details_json,
            };
            const TransportMetadata transport{
                .topic = std::string(topic),
                .qos = qos,
                .retained = retained,
                .received_at_ms = CurrentUnixTimeMilliseconds(),
                .source_address = {},
                .raw_payload = std::string(payload),
            };
            const auto status = persistence_service_->RejectValidatedEvent(envelope, transport, "REJECTED_STALE_EPOCH");
            if (!status.ok()) {
                Log(MqttHandlerLogLevel::kError, "stale process epoch rejection persistence failed: " + status.message);
                return false;
            }
        }
        Log(MqttHandlerLogLevel::kInfo, "MQTT process message rejected: REJECTED_STALE_EPOCH");
        return true;
    }
    if (decoded.value.message_type == mqtt::MessageType::kBoxDetected && work_creation_source_guard_ &&
        !work_creation_source_guard_(decoded.value.source_id)) {
        if (persistence_service_ != nullptr) {
            const auto root = mqtt::Json::parse(payload.begin(), payload.end());
            const std::string details_json = root.at(std::string(mqtt::kDataField)).dump();
            const mqtt::EnvelopeView envelope{
                .protocol_version = decoded.value.protocol_version,
                .message_id = decoded.value.message_id,
                .message_type = decoded.value.message_type,
                .source_id = decoded.value.source_id,
                .timestamp = decoded.value.timestamp,
                .process_epoch =
                    decoded.value.process_epoch ? std::string_view(*decoded.value.process_epoch) : std::string_view{},
                .data_json = details_json,
            };
            const TransportMetadata transport{
                .topic = std::string(topic),
                .qos = qos,
                .retained = retained,
                .received_at_ms = CurrentUnixTimeMilliseconds(),
                .source_address = {},
                .raw_payload = std::string(payload),
            };
            const auto status = persistence_service_->RejectValidatedEvent(envelope, transport,
                                                                           "REJECTED_NON_AUTHORITATIVE_WORK_SOURCE");
            if (!status.ok()) {
                Log(MqttHandlerLogLevel::kError,
                    "non-authoritative BOX_DETECTED rejection persistence failed: " + status.message);
                return false;
            }
        }
        Log(MqttHandlerLogLevel::kInfo,
            "ignored BOX_DETECTED from non-authoritative work source=" + decoded.value.source_id);
        return true;
    }
    // Vision measurements are transient observations.  They have no work id
    // until the central input ultrasonic gate binds them to an active work, so
    // applying the normal process preview here would reject every observation
    // before the application-level binding handler can see it.
    if (decoded.value.message_type != mqtt::MessageType::kVisionMeasurement && process_message_guard_ &&
        !process_message_guard_(decoded.value)) {
        Log(MqttHandlerLogLevel::kError, "MQTT process transition rejected before persistence");
        return false;
    }
    if (!replaying && decoded.value.message_type == mqtt::MessageType::kSensorStatus) {
        if (process_message_handler_ && !process_message_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "sensor telemetry process handling failed");
            return false;
        }
        if (qt_event_handler_ && !qt_event_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "sensor telemetry Qt routing failed");
            return false;
        }
        return true;
    }
    if (!replaying && decoded.value.message_type == mqtt::MessageType::kVisionMeasurement) {
        if (process_message_handler_ && !process_message_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "vision measurement process handling failed");
            return false;
        }
        return true;
    }
    if (persistence_service_ != nullptr) {
        const auto root = mqtt::Json::parse(payload.begin(), payload.end());
        std::string details_json = root.at(std::string(mqtt::kDataField)).dump();
        if (detection_stamped) {
            // Re-render from the stamped message so the persisted event carries
            // detectionStatus too. transport.raw_payload below still keeps the
            // untouched wire bytes.
            const auto restamped = mqtt::SerializeMessage(decoded.value);
            if (!restamped.IsSuccess()) {
                Log(MqttHandlerLogLevel::kError, "SENSOR_STATUS re-serialization failed after detection stamping");
                return false;
            }
            replay_payload = restamped.payload;
            details_json = mqtt::Json::parse(restamped.payload).at(std::string(mqtt::kDataField)).dump();
        }
        const mqtt::EnvelopeView envelope{
            .protocol_version = decoded.value.protocol_version,
            .message_id = decoded.value.message_id,
            .message_type = decoded.value.message_type,
            .source_id = decoded.value.source_id,
            .timestamp = decoded.value.timestamp,
            .process_epoch =
                decoded.value.process_epoch ? std::string_view(*decoded.value.process_epoch) : std::string_view{},
            .data_json = details_json,
        };
        const TransportMetadata transport{
            .topic = std::string(topic),
            .qos = qos,
            .retained = retained,
            .received_at_ms = CurrentUnixTimeMilliseconds(),
            .source_address = {},
            .raw_payload = std::move(replay_payload),
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
        if (!result.requires_side_effects) {
            Log(MqttHandlerLogLevel::kInfo, "MQTT message already completed: " + decoded.value.message_id);
            return true;
        }
        const auto mark_stored = [this](std::string_view message_id) {
            DatabaseStatus status;
            for (int attempt = 0; attempt < 3; ++attempt) {
                status = persistence_service_->MarkEventStored(message_id);
                if (status.ok() || !status.retryable()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
            }
            if (!status.ok()) {
                Log(MqttHandlerLogLevel::kError, "MQTT completion persistence failed: " + status.message);
            }
            return status.ok();
        };
        if (process_message_handler_ && !process_message_handler_(decoded.value)) {
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
            device_registry_updated = true;
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
            if (!catalog_product && !default_destination_.empty()) {
                Log(MqttHandlerLogLevel::kInfo, "product catalog miss; using default destination=" +
                                                    default_destination_ + "; barcode=" + barcode->barcode);
                catalog_product = CatalogProduct{
                    .barcode = barcode->barcode,
                    .product_id = "UNREGISTERED",
                    .product_name = "Unregistered product",
                    .destination = default_destination_,
                };
            }
            if (catalog_product) {
                catalog_product_message = mqtt::MqttMessage{
                    .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                    .message_id = "CATALOG-" + decoded.value.message_id,
                    .message_type = mqtt::MessageType::kProductInfo,
                    .source_id = "central-server",
                    .timestamp = decoded.value.timestamp,
                    .process_epoch = decoded.value.process_epoch,
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
                    .process_epoch = catalog_product_message->process_epoch
                                         ? std::string_view(*catalog_product_message->process_epoch)
                                         : std::string_view{},
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
                if (!product_result.requires_side_effects) {
                    catalog_product_message.reset();
                } else {
                    if (process_message_handler_ && !process_message_handler_(*catalog_product_message)) {
                        Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO process transition commit failed");
                        return false;
                    }
                }
            } else {
                Log(MqttHandlerLogLevel::kInfo, "product catalog miss for barcode=" + barcode->barcode);
            }
        }
        if (decoded.value.message_type == mqtt::MessageType::kBoxDetected && result.work_id && work_created_handler_) {
            const auto disposition = work_created_handler_(decoded.value.source_id, *result.work_id);
            if (disposition == WorkCreationDisposition::kFailed) {
                Log(MqttHandlerLogLevel::kError, "WORK_CREATED publish failed for workId=" + *result.work_id);
                return false;
            }
        }
        if (IsQtProductEvent(decoded.value.message_type) && qt_event_handler_ && !qt_event_handler_(decoded.value)) {
            Log(MqttHandlerLogLevel::kError, "Qt product event publish failed");
            return false;
        }
        if (catalog_product_message && qt_event_handler_ && !qt_event_handler_(*catalog_product_message)) {
            Log(MqttHandlerLogLevel::kError, "catalog PRODUCT_INFO publish failed");
            return false;
        }
        if (catalog_product_message && !mark_stored(catalog_product_message->message_id)) {
            return false;
        }
        bool route_succeeded = true;
        if (parsed_topic.kind == mqtt::TopicKind::kQtRequest && command_route_handler_) {
            route_succeeded = command_route_handler_(decoded.value);
        } else if (parsed_topic.kind == mqtt::TopicKind::kDeviceResponse && qt_response_handler_) {
            route_succeeded = qt_response_handler_(decoded.value);
        } else if ((parsed_topic.kind == mqtt::TopicKind::kDeviceStatus ||
                    parsed_topic.kind == mqtt::TopicKind::kDeviceHeartbeat) &&
                   qt_status_handler_) {
            const auto device = device_manager_.FindDevice(parsed_topic.endpoint_id);
            if (!device.has_value()) {
                route_succeeded = false;
            } else {
                const mqtt::MqttMessage status_message{
                    .protocol_version = decoded.value.protocol_version,
                    .message_id = decoded.value.message_id,
                    .message_type = mqtt::MessageType::kDeviceStatus,
                    .source_id = decoded.value.source_id,
                    .timestamp = decoded.value.timestamp,
                    .process_epoch = device->job_id.has_value() && !process_epoch_.empty()
                                         ? std::optional<std::string>(process_epoch_)
                                         : std::nullopt,
                    .data = MakeDeviceStatusPayload(*device),
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
        if (!mark_stored(decoded.value.message_id)) {
            return false;
        }
    } else if (process_message_handler_ && !process_message_handler_(decoded.value)) {
        Log(MqttHandlerLogLevel::kError, "MQTT process transition commit failed");
        return false;
    }
    if (IsDeviceRegistryMessage(decoded.value.message_type) && !device_registry_updated) {
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

bool MqttHandler::ReplayPendingReceivedEvents(std::size_t limit) {
    if (persistence_service_ == nullptr) {
        return false;
    }
    std::vector<PendingReceivedEvent> events;
    const auto status = persistence_service_->PendingReceivedEvents(events, limit);
    if (!status.ok()) {
        Log(MqttHandlerLogLevel::kError, "MQTT pending event load failed: " + status.message);
        return false;
    }
    for (const auto& event : events) {
        if (!HandleMessage(event.topic, event.raw_payload, {}, event.qos, event.retained, true)) {
            // A malformed/terminally rejected historical row must not starve newer
            // independent events.  Transient failures remain RECEIVED and are retried
            // next pump; continue scanning this batch either way.
            Log(MqttHandlerLogLevel::kError, "MQTT pending event replay failed; continuing with later events");
        }
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
            .process_epoch = device.job_id.has_value() && !process_epoch_.empty()
                                 ? std::optional<std::string>(process_epoch_)
                                 : std::nullopt,
            .data = MakeDeviceStatusPayload(device),
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

bool MqttHandler::ReplayDeviceStatuses(std::string_view target_device_id, std::string_view replayed_at) {
    if (!qt_status_handler_) {
        return false;
    }
    const bool replay_all = target_device_id == "ALL" || target_device_id == "SYSTEM";
    const std::string fallback_timestamp = replayed_at.empty() ? CurrentIso8601Timestamp() : std::string(replayed_at);
    bool matched = false;
    bool published = true;

    for (const auto& device : device_manager_.RegisteredDevices()) {
        if (!replay_all && device.device_id != target_device_id) {
            continue;
        }
        matched = true;
        const mqtt::MqttMessage status_message{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "SNAPSHOT-" + device.device_id + "-" + std::to_string(++replay_message_sequence_),
            .message_type = mqtt::MessageType::kDeviceStatus,
            .source_id = device.device_id,
            .timestamp = device.last_message_timestamp.empty() ? fallback_timestamp : device.last_message_timestamp,
            .process_epoch = device.job_id.has_value() && !process_epoch_.empty()
                                 ? std::optional<std::string>(process_epoch_)
                                 : std::nullopt,
            .data = MakeDeviceStatusPayload(device),
        };
        if (!qt_status_handler_(status_message)) {
            published = false;
            Log(MqttHandlerLogLevel::kError, "device snapshot replay failed for " + device.device_id);
        }
    }
    return matched && published;
}

void MqttHandler::Log(MqttHandlerLogLevel level, std::string_view message) const {
    if (logger_) {
        logger_(level, message);
    }
}

}  // namespace logistics::central_server
