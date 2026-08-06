#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "logistics/contracts/http_upload.hpp"
#include "logistics/contracts/identifier.hpp"
#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::contracts::mqtt {

using Json = nlohmann::json;

inline constexpr std::string_view kDeviceTypeField = "deviceType";
inline constexpr std::string_view kNodeNameField = "nodeName";
inline constexpr std::string_view kStatusField = "status";
inline constexpr std::string_view kIpAddressField = "ipAddress";
inline constexpr std::string_view kUartConnectedField = "uartConnected";
inline constexpr std::string_view kCurrentStateField = "currentState";
inline constexpr std::string_view kUptimeField = "uptime";
inline constexpr std::string_view kJobIdField = "jobId";
inline constexpr std::string_view kErrorCodeField = "errorCode";
inline constexpr std::string_view kDeparturePositionField = "departurePosition";
inline constexpr std::string_view kTargetPositionField = "targetPosition";
inline constexpr std::string_view kConfirmedPositionField = "confirmedPosition";
inline constexpr std::string_view kMovementStateField = "movementState";
inline constexpr std::string_view kAreaField = "area";
inline constexpr std::string_view kLocationField = "location";
inline constexpr std::string_view kDetectedField = "detected";
inline constexpr std::string_view kImageNameField = "imageName";
inline constexpr std::string_view kImagePathField = "imagePath";
inline constexpr std::string_view kImageUrlField = "imageUrl";
inline constexpr std::string_view kImageIdField = "imageId";
inline constexpr std::string_view kImageField = "image";
inline constexpr std::string_view kChecksumField = "checksum";
inline constexpr std::string_view kUploadStatusField = "uploadStatus";
inline constexpr std::string_view kMetadataField = "metadata";
inline constexpr std::string_view kBoxXField = "boxX";
inline constexpr std::string_view kBoxYField = "boxY";
inline constexpr std::string_view kBoxWidthField = "boxWidth";
inline constexpr std::string_view kBoxHeightField = "boxHeight";
inline constexpr std::string_view kCenterXField = "centerX";
inline constexpr std::string_view kCenterYField = "centerY";
inline constexpr std::string_view kOffsetXField = "offsetX";
inline constexpr std::string_view kOffsetYField = "offsetY";
inline constexpr std::string_view kPositionStatusField = "positionStatus";
inline constexpr std::string_view kBoxCornersField = "boxCorners";
inline constexpr std::string_view kXField = "x";
inline constexpr std::string_view kYField = "y";
inline constexpr std::string_view kBarcodeField = "barcode";
inline constexpr std::string_view kRecognitionStatusField = "recognitionStatus";
inline constexpr std::string_view kConfidenceField = "confidence";
inline constexpr std::string_view kFailureStageField = "failureStage";
inline constexpr std::string_view kResultField = "result";
inline constexpr std::string_view kProcessingResultField = "processingResult";
inline constexpr std::string_view kProductIdField = "productId";
inline constexpr std::string_view kProductNameField = "productName";
inline constexpr std::string_view kCategoryField = "category";
inline constexpr std::string_view kDestinationField = "destination";
inline constexpr std::string_view kCommandField = "command";
inline constexpr std::string_view kParamsField = "params";
inline constexpr std::string_view kErrorLevelField = "errorLevel";
inline constexpr std::string_view kMessageField = "message";
inline constexpr std::string_view kDistanceField = "distance";
inline constexpr std::string_view kSensorIdField = "sensorId";
inline constexpr std::string_view kMeasurementStatusField = "measurementStatus";
inline constexpr std::string_view kDistanceCmField = "distanceCm";

[[nodiscard]] constexpr bool IsValidErrorLevel(std::string_view value) noexcept {
    return value == "INFO" || value == "WARNING" || value == "ERROR" || value == "CRITICAL";
}

[[nodiscard]] constexpr bool IsValidRecognitionStatus(std::string_view value) noexcept {
    return value == "SUCCESS" || value == "FAILED" || value == "MISSING_DATA";
}

[[nodiscard]] constexpr bool IsValidMeasurementStatus(std::string_view value) noexcept {
    return value == "CLEAR" || value == "DETECTED" || value == "FAULT";
}

enum class CodecError : std::uint8_t {
    kNone,
    kMalformedJson,
    kSerializationFailed,
    kRootNotObject,
    kMissingField,
    kInvalidFieldType,
    kInvalidFieldValue,
    kUnsupportedProtocolVersion,
    kUnknownMessageType,
    kUnknownCommand,
    kUnknownCommandResult,
    kUnknownConnectionState,
    kUnexpectedPayloadType,
    kInvalidEnvelope,
    kInvalidPayload,
};

struct CodecStatus {
    CodecError error{ CodecError::kNone };
    std::string field;
    std::string message;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error == CodecError::kNone;
    }
};

struct EncodeResult {
    std::string payload;
    CodecStatus status;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return status.IsSuccess();
    }
};

template <typename T>
struct DecodeResult {
    T value{};
    CodecStatus status;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return status.IsSuccess();
    }
};

[[nodiscard]] constexpr bool IsValidUuid(std::string_view value) noexcept {
    return ::logistics::contracts::IsValidUuid(value);
}

struct DeviceRegisterPayload {
    std::string device_type;
    std::string node_name;
    ConnectionState status{ ConnectionState::kUnknown };
    std::string ip_address;
    bool uart_connected{ false };

    [[nodiscard]] bool IsValid() const noexcept {
        return !device_type.empty() && !node_name.empty() && status != ConnectionState::kUnknown && !ip_address.empty();
    }
};

struct HeartbeatPayload {
    ConnectionState status{ ConnectionState::kUnknown };
    std::string current_state;
    std::uint64_t uptime{ 0 };
    std::optional<std::string> job_id;
    std::optional<std::string> error_code;

    [[nodiscard]] bool IsValid() const noexcept {
        return status != ConnectionState::kUnknown && !current_state.empty() &&
               (!job_id.has_value() || IsValidTopicLevel(*job_id)) && (!error_code.has_value() || !error_code->empty());
    }
};

struct BoxDetectedPayload {
    bool detected{ false };
    std::string image_name;

    [[nodiscard]] bool IsValid() const noexcept {
        return detected && !image_name.empty();
    }
};

struct SensorStatusPayload {
    std::int32_t sensor_id{ 0 };
    std::string measurement_status;
    std::int32_t distance_cm{ 0 };

    [[nodiscard]] bool IsValid() const noexcept {
        return sensor_id > 0 && sensor_id <= std::numeric_limits<std::uint8_t>::max() &&
               IsValidMeasurementStatus(measurement_status) && distance_cm >= 0 &&
               distance_cm <= std::numeric_limits<std::uint16_t>::max();
    }
};

struct WorkCreatedPayload {
    std::string work_id;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidUuid(work_id);
    }
};

struct WorkCompletedPayload {
    std::string work_id;
    std::string result;
    std::optional<std::string> message;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidUuid(work_id) && !result.empty() && (!message.has_value() || !message->empty());
    }
};

struct PixelPoint {
    double x{};
    double y{};

    [[nodiscard]] bool IsValid() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0;
    }
};

struct PositionDetectedPayload {
    std::string work_id;
    std::int32_t box_x{ 0 };
    std::int32_t box_y{ 0 };
    std::int32_t box_width{ 0 };
    std::int32_t box_height{ 0 };
    std::int32_t center_x{ 0 };
    std::int32_t center_y{ 0 };
    std::int32_t offset_x{ 0 };
    std::int32_t offset_y{ 0 };
    std::string position_status;
    std::optional<std::array<PixelPoint, 4>> box_corners;

    [[nodiscard]] bool IsValid() const noexcept {
        const bool valid_corners =
            !box_corners.has_value() || std::all_of(box_corners->begin(), box_corners->end(),
                                                    [](const PixelPoint& point) { return point.IsValid(); });
        return IsValidUuid(work_id) && box_x >= 0 && box_y >= 0 && box_width > 0 && box_height > 0 && center_x >= 0 &&
               center_y >= 0 && !position_status.empty() && valid_corners;
    }
};

struct BarcodeDetectedPayload {
    std::string work_id;
    std::string recognition_status;
    std::string barcode;
    std::optional<double> confidence;
    std::optional<std::string> message;
    std::optional<std::string> error_code;
    std::optional<std::string> failure_stage;

    [[nodiscard]] bool IsValid() const noexcept {
        const bool has_failure_metadata = error_code.has_value() || failure_stage.has_value();
        const bool failure_metadata_valid =
            !has_failure_metadata || (recognition_status != "SUCCESS" && error_code.has_value() &&
                                      failure_stage.has_value() && !error_code->empty() && !failure_stage->empty());
        return IsValidUuid(work_id) && IsValidRecognitionStatus(recognition_status) &&
               (recognition_status != "SUCCESS" || !barcode.empty()) &&
               (!confidence.has_value() || (*confidence >= 0.0 && *confidence <= 1.0)) &&
               (!message.has_value() || !message->empty()) && failure_metadata_valid;
    }
};

struct ProductImagePayload {
    std::string work_id;
    std::string image_id;
    std::string image_url;
    std::string image_path;
    std::string checksum;
    std::string upload_status;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidUuid(work_id) && IsValidUuid(image_id) && image_path.starts_with("/uploads/images/") &&
               http::IsSha256(checksum) && upload_status == "UPLOADED";
    }
};

struct ProductInfoPayload {
    std::string work_id;
    std::string recognition_status;
    std::string barcode;
    std::string product_id;
    std::string product_name;
    std::string destination;
    Json image{ nullptr };
    std::optional<double> confidence;
    std::optional<std::string> message;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidUuid(work_id) && IsValidRecognitionStatus(recognition_status) &&
               (image.is_null() || image.is_object()) &&
               (!confidence.has_value() || (*confidence >= 0.0 && *confidence <= 1.0)) &&
               (!message.has_value() || !message->empty());
    }
};

struct DestinationSetPayload {
    std::string request_id;
    std::string work_id;
    ControlCommand command{ ControlCommand::kDestinationSet };
    std::string target_device_id;
    std::string destination;

    [[nodiscard]] bool IsValid() const noexcept {
        const bool is_event = request_id.empty() && target_device_id.empty();
        const bool is_command = CommandRequestView{
            .request_id = request_id, .command = command, .target_device = target_device_id, .component_id = {}
        }.IsValid();
        return command == ControlCommand::kDestinationSet && (is_event || is_command) && IsValidUuid(work_id) &&
               IsValidTopicLevel(destination);
    }
};

struct LineTracerPositionPayload {
    std::string area;
    std::string location;

    [[nodiscard]] bool IsValid() const noexcept {
        const bool valid_area = area == "DEPARTURE" || area == "DESTINATION";
        const bool valid_location = location == "A" || location == "B" || location == "C";
        return valid_area && valid_location;
    }
};

struct DeviceStatusPayload {
    ConnectionState status{ ConnectionState::kUnknown };
    std::string current_state;
    std::optional<std::string> job_id;
    std::optional<std::string> error_code;
    std::optional<LineTracerPositionPayload> departure_position;
    std::optional<LineTracerPositionPayload> target_position;
    std::optional<LineTracerPositionPayload> confirmed_position;
    std::optional<std::string> movement_state;
    bool position_reset{};

    [[nodiscard]] bool IsValid() const noexcept {
        const bool has_any_position = departure_position.has_value() || target_position.has_value() ||
                                      confirmed_position.has_value() || movement_state.has_value();
        const bool has_complete_position = departure_position.has_value() && target_position.has_value() &&
                                           confirmed_position.has_value() && movement_state.has_value();
        const bool valid_movement_state = !movement_state.has_value() || *movement_state == "IDLE" ||
                                          *movement_state == "MOVING" || *movement_state == "ARRIVED";
        return status != ConnectionState::kUnknown && !current_state.empty() &&
               (!job_id.has_value() || IsValidTopicLevel(*job_id)) &&
               (!error_code.has_value() || !error_code->empty()) && (!has_any_position || has_complete_position) &&
               (!departure_position.has_value() || departure_position->IsValid()) &&
               (!target_position.has_value() || target_position->IsValid()) &&
               (!confirmed_position.has_value() || confirmed_position->IsValid()) && valid_movement_state &&
               (!position_reset || !has_any_position);
    }
};

struct ControlCommandPayload {
    std::string request_id;
    ControlCommand command{ ControlCommand::kUnknown };
    std::string target_device_id;
    std::string component_id;
    Json params{ Json::object() };

    [[nodiscard]] bool IsValid() const noexcept {
        const CommandRequestView request{
            .request_id = request_id,
            .command = command,
            .target_device = target_device_id,
            .component_id = component_id,
        };

        return request.IsValid() && command != ControlCommand::kDestinationSet &&
               command != ControlCommand::kEmergencyStop && params.is_object();
    }
};

struct ErrorOccurredPayload {
    std::optional<std::string> job_id;
    std::string error_code;
    std::string error_level;
    std::string current_state;
    std::string message;
    std::optional<std::int32_t> distance;

    [[nodiscard]] bool IsValid() const noexcept {
        return (!job_id.has_value() || IsValidTopicLevel(*job_id)) && !error_code.empty() &&
               IsValidErrorLevel(error_level) && !current_state.empty() && !message.empty() &&
               (!distance.has_value() || *distance >= 0);
    }
};

struct EmergencyStopPayload {
    std::string request_id;
    ControlCommand command{ ControlCommand::kEmergencyStop };
    std::string target_device_id;

    [[nodiscard]] bool IsValid() const noexcept {
        const CommandRequestView request{
            .request_id = request_id,
            .command = command,
            .target_device = target_device_id,
            .component_id = {},
        };

        return command == ControlCommand::kEmergencyStop && request.IsValid();
    }
};

struct CommandResponsePayload {
    std::string request_id;
    ControlCommand command{ ControlCommand::kUnknown };
    CommandResult result{ CommandResult::kUnknown };
    std::optional<std::string> error_code;
    std::string message;

    [[nodiscard]] bool IsValid() const noexcept {
        const CommandResponseView response{
            .request_id = request_id,
            .command = command,
            .result = result,
        };

        return response.IsValid() && (!error_code.has_value() || !error_code->empty()) && !message.empty();
    }
};

using MessagePayload =
    std::variant<std::monostate, DeviceRegisterPayload, HeartbeatPayload, BoxDetectedPayload, WorkCreatedPayload,
                 WorkCompletedPayload, PositionDetectedPayload, BarcodeDetectedPayload, ProductImagePayload,
                 ProductInfoPayload, DestinationSetPayload, DeviceStatusPayload, ControlCommandPayload,
                 ErrorOccurredPayload, EmergencyStopPayload, CommandResponsePayload, SensorStatusPayload>;

struct MqttMessage {
    std::string protocol_version{ std::string(kCurrentProtocolVersion) };
    std::string message_id;
    MessageType message_type{ MessageType::kUnknown };
    std::string source_id;
    std::string timestamp;
    MessagePayload data;

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] constexpr ConnectionState ConnectionStateFromString(std::string_view value) noexcept {
    constexpr std::array values = {
        ConnectionState::kOnline,        ConnectionState::kDelayed,   ConnectionState::kOffline,
        ConnectionState::kReconnecting,  ConnectionState::kRtspError, ConnectionState::kMqttError,
        ConnectionState::kMqttAuthError, ConnectionState::kTlsError,  ConnectionState::kUartError,
    };

    for (const auto state : values) {
        if (ToString(state) == value) {
            return state;
        }
    }

    return ConnectionState::kUnknown;
}

[[nodiscard]] constexpr bool IsAsciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr int ParseTwoDigits(std::string_view value, std::size_t offset) noexcept {
    if (offset + 1U >= value.size() || !IsAsciiDigit(value[offset]) || !IsAsciiDigit(value[offset + 1U])) {
        return -1;
    }

    return (value[offset] - '0') * 10 + (value[offset + 1U] - '0');
}

[[nodiscard]] constexpr int ParseFourDigits(std::string_view value, std::size_t offset) noexcept {
    if (offset + 3U >= value.size()) {
        return -1;
    }

    int result = 0;

    for (std::size_t index = 0U; index < 4U; ++index) {
        if (!IsAsciiDigit(value[offset + index])) {
            return -1;
        }

        result = result * 10 + (value[offset + index] - '0');
    }

    return result;
}

[[nodiscard]] constexpr bool IsLeapYear(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr int DaysInMonth(int year, int month) noexcept {
    constexpr std::array<int, 12U> days = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    if (month < 1 || month > 12) {
        return 0;
    }

    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }

    return days[static_cast<std::size_t>(month - 1)];
}

/*
 * Accepted forms:
 *   YYYY-MM-DDTHH:MM:SSZ
 *   YYYY-MM-DDTHH:MM:SS+09:00
 *   YYYY-MM-DDTHH:MM:SS-05:00
 * Fractional seconds are also accepted before Z or the explicit offset.
 */
[[nodiscard]] constexpr bool IsValidIso8601Timestamp(std::string_view value) noexcept {
    if (value.size() < 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':') {
        return false;
    }

    const int year = ParseFourDigits(value, 0U);
    const int month = ParseTwoDigits(value, 5U);
    const int day = ParseTwoDigits(value, 8U);
    const int hour = ParseTwoDigits(value, 11U);
    const int minute = ParseTwoDigits(value, 14U);
    const int second = ParseTwoDigits(value, 17U);

    if (year < 0 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    std::size_t timezone_offset = 19U;

    if (value[timezone_offset] == '.') {
        ++timezone_offset;
        const std::size_t fraction_begin = timezone_offset;

        while (timezone_offset < value.size() && IsAsciiDigit(value[timezone_offset])) {
            ++timezone_offset;
        }

        if (timezone_offset == fraction_begin) {
            return false;
        }
    }

    if (timezone_offset >= value.size()) {
        return false;
    }

    if (value[timezone_offset] == 'Z') {
        return timezone_offset + 1U == value.size();
    }

    if ((value[timezone_offset] != '+' && value[timezone_offset] != '-') || timezone_offset + 6U != value.size() ||
        value[timezone_offset + 3U] != ':') {
        return false;
    }

    const int offset_hour = ParseTwoDigits(value, timezone_offset + 1U);
    const int offset_minute = ParseTwoDigits(value, timezone_offset + 4U);

    return offset_hour >= 0 && offset_hour <= 23 && offset_minute >= 0 && offset_minute <= 59;
}

[[nodiscard]] constexpr std::string_view ToString(CodecError error) noexcept {
    switch (error) {
        case CodecError::kNone:
            return "NONE";
        case CodecError::kMalformedJson:
            return "MALFORMED_JSON";
        case CodecError::kSerializationFailed:
            return "SERIALIZATION_FAILED";
        case CodecError::kRootNotObject:
            return "ROOT_NOT_OBJECT";
        case CodecError::kMissingField:
            return "MISSING_FIELD";
        case CodecError::kInvalidFieldType:
            return "INVALID_FIELD_TYPE";
        case CodecError::kInvalidFieldValue:
            return "INVALID_FIELD_VALUE";
        case CodecError::kUnsupportedProtocolVersion:
            return "UNSUPPORTED_PROTOCOL_VERSION";
        case CodecError::kUnknownMessageType:
            return "UNKNOWN_MESSAGE_TYPE";
        case CodecError::kUnknownCommand:
            return "UNKNOWN_COMMAND";
        case CodecError::kUnknownCommandResult:
            return "UNKNOWN_COMMAND_RESULT";
        case CodecError::kUnknownConnectionState:
            return "UNKNOWN_CONNECTION_STATE";
        case CodecError::kUnexpectedPayloadType:
            return "UNEXPECTED_PAYLOAD_TYPE";
        case CodecError::kInvalidEnvelope:
            return "INVALID_ENVELOPE";
        case CodecError::kInvalidPayload:
            return "INVALID_PAYLOAD";
    }

    return "UNKNOWN_CODEC_ERROR";
}

[[nodiscard]] inline bool PayloadMatchesMessageType(MessageType message_type, const MessagePayload& payload) noexcept {
    switch (message_type) {
        case MessageType::kDeviceRegister:
            return std::holds_alternative<DeviceRegisterPayload>(payload);
        case MessageType::kHeartbeat:
            return std::holds_alternative<HeartbeatPayload>(payload);
        case MessageType::kBoxDetected:
            return std::holds_alternative<BoxDetectedPayload>(payload);
        case MessageType::kWorkCreated:
            return std::holds_alternative<WorkCreatedPayload>(payload);
        case MessageType::kWorkCompleted:
            return std::holds_alternative<WorkCompletedPayload>(payload);
        case MessageType::kPositionDetected:
            return std::holds_alternative<PositionDetectedPayload>(payload);
        case MessageType::kBarcodeDetected:
            return std::holds_alternative<BarcodeDetectedPayload>(payload);
        case MessageType::kProductImage:
            return std::holds_alternative<ProductImagePayload>(payload);
        case MessageType::kProductInfo:
            return std::holds_alternative<ProductInfoPayload>(payload);
        case MessageType::kDestinationSet:
            return std::holds_alternative<DestinationSetPayload>(payload);
        case MessageType::kDeviceStatus:
            return std::holds_alternative<DeviceStatusPayload>(payload);
        case MessageType::kControlCommand:
            return std::holds_alternative<ControlCommandPayload>(payload);
        case MessageType::kErrorOccurred:
            return std::holds_alternative<ErrorOccurredPayload>(payload);
        case MessageType::kEmergencyStop:
            return std::holds_alternative<EmergencyStopPayload>(payload);
        case MessageType::kCommandResponse:
            return std::holds_alternative<CommandResponsePayload>(payload);
        case MessageType::kSensorStatus:
            return std::holds_alternative<SensorStatusPayload>(payload);
        case MessageType::kUnknown:
            return false;
    }

    return false;
}

[[nodiscard]] inline bool IsPayloadValid(const MessagePayload& payload) noexcept {
    return std::visit(
        [](const auto& value) -> bool {
            using PayloadType = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<PayloadType, std::monostate>) {
                return false;
            } else {
                return value.IsValid();
            }
        },
        payload);
}

inline bool MqttMessage::IsValid() const noexcept {
    const EnvelopeView envelope{
        .protocol_version = protocol_version,
        .message_id = message_id,
        .message_type = message_type,
        .source_id = source_id,
        .timestamp = timestamp,
        .data_json = "{}",
    };

    return envelope.IsValid() && IsValidIso8601Timestamp(timestamp) && PayloadMatchesMessageType(message_type, data) &&
           IsPayloadValid(data);
}

namespace codec_detail {

[[nodiscard]] inline CodecStatus MakeError(CodecError error, std::string_view field, std::string_view message) {
    return {
        .error = error,
        .field = std::string(field),
        .message = std::string(message),
    };
}

[[nodiscard]] inline bool ReadRequiredString(const Json& object, std::string_view field, std::string& output,
                                             CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end()) {
        status = MakeError(CodecError::kMissingField, field, "Required JSON field is missing");
        return false;
    }

    if (!iterator->is_string()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a string");
        return false;
    }

    output = iterator->get<std::string>();
    if (output.empty()) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "Required JSON string must not be empty");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool ReadRequiredUuid(const Json& object, std::string_view field, std::string& output,
                                           CodecStatus& status) {
    if (!ReadRequiredString(object, field, output, status)) {
        return false;
    }
    if (!IsValidUuid(output)) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "JSON field must contain a UUID");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool ReadOptionalString(const Json& object, std::string_view field,
                                             std::optional<std::string>& output, CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end() || iterator->is_null()) {
        output.reset();
        return true;
    }

    if (!iterator->is_string()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a string or null");
        return false;
    }

    output = iterator->get<std::string>();
    return true;
}

[[nodiscard]] inline bool ReadOptionalPlainString(const Json& object, std::string_view field, std::string& output,
                                                  CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end() || iterator->is_null()) {
        output.clear();
        return true;
    }

    if (!iterator->is_string()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a string or null");
        return false;
    }

    output = iterator->get<std::string>();
    return true;
}

[[nodiscard]] inline bool ReadRequiredBoolean(const Json& object, std::string_view field, bool& output,
                                              CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end()) {
        status = MakeError(CodecError::kMissingField, field, "Required JSON field is missing");
        return false;
    }

    if (!iterator->is_boolean()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a boolean");
        return false;
    }

    output = iterator->get<bool>();
    return true;
}

[[nodiscard]] inline bool ReadRequiredSignedInteger(const Json& object, std::string_view field, std::int32_t& output,
                                                    CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end()) {
        status = MakeError(CodecError::kMissingField, field, "Required JSON field is missing");
        return false;
    }

    if (!iterator->is_number_integer() && !iterator->is_number_unsigned()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be an integer");
        return false;
    }

    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value is out of range");
            return false;
        }
        output = static_cast<std::int32_t>(value);
        return true;
    }

    const auto value = iterator->get<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value is out of range");
        return false;
    }

    output = static_cast<std::int32_t>(value);
    return true;
}

[[nodiscard]] inline bool ReadOptionalSignedInteger(const Json& object, std::string_view field,
                                                    std::optional<std::int32_t>& output, CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end() || iterator->is_null()) {
        output.reset();
        return true;
    }

    if (!iterator->is_number_integer() && !iterator->is_number_unsigned()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be an integer or null");
        return false;
    }

    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value is out of range");
            return false;
        }
        output = static_cast<std::int32_t>(value);
        return true;
    }

    const auto value = iterator->get<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value is out of range");
        return false;
    }

    output = static_cast<std::int32_t>(value);
    return true;
}

[[nodiscard]] inline bool ReadOptionalStringValue(const Json& object, std::string_view field, std::string& output,
                                                  CodecStatus& status) {
    std::optional<std::string> value;
    if (!ReadOptionalString(object, field, value, status)) {
        return false;
    }
    output = value.value_or(std::string{});
    return true;
}

[[nodiscard]] inline bool ReadOptionalDouble(const Json& object, std::string_view field, std::optional<double>& output,
                                             CodecStatus& status) {
    const auto iterator = object.find(std::string(field));
    if (iterator == object.end() || iterator->is_null()) {
        output.reset();
        return true;
    }
    if (!iterator->is_number()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a number or null");
        return false;
    }
    output = iterator->get<double>();
    if (*output < 0.0 || *output > 1.0) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "Number must be between 0 and 1");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool ReadRequiredUnsignedInteger(const Json& object, std::string_view field, std::uint64_t& output,
                                                      CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end()) {
        status = MakeError(CodecError::kMissingField, field, "Required JSON field is missing");
        return false;
    }

    if (!iterator->is_number_unsigned() && !iterator->is_number_integer()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be a non-negative integer");
        return false;
    }

    try {
        if (iterator->is_number_unsigned()) {
            output = iterator->get<std::uint64_t>();
            return true;
        }

        const auto signed_value = iterator->get<std::int64_t>();
        if (signed_value < 0) {
            status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value must not be negative");
            return false;
        }

        output = static_cast<std::uint64_t>(signed_value);
        return true;
    } catch (const nlohmann::json::exception&) {
        status = MakeError(CodecError::kInvalidFieldValue, field, "Integer value is out of range");
        return false;
    }
}

[[nodiscard]] inline const Json* ReadRequiredObject(const Json& object, std::string_view field, CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end()) {
        status = MakeError(CodecError::kMissingField, field, "Required JSON object is missing");
        return nullptr;
    }

    if (!iterator->is_object()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be an object");
        return nullptr;
    }

    return &(*iterator);
}

[[nodiscard]] inline bool ReadRequiredObjectValue(const Json& object, std::string_view field, Json& output,
                                                  CodecStatus& status) {
    const Json* value = ReadRequiredObject(object, field, status);

    if (value == nullptr) {
        return false;
    }

    output = *value;
    return true;
}

[[nodiscard]] inline bool ReadOptionalObjectValue(const Json& object, std::string_view field, Json& output,
                                                  CodecStatus& status) {
    const auto iterator = object.find(std::string(field));

    if (iterator == object.end() || iterator->is_null()) {
        output = Json::object();
        return true;
    }

    if (!iterator->is_object()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be an object or null");
        return false;
    }

    output = *iterator;
    return true;
}

inline void WriteOptionalString(Json& object, std::string_view field, const std::optional<std::string>& value) {
    object[std::string(field)] = value.has_value() ? Json(*value) : Json(nullptr);
}

inline void WriteOptionalDouble(Json& object, std::string_view field, const std::optional<double>& value) {
    if (value.has_value()) {
        object[std::string(field)] = *value;
    }
}

[[nodiscard]] inline bool ReadConnectionState(const Json& object, std::string_view field, ConnectionState& output,
                                              CodecStatus& status) {
    std::string value;

    if (!ReadRequiredString(object, field, value, status)) {
        return false;
    }

    output = ConnectionStateFromString(value);

    if (output == ConnectionState::kUnknown) {
        status = MakeError(CodecError::kUnknownConnectionState, field, "Unknown connection state");
        return false;
    }

    return true;
}

[[nodiscard]] inline bool ReadControlCommand(const Json& object, std::string_view field, ControlCommand& output,
                                             CodecStatus& status) {
    std::string value;

    if (!ReadRequiredString(object, field, value, status)) {
        return false;
    }

    output = ControlCommandFromString(value);

    if (output == ControlCommand::kUnknown) {
        status = MakeError(CodecError::kUnknownCommand, field, "Unknown control command");
        return false;
    }

    return true;
}

[[nodiscard]] inline bool ReadCommandResult(const Json& object, std::string_view field, CommandResult& output,
                                            CodecStatus& status) {
    std::string value;

    if (!ReadRequiredString(object, field, value, status)) {
        return false;
    }

    output = CommandResultFromString(value);

    if (output == CommandResult::kUnknown) {
        status = MakeError(CodecError::kUnknownCommandResult, field, "Unknown command result");
        return false;
    }

    return true;
}

[[nodiscard]] inline bool ReadEnvelope(const Json& root, MqttMessage& message, CodecStatus& status) {
    std::string message_type;

    if (!ReadRequiredString(root, kProtocolVersionField, message.protocol_version, status)) {
        return false;
    }

    if (message.protocol_version != kCurrentProtocolVersion) {
        status = MakeError(CodecError::kUnsupportedProtocolVersion, kProtocolVersionField,
                           "Unsupported MQTT protocol version");
        return false;
    }

    if (!ReadRequiredString(root, kMessageIdField, message.message_id, status) ||
        !ReadRequiredString(root, kMessageTypeField, message_type, status) ||
        !ReadRequiredString(root, kSourceIdField, message.source_id, status) ||
        !ReadRequiredString(root, kTimestampField, message.timestamp, status)) {
        return false;
    }

    if (!IsValidIso8601Timestamp(message.timestamp)) {
        status = MakeError(CodecError::kInvalidFieldValue, kTimestampField,
                           "Timestamp must be ISO 8601 with UTC Z or an explicit offset");
        return false;
    }

    message.message_type = MessageTypeFromString(message_type);

    if (message.message_type == MessageType::kUnknown) {
        status = MakeError(CodecError::kUnknownMessageType, kMessageTypeField, "Unknown MQTT message type");
        return false;
    }

    const EnvelopeView envelope{
        .protocol_version = message.protocol_version,
        .message_id = message.message_id,
        .message_type = message.message_type,
        .source_id = message.source_id,
        .timestamp = message.timestamp,
        .data_json = "{}",
    };

    if (!envelope.IsValid()) {
        status = MakeError(CodecError::kInvalidEnvelope, "", "MQTT envelope is invalid");
        return false;
    }

    return true;
}

[[nodiscard]] inline Json SerializePayload(const std::monostate&) {
    return Json::object();
}

[[nodiscard]] inline Json SerializePayload(const DeviceRegisterPayload& payload) {
    return {
        { std::string(kDeviceTypeField), payload.device_type },
        { std::string(kNodeNameField), payload.node_name },
        { std::string(kStatusField), std::string(ToString(payload.status)) },
        { std::string(kIpAddressField), payload.ip_address },
        { std::string(kUartConnectedField), payload.uart_connected },
    };
}

[[nodiscard]] inline Json SerializePayload(const HeartbeatPayload& payload) {
    Json data = {
        { std::string(kStatusField), std::string(ToString(payload.status)) },
        { std::string(kCurrentStateField), payload.current_state },
        { std::string(kUptimeField), payload.uptime },
    };

    WriteOptionalString(data, kJobIdField, payload.job_id);
    WriteOptionalString(data, kErrorCodeField, payload.error_code);
    return data;
}

[[nodiscard]] inline Json SerializePayload(const BoxDetectedPayload& payload) {
    return {
        { std::string(kDetectedField), payload.detected },
        { std::string(kImageNameField), payload.image_name },
    };
}

[[nodiscard]] inline Json SerializePayload(const SensorStatusPayload& payload) {
    return {
        { std::string(kSensorIdField), payload.sensor_id },
        { std::string(kMeasurementStatusField), payload.measurement_status },
        { std::string(kDistanceCmField), payload.distance_cm },
    };
}

[[nodiscard]] inline Json SerializePayload(const WorkCreatedPayload& payload) {
    return {
        { std::string(kWorkIdField), payload.work_id },
    };
}

[[nodiscard]] inline Json SerializePayload(const WorkCompletedPayload& payload) {
    Json data = {
        { std::string(kWorkIdField), payload.work_id },
        { std::string(kResultField), payload.result },
    };
    WriteOptionalString(data, kMessageField, payload.message);
    return data;
}

[[nodiscard]] inline Json SerializePayload(const PositionDetectedPayload& payload) {
    Json data = {
        { std::string(kWorkIdField), payload.work_id },
        { std::string(kBoxXField), payload.box_x },
        { std::string(kBoxYField), payload.box_y },
        { std::string(kBoxWidthField), payload.box_width },
        { std::string(kBoxHeightField), payload.box_height },
        { std::string(kCenterXField), payload.center_x },
        { std::string(kCenterYField), payload.center_y },
        { std::string(kOffsetXField), payload.offset_x },
        { std::string(kOffsetYField), payload.offset_y },
        { std::string(kPositionStatusField), payload.position_status },
    };
    if (payload.box_corners.has_value()) {
        data[std::string(kBoxCornersField)] = Json::array();
        for (const auto& point : *payload.box_corners) {
            data[std::string(kBoxCornersField)].push_back(
                Json{ { std::string(kXField), point.x }, { std::string(kYField), point.y } });
        }
    }
    return data;
}

[[nodiscard]] inline Json SerializePayload(const BarcodeDetectedPayload& payload) {
    Json data = {
        { std::string(kWorkIdField), payload.work_id },
        { std::string(kRecognitionStatusField), payload.recognition_status },
    };
    if (!payload.barcode.empty())
        data[std::string(kBarcodeField)] = payload.barcode;
    WriteOptionalDouble(data, kConfidenceField, payload.confidence);
    WriteOptionalString(data, kMessageField, payload.message);
    WriteOptionalString(data, kErrorCodeField, payload.error_code);
    WriteOptionalString(data, kFailureStageField, payload.failure_stage);
    return data;
}

[[nodiscard]] inline Json SerializePayload(const ProductImagePayload& payload) {
    return {
        { std::string(kWorkIdField), payload.work_id },     { std::string(kImageIdField), payload.image_id },
        { std::string(kImageUrlField), payload.image_url }, { std::string(kImagePathField), payload.image_path },
        { std::string(kChecksumField), payload.checksum },  { std::string(kUploadStatusField), payload.upload_status },
    };
}

[[nodiscard]] inline Json SerializePayload(const ProductInfoPayload& payload) {
    Json data = {
        { std::string(kWorkIdField), payload.work_id },
        { std::string(kRecognitionStatusField), payload.recognition_status },
    };
    if (!payload.barcode.empty())
        data[std::string(kBarcodeField)] = payload.barcode;
    if (!payload.product_id.empty())
        data[std::string(kProductIdField)] = payload.product_id;
    if (!payload.product_name.empty())
        data[std::string(kProductNameField)] = payload.product_name;
    if (!payload.destination.empty())
        data[std::string(kDestinationField)] = payload.destination;
    if (!payload.image.is_null())
        data[std::string(kImageField)] = payload.image;
    WriteOptionalDouble(data, kConfidenceField, payload.confidence);
    WriteOptionalString(data, kMessageField, payload.message);
    return data;
}

[[nodiscard]] inline Json SerializePayload(const DestinationSetPayload& payload) {
    Json data = {
        { std::string(kWorkIdField), payload.work_id },
        { std::string(kDestinationField), payload.destination },
    };
    if (!payload.request_id.empty()) {
        data[std::string(kRequestIdField)] = payload.request_id;
        data[std::string(kCommandField)] = std::string(ToString(payload.command));
        data[std::string(kTargetDeviceIdField)] = payload.target_device_id;
    }
    return data;
}

[[nodiscard]] inline Json SerializeLineTracerPosition(const LineTracerPositionPayload& position) {
    return {
        { std::string(kAreaField), position.area },
        { std::string(kLocationField), position.location },
    };
}

inline void WriteOptionalLineTracerPosition(Json& data, std::string_view field,
                                            const std::optional<LineTracerPositionPayload>& position) {
    if (position.has_value()) {
        data[std::string(field)] = SerializeLineTracerPosition(*position);
    }
}

[[nodiscard]] inline Json SerializePayload(const DeviceStatusPayload& payload) {
    Json data = {
        { std::string(kStatusField), std::string(ToString(payload.status)) },
        { std::string(kCurrentStateField), payload.current_state },
    };

    WriteOptionalString(data, kJobIdField, payload.job_id);
    WriteOptionalString(data, kErrorCodeField, payload.error_code);
    if (payload.position_reset) {
        data[std::string(kDeparturePositionField)] = nullptr;
        data[std::string(kTargetPositionField)] = nullptr;
        data[std::string(kConfirmedPositionField)] = nullptr;
        data[std::string(kMovementStateField)] = nullptr;
    } else {
        WriteOptionalLineTracerPosition(data, kDeparturePositionField, payload.departure_position);
        WriteOptionalLineTracerPosition(data, kTargetPositionField, payload.target_position);
        WriteOptionalLineTracerPosition(data, kConfirmedPositionField, payload.confirmed_position);
        if (payload.movement_state.has_value()) {
            data[std::string(kMovementStateField)] = *payload.movement_state;
        }
    }
    return data;
}

[[nodiscard]] inline Json SerializePayload(const ControlCommandPayload& payload) {
    Json data = {
        { std::string(kRequestIdField), payload.request_id },
        { std::string(kCommandField), std::string(ToString(payload.command)) },
        { std::string(kTargetDeviceIdField), payload.target_device_id },
        { std::string(kParamsField), payload.params },
    };

    if (!payload.component_id.empty()) {
        data[std::string(kComponentIdField)] = payload.component_id;
    }

    return data;
}

[[nodiscard]] inline Json SerializePayload(const ErrorOccurredPayload& payload) {
    Json data = {
        { std::string(kErrorCodeField), payload.error_code },
        { std::string(kErrorLevelField), payload.error_level },
        { std::string(kCurrentStateField), payload.current_state },
        { std::string(kMessageField), payload.message },
    };

    WriteOptionalString(data, kJobIdField, payload.job_id);
    data[std::string(kDistanceField)] = payload.distance.has_value() ? Json(*payload.distance) : Json(nullptr);
    return data;
}

[[nodiscard]] inline Json SerializePayload(const EmergencyStopPayload& payload) {
    return {
        { std::string(kRequestIdField), payload.request_id },
        { std::string(kCommandField), std::string(ToString(payload.command)) },
        { std::string(kTargetDeviceIdField), payload.target_device_id },
    };
}

[[nodiscard]] inline Json SerializePayload(const CommandResponsePayload& payload) {
    Json data = {
        { std::string(kRequestIdField), payload.request_id },
        { std::string(kCommandField), std::string(ToString(payload.command)) },
        { std::string(kResultField), std::string(ToString(payload.result)) },
        { std::string(kMessageField), payload.message },
    };

    WriteOptionalString(data, kErrorCodeField, payload.error_code);
    return data;
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, DeviceRegisterPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kDeviceTypeField, payload.device_type, status) &&
           ReadRequiredString(data, kNodeNameField, payload.node_name, status) &&
           ReadConnectionState(data, kStatusField, payload.status, status) &&
           ReadRequiredString(data, kIpAddressField, payload.ip_address, status) &&
           ReadRequiredBoolean(data, kUartConnectedField, payload.uart_connected, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, HeartbeatPayload& payload, CodecStatus& status) {
    return ReadConnectionState(data, kStatusField, payload.status, status) &&
           ReadRequiredString(data, kCurrentStateField, payload.current_state, status) &&
           ReadRequiredUnsignedInteger(data, kUptimeField, payload.uptime, status) &&
           ReadOptionalString(data, kJobIdField, payload.job_id, status) &&
           ReadOptionalString(data, kErrorCodeField, payload.error_code, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, BoxDetectedPayload& payload, CodecStatus& status) {
    return ReadRequiredBoolean(data, kDetectedField, payload.detected, status) &&
           ReadRequiredString(data, kImageNameField, payload.image_name, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, SensorStatusPayload& payload, CodecStatus& status) {
    return ReadRequiredSignedInteger(data, kSensorIdField, payload.sensor_id, status) &&
           ReadRequiredString(data, kMeasurementStatusField, payload.measurement_status, status) &&
           ReadRequiredSignedInteger(data, kDistanceCmField, payload.distance_cm, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, WorkCreatedPayload& payload, CodecStatus& status) {
    return ReadRequiredUuid(data, kWorkIdField, payload.work_id, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, WorkCompletedPayload& payload, CodecStatus& status) {
    return ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) &&
           ReadRequiredString(data, kResultField, payload.result, status) &&
           ReadOptionalString(data, kMessageField, payload.message, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, PositionDetectedPayload& payload, CodecStatus& status) {
    if (!ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) ||
        !ReadRequiredSignedInteger(data, kBoxXField, payload.box_x, status) ||
        !ReadRequiredSignedInteger(data, kBoxYField, payload.box_y, status) ||
        !ReadRequiredSignedInteger(data, kBoxWidthField, payload.box_width, status) ||
        !ReadRequiredSignedInteger(data, kBoxHeightField, payload.box_height, status) ||
        !ReadRequiredSignedInteger(data, kCenterXField, payload.center_x, status) ||
        !ReadRequiredSignedInteger(data, kCenterYField, payload.center_y, status) ||
        !ReadRequiredSignedInteger(data, kOffsetXField, payload.offset_x, status) ||
        !ReadRequiredSignedInteger(data, kOffsetYField, payload.offset_y, status) ||
        !ReadRequiredString(data, kPositionStatusField, payload.position_status, status)) {
        return false;
    }

    const auto corners = data.find(std::string(kBoxCornersField));
    if (corners == data.end()) {
        payload.box_corners.reset();
        return true;
    }
    if (!corners->is_array() || corners->size() != 4U) {
        status =
            MakeError(CodecError::kInvalidFieldValue, kBoxCornersField, "boxCorners must contain exactly four points");
        return false;
    }

    std::array<PixelPoint, 4> decoded{};
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        const Json& point = (*corners)[index];
        if (!point.is_object()) {
            status = MakeError(CodecError::kInvalidFieldType, kBoxCornersField, "boxCorners entries must be objects");
            return false;
        }
        const auto x = point.find(std::string(kXField));
        const auto y = point.find(std::string(kYField));
        if (x == point.end() || y == point.end() || !x->is_number() || !y->is_number()) {
            status = MakeError(CodecError::kInvalidFieldType, kBoxCornersField,
                               "boxCorners entries require numeric x and y");
            return false;
        }
        decoded[index] = { .x = x->get<double>(), .y = y->get<double>() };
    }
    payload.box_corners = decoded;
    return true;
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, BarcodeDetectedPayload& payload, CodecStatus& status) {
    return ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) &&
           ReadRequiredString(data, kRecognitionStatusField, payload.recognition_status, status) &&
           ReadOptionalStringValue(data, kBarcodeField, payload.barcode, status) &&
           ReadOptionalDouble(data, kConfidenceField, payload.confidence, status) &&
           ReadOptionalString(data, kMessageField, payload.message, status) &&
           ReadOptionalString(data, kErrorCodeField, payload.error_code, status) &&
           ReadOptionalString(data, kFailureStageField, payload.failure_stage, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ProductImagePayload& payload, CodecStatus& status) {
    return ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) &&
           ReadRequiredString(data, kImageIdField, payload.image_id, status) &&
           ReadOptionalStringValue(data, kImageUrlField, payload.image_url, status) &&
           ReadOptionalStringValue(data, kImagePathField, payload.image_path, status) &&
           ReadRequiredString(data, kChecksumField, payload.checksum, status) &&
           ReadRequiredString(data, kUploadStatusField, payload.upload_status, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ProductInfoPayload& payload, CodecStatus& status) {
    return ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) &&
           ReadRequiredString(data, kRecognitionStatusField, payload.recognition_status, status) &&
           ReadOptionalStringValue(data, kBarcodeField, payload.barcode, status) &&
           ReadOptionalStringValue(data, kProductIdField, payload.product_id, status) &&
           ReadOptionalStringValue(data, kProductNameField, payload.product_name, status) &&
           ReadOptionalStringValue(data, kDestinationField, payload.destination, status) &&
           ReadOptionalObjectValue(data, kImageField, payload.image, status) &&
           ReadOptionalDouble(data, kConfidenceField, payload.confidence, status) &&
           ReadOptionalString(data, kMessageField, payload.message, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, DestinationSetPayload& payload, CodecStatus& status) {
    if (!ReadRequiredUuid(data, kWorkIdField, payload.work_id, status) ||
        !ReadRequiredString(data, kDestinationField, payload.destination, status) ||
        !ReadOptionalStringValue(data, kRequestIdField, payload.request_id, status) ||
        !ReadOptionalStringValue(data, kTargetDeviceIdField, payload.target_device_id, status)) {
        return false;
    }
    if (payload.request_id.empty() && payload.target_device_id.empty()) {
        payload.command = ControlCommand::kDestinationSet;
        return true;
    }
    return ReadControlCommand(data, kCommandField, payload.command, status);
}

[[nodiscard]] inline bool ReadOptionalLineTracerPosition(const Json& data, std::string_view field,
                                                         std::optional<LineTracerPositionPayload>& output,
                                                         CodecStatus& status) {
    const auto iterator = data.find(std::string(field));
    if (iterator == data.end() || iterator->is_null()) {
        output.reset();
        return true;
    }
    if (!iterator->is_object()) {
        status = MakeError(CodecError::kInvalidFieldType, field, "JSON field must be an object or null");
        return false;
    }

    LineTracerPositionPayload position;
    if (!ReadRequiredString(*iterator, kAreaField, position.area, status) ||
        !ReadRequiredString(*iterator, kLocationField, position.location, status)) {
        return false;
    }
    output = std::move(position);
    return true;
}

[[nodiscard]] inline bool ReadPositionReset(const Json& data, bool& output, CodecStatus& status) {
    constexpr std::array position_fields{ kDeparturePositionField, kTargetPositionField, kConfirmedPositionField };
    std::size_t position_present_count = 0;
    std::size_t null_count = 0;
    for (const std::string_view field : position_fields) {
        const auto iterator = data.find(std::string(field));
        if (iterator == data.end()) {
            continue;
        }
        ++position_present_count;
        if (iterator->is_null()) {
            ++null_count;
        }
    }
    const auto movement_iterator = data.find(std::string(kMovementStateField));
    const bool movement_present = movement_iterator != data.end();
    if (movement_present && movement_iterator->is_null()) {
        ++null_count;
    }

    output = false;
    if (position_present_count == 0U && !movement_present) {
        return true;
    }
    if (position_present_count != position_fields.size() || !movement_present) {
        status = MakeError(CodecError::kMissingField, kDeparturePositionField,
                           "line tracer position fields must be omitted or supplied together");
        return false;
    }
    constexpr std::size_t field_count = position_fields.size() + 1U;
    if (null_count == field_count) {
        output = true;
        return true;
    }
    if (null_count != 0U) {
        status = MakeError(CodecError::kInvalidFieldType, kDeparturePositionField,
                           "line tracer position fields must all contain values or all be null");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, DeviceStatusPayload& payload, CodecStatus& status) {
    return ReadPositionReset(data, payload.position_reset, status) &&
           ReadConnectionState(data, kStatusField, payload.status, status) &&
           ReadRequiredString(data, kCurrentStateField, payload.current_state, status) &&
           ReadOptionalString(data, kJobIdField, payload.job_id, status) &&
           ReadOptionalString(data, kErrorCodeField, payload.error_code, status) &&
           ReadOptionalLineTracerPosition(data, kDeparturePositionField, payload.departure_position, status) &&
           ReadOptionalLineTracerPosition(data, kTargetPositionField, payload.target_position, status) &&
           ReadOptionalLineTracerPosition(data, kConfirmedPositionField, payload.confirmed_position, status) &&
           ReadOptionalString(data, kMovementStateField, payload.movement_state, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ControlCommandPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kRequestIdField, payload.request_id, status) &&
           ReadControlCommand(data, kCommandField, payload.command, status) &&
           ReadRequiredString(data, kTargetDeviceIdField, payload.target_device_id, status) &&
           ReadOptionalPlainString(data, kComponentIdField, payload.component_id, status) &&
           ReadOptionalObjectValue(data, kParamsField, payload.params, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ErrorOccurredPayload& payload, CodecStatus& status) {
    if (!ReadOptionalString(data, kJobIdField, payload.job_id, status) ||
        !ReadRequiredString(data, kErrorCodeField, payload.error_code, status) ||
        !ReadRequiredString(data, kErrorLevelField, payload.error_level, status)) {
        return false;
    }
    if (!IsValidErrorLevel(payload.error_level)) {
        status = MakeError(CodecError::kInvalidFieldValue, kErrorLevelField,
                           "errorLevel must be INFO, WARNING, ERROR, or CRITICAL");
        return false;
    }
    return ReadRequiredString(data, kCurrentStateField, payload.current_state, status) &&
           ReadRequiredString(data, kMessageField, payload.message, status) &&
           ReadOptionalSignedInteger(data, kDistanceField, payload.distance, status);
}

[[nodiscard]] inline std::optional<std::string_view> WorkIdFromPayload(const MessagePayload& payload) noexcept {
    if (const auto* value = std::get_if<WorkCreatedPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<WorkCompletedPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<PositionDetectedPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<BarcodeDetectedPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<ProductImagePayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<ProductInfoPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    if (const auto* value = std::get_if<DestinationSetPayload>(&payload); value != nullptr) {
        return value->work_id;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, EmergencyStopPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kRequestIdField, payload.request_id, status) &&
           ReadControlCommand(data, kCommandField, payload.command, status) &&
           ReadRequiredString(data, kTargetDeviceIdField, payload.target_device_id, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, CommandResponsePayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kRequestIdField, payload.request_id, status) &&
           ReadControlCommand(data, kCommandField, payload.command, status) &&
           ReadCommandResult(data, kResultField, payload.result, status) &&
           ReadOptionalString(data, kErrorCodeField, payload.error_code, status) &&
           ReadRequiredString(data, kMessageField, payload.message, status);
}

template <typename PayloadType>
[[nodiscard]] inline bool DecodeAndAssignPayload(const Json& data, MqttMessage& message, CodecStatus& status) {
    PayloadType payload;

    if (!DeserializePayload(data, payload, status)) {
        return false;
    }

    if (!payload.IsValid()) {
        status = MakeError(CodecError::kInvalidPayload, kDataField, "MQTT data payload is invalid");
        return false;
    }

    message.data = std::move(payload);
    return true;
}

}  // namespace codec_detail

[[nodiscard]] inline EncodeResult SerializeMessage(const MqttMessage& message) {
    if (!message.IsValid()) {
        CodecError error = CodecError::kInvalidPayload;
        std::string_view field = kDataField;
        std::string_view description = "MQTT message payload is invalid";

        const EnvelopeView envelope{
            .protocol_version = message.protocol_version,
            .message_id = message.message_id,
            .message_type = message.message_type,
            .source_id = message.source_id,
            .timestamp = message.timestamp,
            .data_json = "{}",
        };

        if (message.protocol_version != kCurrentProtocolVersion) {
            error = CodecError::kUnsupportedProtocolVersion;
            field = kProtocolVersionField;
            description = "Unsupported MQTT protocol version";
        } else if (!IsValidTopicLevel(message.message_id)) {
            error = CodecError::kInvalidEnvelope;
            field = kMessageIdField;
            description = "messageId must be one non-wildcard MQTT topic level";
        } else if (message.message_type == MessageType::kUnknown) {
            error = CodecError::kUnknownMessageType;
            field = kMessageTypeField;
            description = "Unknown MQTT message type";
        } else if (!IsValidTopicLevel(message.source_id)) {
            error = CodecError::kInvalidEnvelope;
            field = kSourceIdField;
            description = "sourceId must be one non-wildcard MQTT topic level";
        } else if (!IsValidIso8601Timestamp(message.timestamp)) {
            error = CodecError::kInvalidFieldValue;
            field = kTimestampField;
            description = "Timestamp must be ISO 8601 with UTC Z or an explicit offset";
        } else if (!envelope.IsValid()) {
            error = CodecError::kInvalidEnvelope;
            field = "";
            description = "MQTT envelope is invalid";
        } else if (!PayloadMatchesMessageType(message.message_type, message.data)) {
            error = CodecError::kUnexpectedPayloadType;
            description = "Payload type does not match messageType";
        } else if (const auto work_id = codec_detail::WorkIdFromPayload(message.data);
                   work_id.has_value() && !IsValidUuid(*work_id)) {
            error = CodecError::kInvalidFieldValue;
            field = kWorkIdField;
            description = "workId must contain a UUID";
        } else if (const auto* payload = std::get_if<ErrorOccurredPayload>(&message.data);
                   payload != nullptr && !IsValidErrorLevel(payload->error_level)) {
            error = CodecError::kInvalidFieldValue;
            field = kErrorLevelField;
            description = "errorLevel must be INFO, WARNING, ERROR, or CRITICAL";
        }

        return {
            .payload = {},
            .status = codec_detail::MakeError(error, field, description),
        };
    }

    try {
        Json root = {
            { std::string(kProtocolVersionField), message.protocol_version },
            { std::string(kMessageIdField), message.message_id },
            { std::string(kMessageTypeField), std::string(ToString(message.message_type)) },
            { std::string(kSourceIdField), message.source_id },
            { std::string(kTimestampField), message.timestamp },
        };

        root[std::string(kDataField)] =
            std::visit([](const auto& payload) { return codec_detail::SerializePayload(payload); }, message.data);

        return {
            .payload = root.dump(),
            .status = {},
        };
    } catch (const nlohmann::json::exception& exception) {
        return {
            .payload = {},
            .status = codec_detail::MakeError(CodecError::kSerializationFailed, "", exception.what()),
        };
    }
}

[[nodiscard]] inline DecodeResult<MqttMessage> DeserializeMessage(std::string_view payload) {
    DecodeResult<MqttMessage> result;
    const Json root = Json::parse(payload.begin(), payload.end(), nullptr, false);

    if (root.is_discarded()) {
        result.status = codec_detail::MakeError(CodecError::kMalformedJson, "", "MQTT payload is not valid JSON");
        return result;
    }

    if (!root.is_object()) {
        result.status = codec_detail::MakeError(CodecError::kRootNotObject, "", "MQTT JSON root must be an object");
        return result;
    }

    if (!codec_detail::ReadEnvelope(root, result.value, result.status)) {
        return result;
    }

    const Json* data = codec_detail::ReadRequiredObject(root, kDataField, result.status);

    if (data == nullptr) {
        return result;
    }

    bool decoded = false;

    switch (result.value.message_type) {
        case MessageType::kDeviceRegister:
            decoded = codec_detail::DecodeAndAssignPayload<DeviceRegisterPayload>(*data, result.value, result.status);
            break;
        case MessageType::kHeartbeat:
            decoded = codec_detail::DecodeAndAssignPayload<HeartbeatPayload>(*data, result.value, result.status);
            break;
        case MessageType::kBoxDetected:
            decoded = codec_detail::DecodeAndAssignPayload<BoxDetectedPayload>(*data, result.value, result.status);
            break;
        case MessageType::kWorkCreated:
            decoded = codec_detail::DecodeAndAssignPayload<WorkCreatedPayload>(*data, result.value, result.status);
            break;
        case MessageType::kWorkCompleted:
            decoded = codec_detail::DecodeAndAssignPayload<WorkCompletedPayload>(*data, result.value, result.status);
            break;
        case MessageType::kPositionDetected:
            decoded = codec_detail::DecodeAndAssignPayload<PositionDetectedPayload>(*data, result.value, result.status);
            break;
        case MessageType::kBarcodeDetected:
            decoded = codec_detail::DecodeAndAssignPayload<BarcodeDetectedPayload>(*data, result.value, result.status);
            break;
        case MessageType::kProductImage:
            decoded = codec_detail::DecodeAndAssignPayload<ProductImagePayload>(*data, result.value, result.status);
            break;
        case MessageType::kProductInfo:
            decoded = codec_detail::DecodeAndAssignPayload<ProductInfoPayload>(*data, result.value, result.status);
            break;
        case MessageType::kDestinationSet:
            decoded = codec_detail::DecodeAndAssignPayload<DestinationSetPayload>(*data, result.value, result.status);
            break;
        case MessageType::kDeviceStatus:
            decoded = codec_detail::DecodeAndAssignPayload<DeviceStatusPayload>(*data, result.value, result.status);
            break;
        case MessageType::kControlCommand:
            decoded = codec_detail::DecodeAndAssignPayload<ControlCommandPayload>(*data, result.value, result.status);
            break;
        case MessageType::kErrorOccurred:
            decoded = codec_detail::DecodeAndAssignPayload<ErrorOccurredPayload>(*data, result.value, result.status);
            break;
        case MessageType::kEmergencyStop:
            decoded = codec_detail::DecodeAndAssignPayload<EmergencyStopPayload>(*data, result.value, result.status);
            break;
        case MessageType::kCommandResponse:
            decoded = codec_detail::DecodeAndAssignPayload<CommandResponsePayload>(*data, result.value, result.status);
            break;
        case MessageType::kSensorStatus:
            decoded = codec_detail::DecodeAndAssignPayload<SensorStatusPayload>(*data, result.value, result.status);
            break;
        case MessageType::kUnknown:
            result.status = codec_detail::MakeError(CodecError::kUnknownMessageType, kMessageTypeField,
                                                    "Unknown MQTT message type");
            return result;
    }

    if (!decoded) {
        return result;
    }

    if (!result.value.IsValid()) {
        result.status = codec_detail::MakeError(CodecError::kInvalidPayload, kDataField, "MQTT message is invalid");
    }

    return result;
}

template <typename PayloadType>
[[nodiscard]] inline const PayloadType* GetPayload(const MqttMessage& message) noexcept {
    return std::get_if<PayloadType>(&message.data);
}

template <typename PayloadType>
[[nodiscard]] inline PayloadType* GetPayload(MqttMessage& message) noexcept {
    return std::get_if<PayloadType>(&message.data);
}

}  // namespace logistics::contracts::mqtt
