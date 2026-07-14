#include <cassert>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/process.hpp"

namespace mqtt = logistics::contracts::mqtt;

namespace {

mqtt::MqttMessage MakeMessage(std::string message_id, mqtt::MessageType message_type, mqtt::MessagePayload payload,
                              std::string source_id = "PI-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = message_type,
        .source_id = std::move(source_id),
        .timestamp = "2026-08-20T14:31:30+09:00",
        .data = std::move(payload),
    };
}

template <typename PayloadType>
void AssertRoundTrip(const mqtt::MqttMessage& original) {
    const auto encoded = mqtt::SerializeMessage(original);

    assert(encoded.IsSuccess());
    assert(!encoded.payload.empty());

    const auto decoded = mqtt::DeserializeMessage(encoded.payload);

    assert(decoded.IsSuccess());
    assert(decoded.value.protocol_version == original.protocol_version);
    assert(decoded.value.message_id == original.message_id);
    assert(decoded.value.message_type == original.message_type);
    assert(decoded.value.source_id == original.source_id);
    assert(decoded.value.timestamp == original.timestamp);
    assert(mqtt::GetPayload<PayloadType>(decoded.value) != nullptr);

    const auto reencoded = mqtt::SerializeMessage(decoded.value);

    assert(reencoded.IsSuccess());

    const mqtt::Json first_json = mqtt::Json::parse(encoded.payload);
    const mqtt::Json second_json = mqtt::Json::parse(reencoded.payload);

    assert(first_json == second_json);
}

void TestAllMqttMessageRoundTrips() {
    AssertRoundTrip<mqtt::DeviceRegisterPayload>(MakeMessage("MSG-0001", mqtt::MessageType::kDeviceRegister,
                                                             mqtt::DeviceRegisterPayload{
                                                                 .device_type = "camera-node",
                                                                 .node_name = "camera-node-01",
                                                                 .status = mqtt::ConnectionState::kOnline,
                                                                 .ip_address = "192.168.0.21",
                                                                 .uart_connected = true,
                                                             }));

    AssertRoundTrip<mqtt::HeartbeatPayload>(MakeMessage("MSG-0002", mqtt::MessageType::kHeartbeat,
                                                        mqtt::HeartbeatPayload{
                                                            .status = mqtt::ConnectionState::kOnline,
                                                            .current_state = "IDLE",
                                                            .uptime = 3600,
                                                            .job_id = std::nullopt,
                                                            .error_code = std::nullopt,
                                                        }));

    AssertRoundTrip<mqtt::BoxDetectedPayload>(MakeMessage("MSG-0003", mqtt::MessageType::kBoxDetected,
                                                          mqtt::BoxDetectedPayload{
                                                              .job_id = "JOB-0001",
                                                              .detected = true,
                                                              .image_name = "JOB-0001-BOX.jpg",
                                                          }));

    AssertRoundTrip<mqtt::PositionDetectedPayload>(MakeMessage("MSG-0004", mqtt::MessageType::kPositionDetected,
                                                               mqtt::PositionDetectedPayload{
                                                                   .job_id = "JOB-0001",
                                                                   .box_x = 120,
                                                                   .box_y = 80,
                                                                   .box_width = 380,
                                                                   .box_height = 260,
                                                                   .center_x = 315,
                                                                   .center_y = 226,
                                                                   .offset_x = 5,
                                                                   .offset_y = -3,
                                                                   .position_status = "OK",
                                                               }));

    AssertRoundTrip<mqtt::BarcodeDetectedPayload>(MakeMessage("MSG-0005", mqtt::MessageType::kBarcodeDetected,
                                                              mqtt::BarcodeDetectedPayload{
                                                                  .job_id = "JOB-0001",
                                                                  .barcode = "8801234567890",
                                                                  .center_x = 315,
                                                                  .center_y = 226,
                                                                  .image_name = "JOB-0001.jpg",
                                                                  .image_path = "/data/images/JOB-0001.jpg",
                                                                  .result = "SUCCESS",
                                                              }));

    AssertRoundTrip<mqtt::ProductImagePayload>(
        MakeMessage(
            "MSG-0006",
            mqtt::MessageType::kProductImage,
            mqtt::ProductImagePayload{
                .job_id = "JOB-0001",
                .image_name = "JOB-0001.jpg",
                .image_path = "/data/images/JOB-0001.jpg",
                .metadata =
                    {
                        {"format", "JPEG"},
                        {"width", 1920},
                        {"height", 1080},
                        {"fileSize", 183742},
                    },
            }
        )
    );

    AssertRoundTrip<mqtt::ProductInfoPayload>(MakeMessage("MSG-0007", mqtt::MessageType::kProductInfo,
                                                          mqtt::ProductInfoPayload{
                                                              .job_id = "JOB-0001",
                                                              .barcode = "8801234567890",
                                                              .product_name = "Sample Product",
                                                              .category = "A",
                                                              .destination = "DEST-01",
                                                              .image_path = "/data/images/JOB-0001.jpg",
                                                          },
                                                          "SERVER-01"));

    AssertRoundTrip<mqtt::DestinationSetPayload>(MakeMessage("MSG-0008", mqtt::MessageType::kDestinationSet,
                                                             mqtt::DestinationSetPayload{
                                                                 .request_id = "REQ-0002",
                                                                 .job_id = "JOB-0001",
                                                                 .command = mqtt::ControlCommand::kDestinationSet,
                                                                 .target_device_id = "PI-LT-01",
                                                                 .destination = "DEST-01",
                                                             },
                                                             "SERVER-01"));

    AssertRoundTrip<mqtt::DeviceStatusPayload>(MakeMessage("MSG-0009", mqtt::MessageType::kDeviceStatus,
                                                           mqtt::DeviceStatusPayload{
                                                               .status = mqtt::ConnectionState::kOffline,
                                                               .current_state = "DISCONNECTED",
                                                               .job_id = std::nullopt,
                                                               .error_code = std::string("ERR-MQTT-DISCONNECTED"),
                                                           }));

    AssertRoundTrip<mqtt::ControlCommandPayload>(
        MakeMessage(
            "MSG-0010",
            mqtt::MessageType::kControlCommand,
            mqtt::ControlCommandPayload{
                .request_id = "REQ-0001",
                .command = mqtt::ControlCommand::kStart,
                .target_device_id = "PI-01",
                .component_id = "CONVEYOR-MOTOR-01",
                .params =
                    {
                        {"speed", 50},
                    },
            },
            "QT-01"
        )
    );

    AssertRoundTrip<mqtt::ErrorOccurredPayload>(MakeMessage("MSG-0011", mqtt::MessageType::kErrorOccurred,
                                                            mqtt::ErrorOccurredPayload{
                                                                .job_id = std::string("JOB-0001"),
                                                                .error_code = "ERR-OBSTACLE-FRONT",
                                                                .error_level = "WARNING",
                                                                .current_state = "MOVING-TO-DEST",
                                                                .message = "Front obstacle detected.",
                                                                .distance = 18,
                                                            }));

    AssertRoundTrip<mqtt::EmergencyStopPayload>(MakeMessage("MSG-0012", mqtt::MessageType::kEmergencyStop,
                                                            mqtt::EmergencyStopPayload{
                                                                .request_id = "REQ-0003",
                                                                .command = mqtt::ControlCommand::kEmergencyStop,
                                                                .target_device_id = "ALL",
                                                            },
                                                            "QT-01"));

    AssertRoundTrip<mqtt::CommandResponsePayload>(MakeMessage("MSG-0013", mqtt::MessageType::kCommandResponse,
                                                              mqtt::CommandResponsePayload{
                                                                  .request_id = "REQ-0002",
                                                                  .command = mqtt::ControlCommand::kDestinationSet,
                                                                  .result = mqtt::CommandResult::kSuccess,
                                                                  .error_code = std::nullopt,
                                                                  .message = "Destination information received.",
                                                              }));
}

void TestMqttCodecInvalidInputs() {
    const auto malformed_json = mqtt::DeserializeMessage("{");
    assert(!malformed_json.IsSuccess());
    assert(malformed_json.status.error == mqtt::CodecError::kMalformedJson);

    const auto unsupported_protocol = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "2.0",
            "messageId": "MSG-BAD-01",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "status": "ONLINE",
                "currentState": "IDLE",
                "uptime": 3600,
                "jobId": null,
                "errorCode": null
            }
        }
    )json");
    assert(!unsupported_protocol.IsSuccess());
    assert(unsupported_protocol.status.error == mqtt::CodecError::kUnsupportedProtocolVersion);

    const auto unknown_message_type = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-02",
            "messageType": "NOT-A-MESSAGE",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {}
        }
    )json");
    assert(!unknown_message_type.IsSuccess());
    assert(unknown_message_type.status.error == mqtt::CodecError::kUnknownMessageType);

    const auto missing_data = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-03",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00"
        }
    )json");
    assert(!missing_data.IsSuccess());
    assert(missing_data.status.error == mqtt::CodecError::kMissingField);
    assert(missing_data.status.field == "data");

    const auto invalid_uptime_type = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-04",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "status": "ONLINE",
                "currentState": "IDLE",
                "uptime": "3600",
                "jobId": null,
                "errorCode": null
            }
        }
    )json");
    assert(!invalid_uptime_type.IsSuccess());
    assert(invalid_uptime_type.status.error == mqtt::CodecError::kInvalidFieldType);
    assert(invalid_uptime_type.status.field == "uptime");

    const auto negative_uptime = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-05",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "status": "ONLINE",
                "currentState": "IDLE",
                "uptime": -1,
                "jobId": null,
                "errorCode": null
            }
        }
    )json");
    assert(!negative_uptime.IsSuccess());
    assert(negative_uptime.status.error == mqtt::CodecError::kInvalidFieldValue);
    assert(negative_uptime.status.field == "uptime");

    const auto unknown_connection_state = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-06",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "status": "BROKEN",
                "currentState": "IDLE",
                "uptime": 3600,
                "jobId": null,
                "errorCode": null
            }
        }
    )json");
    assert(!unknown_connection_state.IsSuccess());
    assert(unknown_connection_state.status.error == mqtt::CodecError::kUnknownConnectionState);

    const auto unknown_command = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-07",
            "messageType": "CONTROL_COMMAND",
            "sourceId": "QT-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "requestId": "REQ-0001",
                "command": "FLY",
                "targetDeviceId": "PI-01",
                "componentId": "CONVEYOR-MOTOR-01",
                "params": {}
            }
        }
    )json");
    assert(!unknown_command.IsSuccess());
    assert(unknown_command.status.error == mqtt::CodecError::kUnknownCommand);

    const auto unknown_result = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-08",
            "messageType": "COMMAND_RESPONSE",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30+09:00",
            "data": {
                "requestId": "REQ-0001",
                "command": "START",
                "result": "MAYBE",
                "errorCode": null,
                "message": "Unknown result."
            }
        }
    )json");
    assert(!unknown_result.IsSuccess());
    assert(unknown_result.status.error == mqtt::CodecError::kUnknownCommandResult);

    const mqtt::MqttMessage mismatched_message = MakeMessage("MSG-BAD-09", mqtt::MessageType::kHeartbeat,
                                                             mqtt::BoxDetectedPayload{
                                                                 .job_id = "JOB-0001",
                                                                 .detected = true,
                                                                 .image_name = "JOB-0001.jpg",
                                                             });

    const auto mismatched_result = mqtt::SerializeMessage(mismatched_message);
    assert(!mismatched_result.IsSuccess());
    assert(mismatched_result.status.error == mqtt::CodecError::kUnexpectedPayloadType);
}

}  // namespace

int main() {
    using logistics::contracts::DeviceRole;
    using logistics::contracts::ProcessState;

    assert(logistics::contracts::ToString(DeviceRole::kVision) == "vision");
    assert(logistics::contracts::ToString(ProcessState::kEmergencyStop) == "ESTOP");

    assert(mqtt::QtRequestTopic("QT-01") == "server/request/QT-01");
    assert(mqtt::QtResponseTopic("QT-01") == "qt/QT-01/response");
    assert(mqtt::DeviceCommandTopic("PI-LT-01") == "device/PI-LT-01/command");
    assert(mqtt::DeviceHeartbeatTopic("PI-01") == "device/PI-01/heartbeat");

    const auto qt_request = mqtt::ParseTopic("server/request/QT-01");
    assert(qt_request.kind == mqtt::TopicKind::kQtRequest);
    assert(qt_request.endpoint_id == "QT-01");

    const auto qt_status = mqtt::ParseTopic("qt/QT-01/status");
    assert(qt_status.kind == mqtt::TopicKind::kQtStatus);
    assert(qt_status.endpoint_id == "QT-01");

    const auto device_event = mqtt::ParseTopic("device/PI-01/event");
    assert(device_event.kind == mqtt::TopicKind::kDeviceEvent);
    assert(device_event.endpoint_id == "PI-01");
    assert(!mqtt::ParseTopic("device/+/event").IsValid());
    assert(!mqtt::ParseTopic("device/PI-01/unknown").IsValid());
    assert(mqtt::ParseTopic(mqtt::kSystemBroadcastCommandTopic).kind == mqtt::TopicKind::kSystemBroadcastCommand);
    assert(mqtt::ParseTopic(mqtt::kServerHeartbeatTopic).kind == mqtt::TopicKind::kServerHeartbeat);

    bool invalid_id_rejected = false;
    try {
        static_cast<void>(mqtt::DeviceStatusTopic("PI/01"));
    } catch (const std::invalid_argument&) {
        invalid_id_rejected = true;
    }
    assert(invalid_id_rejected);

    assert(mqtt::MessageTypeFromString("CONTROL_COMMAND") == mqtt::MessageType::kControlCommand);
    assert(mqtt::ControlCommandFromString("EMERGENCY_STOP") == mqtt::ControlCommand::kEmergencyStop);
    assert(mqtt::CommandResultFromString("RECEIVED") == mqtt::CommandResult::kReceived);
    assert(mqtt::ToString(mqtt::CommandResult::kDuplicated) == "DUPLICATED");
    assert(mqtt::IsTerminal(mqtt::CommandResult::kSuccess));
    assert(!mqtt::IsTerminal(mqtt::CommandResult::kProcessing));

    const auto heartbeat_policy = mqtt::PolicyFor(mqtt::MessageType::kHeartbeat);
    assert(heartbeat_policy.minimum_qos == mqtt::Qos::kAtMostOnce);
    assert(heartbeat_policy.retain == mqtt::RetainPolicy::kNever);

    const auto status_policy = mqtt::PolicyFor(mqtt::MessageType::kDeviceStatus);
    assert(status_policy.maximum_qos == mqtt::Qos::kAtLeastOnce);
    assert(status_policy.retain == mqtt::RetainPolicy::kLatestStateAllowed);

    constexpr mqtt::EnvelopeView valid_envelope{
        .protocol_version = mqtt::kCurrentProtocolVersion,
        .message_id = "MSG-0001",
        .message_type = mqtt::MessageType::kHeartbeat,
        .source_id = "PI-01",
        .timestamp = "2026-08-20T14:31:30+09:00",
        .data_json = "{}",
    };
    static_assert(valid_envelope.IsValid());

    constexpr mqtt::EnvelopeView invalid_envelope{
        .protocol_version = "2.0",
        .message_id = "MSG-0002",
        .message_type = mqtt::MessageType::kHeartbeat,
        .source_id = "PI-01",
        .timestamp = "2026-08-20T14:31:30+09:00",
        .data_json = "{}",
    };
    static_assert(!invalid_envelope.IsValid());

    constexpr mqtt::CommandRequestView command_request{
        .request_id = "REQ-0001",
        .command = mqtt::ControlCommand::kStart,
        .target_device = "PI-INPUT-01",
        .component_id = "CONVEYOR-MOTOR-01",
    };
    static_assert(command_request.IsValid());

    constexpr mqtt::CommandResponseView command_response{
        .request_id = "REQ-0001",
        .command = mqtt::ControlCommand::kStart,
        .result = mqtt::CommandResult::kReceived,
    };
    static_assert(command_response.IsValid());

    static_assert(mqtt::kHeartbeatInterval.count() == 5);
    static_assert(mqtt::kHeartbeatDelayedAfter.count() == 10);
    static_assert(mqtt::kHeartbeatOfflineAfter.count() == 15);
    static_assert(mqtt::kMqttMaximumRetries == 3);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 9 }) == mqtt::ConnectionState::kOnline);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 10 }) == mqtt::ConnectionState::kDelayed);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 15 }) == mqtt::ConnectionState::kOffline);

    TestAllMqttMessageRoundTrips();
    TestMqttCodecInvalidInputs();

    return 0;
}
