#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/process.hpp"
#include "logistics/contracts/uart_codec.hpp"

namespace mqtt = logistics::contracts::mqtt;
namespace uart = logistics::contracts::uart;

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

uart::UartCommand ResolveTestUartCommand(const mqtt::MqttMessage& message) {
    if (message.message_type == mqtt::MessageType::kDestinationSet) {
        return uart::UartCommand::kGateSet;
    }

    if (message.message_type != mqtt::MessageType::kControlCommand) {
        return uart::UartCommand::kUnknown;
    }

    const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(message);

    if (payload == nullptr) {
        return uart::UartCommand::kUnknown;
    }

    if (payload->command == mqtt::ControlCommand::kStart && payload->component_id == "CONVEYOR-MOTOR-01") {
        return uart::UartCommand::kConveyorStart;
    }

    if (payload->command == mqtt::ControlCommand::kStop && payload->component_id == "CONVEYOR-MOTOR-01") {
        return uart::UartCommand::kConveyorStop;
    }

    return uart::UartCommand::kUnknown;
}

bool BuildTestUartPayload(const mqtt::MqttMessage& message, uart::UartCommand command,
                          std::vector<std::uint8_t>& output_payload) {
    output_payload.clear();

    if (command == uart::UartCommand::kConveyorStart) {
        const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(message);

        if (payload == nullptr || !payload->params.is_object() || !payload->params.contains("speed") ||
            !payload->params.at("speed").is_number_integer()) {
            return false;
        }

        const int speed = payload->params.at("speed").get<int>();

        if (speed < 0 || speed > 100) {
            return false;
        }

        output_payload.push_back(static_cast<std::uint8_t>(speed));
        return true;
    }

    if (command == uart::UartCommand::kGateSet) {
        const auto* payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(message);

        if (payload == nullptr || payload->destination != "DEST-01") {
            return false;
        }

        output_payload.push_back(0x01U);
        return true;
    }

    return true;
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

void TestMqttTimestampValidation() {
    assert(mqtt::IsValidIso8601Timestamp("2026-08-20T05:31:30Z"));
    assert(mqtt::IsValidIso8601Timestamp("2026-08-20T14:31:30+09:00"));
    assert(mqtt::IsValidIso8601Timestamp("2026-08-20T14:31:30.123+09:00"));

    assert(!mqtt::IsValidIso8601Timestamp("2026-08-20T14:31:30"));
    assert(!mqtt::IsValidIso8601Timestamp("2026/08/20 14:31:30"));
    assert(!mqtt::IsValidIso8601Timestamp("2026-13-20T14:31:30+09:00"));

    const auto invalid_timestamp = mqtt::DeserializeMessage(R"json(
        {
            "protocolVersion": "1.0",
            "messageId": "MSG-BAD-TIME-01",
            "messageType": "HEARTBEAT",
            "sourceId": "PI-01",
            "timestamp": "2026-08-20T14:31:30",
            "data": {
                "status": "ONLINE",
                "currentState": "IDLE",
                "uptime": 3600,
                "jobId": null,
                "errorCode": null
            }
        }
    )json");

    assert(!invalid_timestamp.IsSuccess());
    assert(invalid_timestamp.status.error == mqtt::CodecError::kInvalidFieldValue);
    assert(invalid_timestamp.status.field == "timestamp");
}

void TestUartEnumConversions() {
    assert(uart::UartCommandFromCode(static_cast<std::uint8_t>(uart::UartCommand::kConveyorStart)) ==
           uart::UartCommand::kConveyorStart);
    assert(uart::UartCommandFromCode(0xFEU) == uart::UartCommand::kUnknown);

    assert(uart::UartResultFromCode(0x00U) == uart::UartResult::kSuccess);
    assert(uart::UartResultFromCode(0x14U) == uart::UartResult::kHardwareError);
    assert(uart::UartResultFromCode(0xFEU) == uart::UartResult::kUnknown);

    assert(uart::UartErrorCodeFromCode(0x00U) == uart::UartErrorCode::kNone);
    assert(uart::UartErrorCodeFromCode(0x15U) == uart::UartErrorCode::kCrcError);
    assert(uart::UartErrorCodeFromCode(0xFEU) == uart::UartErrorCode::kUnknown);

    assert(uart::ToString(uart::UartCommand::kCommandResult) == "COMMAND_RESULT");
    assert(uart::ToString(uart::UartResult::kProcessing) == "RESULT_PROCESSING");
    assert(uart::ToString(uart::UartErrorCode::kHardwareError) == "ERR_HARDWARE_ERROR");
}

void TestUartCrc16() {
    constexpr std::array<std::uint8_t, 9> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };

    assert(uart::CalculateCrc16(input.data(), input.size()) == 0x29B1U);
}

void TestUartFrameRoundTrip() {
    const uart::UartFrame original{
        .version = uart::wire::kProtocolVersion,
        .sequence = 0x2AU,
        .command = uart::UartCommand::kConveyorStart,
        .payload = { 80U },
    };

    const auto encoded = uart::EncodeFrame(original);

    assert(encoded.IsSuccess());
    assert(encoded.bytes.size() == original.payload.size() + 7U);
    assert(encoded.bytes[0] == uart::wire::kSof);
    assert(encoded.bytes[1] == uart::wire::kProtocolVersion);
    assert(encoded.bytes[2] == original.sequence);
    assert(encoded.bytes[3] == static_cast<std::uint8_t>(original.command));
    assert(encoded.bytes[4] == original.payload.size());

    const auto decoded = uart::DecodeFrame(encoded.bytes);

    assert(decoded.IsSuccess());
    assert(decoded.value.version == original.version);
    assert(decoded.value.sequence == original.sequence);
    assert(decoded.value.command == original.command);
    assert(decoded.value.payload == original.payload);
}

void TestUartInvalidFrames() {
    const uart::UartFrame original{
        .version = uart::wire::kProtocolVersion,
        .sequence = 0x2AU,
        .command = uart::UartCommand::kStatusRequest,
        .payload = {},
    };

    const auto encoded = uart::EncodeFrame(original);
    assert(encoded.IsSuccess());

    {
        const std::vector<std::uint8_t> too_short{ uart::wire::kSof, uart::wire::kProtocolVersion };

        const auto decoded = uart::DecodeFrame(too_short);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kFrameTooShort);
    }

    {
        auto bytes = encoded.bytes;
        bytes[0] = 0x00U;

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kInvalidSof);
    }

    {
        auto bytes = encoded.bytes;
        bytes[1] = static_cast<std::uint8_t>(uart::wire::kProtocolVersion + 1U);

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kUnsupportedVersion);
    }

    {
        auto bytes = encoded.bytes;
        bytes[4] = 1U;

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kInvalidLength);
    }

    {
        auto bytes = encoded.bytes;
        bytes[3] = 0xFEU;

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kUnknownCommand);
    }

    {
        auto bytes = encoded.bytes;
        bytes.back() ^= 0x01U;

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kCrcMismatch);
    }

    {
        auto bytes = encoded.bytes;
        bytes.push_back(0x00U);

        const auto decoded = uart::DecodeFrame(bytes);
        assert(!decoded.IsSuccess());
        assert(decoded.status.error == uart::CodecError::kTrailingBytes);
    }

    {
        uart::UartFrame oversized{
            .version = uart::wire::kProtocolVersion,
            .sequence = 0x2AU,
            .command = uart::UartCommand::kConveyorStart,
            .payload = std::vector<std::uint8_t>(uart::wire::kMaximumPayloadSize + 1U, 0U),
        };

        const auto oversized_result = uart::EncodeFrame(oversized);
        assert(!oversized_result.IsSuccess());
        assert(oversized_result.status.error == uart::CodecError::kPayloadTooLarge);
    }
}

void TestUartCommandResultPayload() {
    {
        const uart::CommandResultPayload original{
            .original_command = uart::UartCommand::kConveyorStart,
            .result = uart::UartResult::kSuccess,
            .error_code = uart::UartErrorCode::kNone,
        };

        const auto encoded = uart::EncodeCommandResultPayload(original);

        assert(encoded.IsSuccess());
        assert(encoded.bytes.size() == 3U);

        const auto decoded = uart::DecodeCommandResultPayload(encoded.bytes);

        assert(decoded.IsSuccess());
        assert(decoded.value.original_command == original.original_command);
        assert(decoded.value.result == original.result);
        assert(decoded.value.error_code == original.error_code);
    }

    {
        const uart::CommandResultPayload original{
            .original_command = uart::UartCommand::kDeliveryStart,
            .result = uart::UartResult::kHardwareError,
            .error_code = uart::UartErrorCode::kHardwareError,
        };

        const auto encoded = uart::EncodeCommandResultPayload(original);

        assert(encoded.IsSuccess());

        const auto decoded = uart::DecodeCommandResultPayload(encoded.bytes);

        assert(decoded.IsSuccess());
        assert(decoded.value.result == uart::UartResult::kHardwareError);
        assert(decoded.value.error_code == uart::UartErrorCode::kHardwareError);
    }

    {
        const uart::CommandResultPayload invalid{
            .original_command = uart::UartCommand::kConveyorStart,
            .result = uart::UartResult::kHardwareError,
            .error_code = uart::UartErrorCode::kNone,
        };

        const auto encoded = uart::EncodeCommandResultPayload(invalid);

        assert(!encoded.IsSuccess());
        assert(encoded.status.error == uart::CodecError::kInvalidPayload);
    }
}

void TestMqttToUartConversion() {
    {
        const mqtt::MqttMessage message = MakeMessage("MSG-BRIDGE-01", mqtt::MessageType::kControlCommand,
                                                      mqtt::ControlCommandPayload{
                                                          .request_id = "REQ-BRIDGE-01",
                                                          .command = mqtt::ControlCommand::kStatusRequest,
                                                          .target_device_id = "PI-01",
                                                          .component_id = "",
                                                          .params = mqtt::Json::object(),
                                                      },
                                                      "SERVER-01");

        const auto converted = uart::bridge::ConvertMqttToUart(message, 0x2AU);

        assert(converted.IsSuccess());
        assert(converted.frame.sequence == 0x2AU);
        assert(converted.frame.command == uart::UartCommand::kStatusRequest);
        assert(converted.frame.payload.empty());
    }

    {
        const mqtt::MqttMessage message =
            MakeMessage("MSG-BRIDGE-02",
                        mqtt::MessageType::kControlCommand,
                        mqtt::ControlCommandPayload{
                            .request_id = "REQ-BRIDGE-02",
                            .command = mqtt::ControlCommand::kStart,
                            .target_device_id = "PI-01",
                            .component_id = "CONVEYOR-MOTOR-01",
                            .params =
                                {
                                    {"speed", 50},
                                },
                        },
                        "SERVER-01");

        const auto converted =
            uart::bridge::ConvertMqttToUart(message, 0x2BU, ResolveTestUartCommand, BuildTestUartPayload);

        assert(converted.IsSuccess());
        assert(converted.frame.sequence == 0x2BU);
        assert(converted.frame.command == uart::UartCommand::kConveyorStart);
        assert(converted.frame.payload == std::vector<std::uint8_t>{ 50U });

        const auto encoded = uart::EncodeFrame(converted.frame);
        assert(encoded.IsSuccess());
    }

    {
        const mqtt::MqttMessage message =
            MakeMessage("MSG-BRIDGE-03",
                        mqtt::MessageType::kControlCommand,
                        mqtt::ControlCommandPayload{
                            .request_id = "REQ-BRIDGE-03",
                            .command = mqtt::ControlCommand::kStart,
                            .target_device_id = "PI-01",
                            .component_id = "CONVEYOR-MOTOR-01",
                            .params =
                                {
                                    {"speed", 101},
                                },
                        },
                        "SERVER-01");

        const auto converted =
            uart::bridge::ConvertMqttToUart(message, 0x2CU, ResolveTestUartCommand, BuildTestUartPayload);

        assert(!converted.IsSuccess());
        assert(converted.status.error == uart::bridge::Error::kInvalidPayload);
    }
}

void TestUartToMqttConversion() {
    {
        const uart::CommandResultPayload command_result{
            .original_command = uart::UartCommand::kStatusRequest,
            .result = uart::UartResult::kSuccess,
            .error_code = uart::UartErrorCode::kNone,
        };

        const auto encoded_payload = uart::EncodeCommandResultPayload(command_result);
        assert(encoded_payload.IsSuccess());

        const uart::UartFrame frame{
            .version = uart::wire::kProtocolVersion,
            .sequence = 0x2AU,
            .command = uart::UartCommand::kCommandResult,
            .payload = encoded_payload.bytes,
        };

        const uart::bridge::PendingRequestContext context{
            .request_id = "REQ-BRIDGE-01",
            .mqtt_command = mqtt::ControlCommand::kStatusRequest,
        };

        const auto converted = uart::bridge::ConvertUartCommandResultToMqtt(frame, context, "MSG-BRIDGE-RESPONSE-01",
                                                                            "PI-01", "2026-08-20T14:31:30+09:00");

        assert(converted.IsSuccess());
        assert(converted.message.message_type == mqtt::MessageType::kCommandResponse);

        const auto* payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(converted.message);

        assert(payload != nullptr);
        assert(payload->request_id == context.request_id);
        assert(payload->command == context.mqtt_command);
        assert(payload->result == mqtt::CommandResult::kSuccess);
        assert(!payload->error_code.has_value());

        const auto serialized = mqtt::SerializeMessage(converted.message);
        assert(serialized.IsSuccess());
    }

    {
        const uart::CommandResultPayload command_result{
            .original_command = uart::UartCommand::kConveyorStart,
            .result = uart::UartResult::kHardwareError,
            .error_code = uart::UartErrorCode::kHardwareError,
        };

        const auto encoded_payload = uart::EncodeCommandResultPayload(command_result);
        assert(encoded_payload.IsSuccess());

        const uart::UartFrame frame{
            .version = uart::wire::kProtocolVersion,
            .sequence = 0x2BU,
            .command = uart::UartCommand::kCommandResult,
            .payload = encoded_payload.bytes,
        };

        const uart::bridge::PendingRequestContext context{
            .request_id = "REQ-BRIDGE-02",
            .mqtt_command = mqtt::ControlCommand::kStart,
        };

        const auto converted = uart::bridge::ConvertUartCommandResultToMqtt(frame, context, "MSG-BRIDGE-RESPONSE-02",
                                                                            "PI-01", "2026-08-20T14:31:30+09:00");

        assert(converted.IsSuccess());

        const auto* payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(converted.message);

        assert(payload != nullptr);
        assert(payload->result == mqtt::CommandResult::kFailed);
        assert(payload->error_code.has_value());
        assert(*payload->error_code == "ERR_HARDWARE_ERROR");
    }

    {
        const uart::UartFrame frame{
            .version = uart::wire::kProtocolVersion,
            .sequence = 0x2CU,
            .command = uart::UartCommand::kStatusRequest,
            .payload = {},
        };

        const uart::bridge::PendingRequestContext context{
            .request_id = "REQ-BRIDGE-03",
            .mqtt_command = mqtt::ControlCommand::kStatusRequest,
        };

        const auto converted = uart::bridge::ConvertUartCommandResultToMqtt(frame, context, "MSG-BRIDGE-RESPONSE-03",
                                                                            "PI-01", "2026-08-20T14:31:30+09:00");

        assert(!converted.IsSuccess());
        assert(converted.status.error == uart::bridge::Error::kUnexpectedUartCommand);
    }
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
    static_assert(uart::wire::kResponseTimeoutMilliseconds == 500U);
    static_assert(uart::wire::kMaximumRetries == 3U);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 9 }) == mqtt::ConnectionState::kOnline);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 10 }) == mqtt::ConnectionState::kDelayed);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 15 }) == mqtt::ConnectionState::kOffline);

    TestAllMqttMessageRoundTrips();
    TestMqttCodecInvalidInputs();
    TestMqttTimestampValidation();
    TestUartEnumConversions();
    TestUartCrc16();
    TestUartFrameRoundTrip();
    TestUartInvalidFrames();
    TestUartCommandResultPayload();
    TestMqttToUartConversion();
    TestUartToMqttConversion();

    return 0;
}
