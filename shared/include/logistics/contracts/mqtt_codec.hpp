#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

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
inline constexpr std::string_view kDetectedField = "detected";
inline constexpr std::string_view kImageNameField = "imageName";
inline constexpr std::string_view kImagePathField = "imagePath";
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
inline constexpr std::string_view kBarcodeField = "barcode";
inline constexpr std::string_view kResultField = "result";
inline constexpr std::string_view kProductNameField = "productName";
inline constexpr std::string_view kCategoryField = "category";
inline constexpr std::string_view kDestinationField = "destination";
inline constexpr std::string_view kCommandField = "command";
inline constexpr std::string_view kParamsField = "params";
inline constexpr std::string_view kErrorLevelField = "errorLevel";
inline constexpr std::string_view kMessageField = "message";
inline constexpr std::string_view kDistanceField = "distance";

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
    std::string job_id;
    bool detected{ false };
    std::string image_name;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidTopicLevel(job_id) && !image_name.empty();
    }
};

struct PositionDetectedPayload {
    std::string job_id;
    std::int32_t box_x{ 0 };
    std::int32_t box_y{ 0 };
    std::int32_t box_width{ 0 };
    std::int32_t box_height{ 0 };
    std::int32_t center_x{ 0 };
    std::int32_t center_y{ 0 };
    std::int32_t offset_x{ 0 };
    std::int32_t offset_y{ 0 };
    std::string position_status;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidTopicLevel(job_id) && box_x >= 0 && box_y >= 0 && box_width > 0 && box_height > 0 &&
               center_x >= 0 && center_y >= 0 && !position_status.empty();
    }
};

struct BarcodeDetectedPayload {
    std::string job_id;
    std::string barcode;
    std::int32_t center_x{ 0 };
    std::int32_t center_y{ 0 };
    std::string image_name;
    std::string image_path;
    std::string result;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidTopicLevel(job_id) && !barcode.empty() && center_x >= 0 && center_y >= 0 && !image_name.empty() &&
               !image_path.empty() && !result.empty();
    }
};

struct ProductImagePayload {
    std::string job_id;
    std::string image_name;
    std::string image_path;
    Json metadata{ Json::object() };

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidTopicLevel(job_id) && !image_name.empty() && !image_path.empty() && metadata.is_object();
    }
};

struct ProductInfoPayload {
    std::string job_id;
    std::string barcode;
    std::string product_name;
    std::string category;
    std::string destination;
    std::string image_path;

    [[nodiscard]] bool IsValid() const noexcept {
        return IsValidTopicLevel(job_id) && !barcode.empty() && !product_name.empty() && !category.empty() &&
               !destination.empty() && !image_path.empty();
    }
};

struct DestinationSetPayload {
    std::string request_id;
    std::string job_id;
    ControlCommand command{ ControlCommand::kDestinationSet };
    std::string target_device_id;
    std::string destination;

    [[nodiscard]] bool IsValid() const noexcept {
        const CommandRequestView request{
            .request_id = request_id,
            .command = command,
            .target_device = target_device_id,
            .component_id = {},
        };

        return command == ControlCommand::kDestinationSet && request.IsValid() && IsValidTopicLevel(job_id) &&
               IsValidTopicLevel(destination);
    }
};

struct DeviceStatusPayload {
    ConnectionState status{ ConnectionState::kUnknown };
    std::string current_state;
    std::optional<std::string> job_id;
    std::optional<std::string> error_code;

    [[nodiscard]] bool IsValid() const noexcept {
        return status != ConnectionState::kUnknown && !current_state.empty() &&
               (!job_id.has_value() || IsValidTopicLevel(*job_id)) && (!error_code.has_value() || !error_code->empty());
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
        return (!job_id.has_value() || IsValidTopicLevel(*job_id)) && !error_code.empty() && !error_level.empty() &&
               !current_state.empty() && !message.empty() && (!distance.has_value() || *distance >= 0);
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
    std::variant<std::monostate, DeviceRegisterPayload, HeartbeatPayload, BoxDetectedPayload, PositionDetectedPayload,
                 BarcodeDetectedPayload, ProductImagePayload, ProductInfoPayload, DestinationSetPayload,
                 DeviceStatusPayload, ControlCommandPayload, ErrorOccurredPayload, EmergencyStopPayload,
                 CommandResponsePayload>;

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
        { std::string(kJobIdField), payload.job_id },
        { std::string(kDetectedField), payload.detected },
        { std::string(kImageNameField), payload.image_name },
    };
}

[[nodiscard]] inline Json SerializePayload(const PositionDetectedPayload& payload) {
    return {
        { std::string(kJobIdField), payload.job_id },
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
}

[[nodiscard]] inline Json SerializePayload(const BarcodeDetectedPayload& payload) {
    return {
        { std::string(kJobIdField), payload.job_id },         { std::string(kBarcodeField), payload.barcode },
        { std::string(kCenterXField), payload.center_x },     { std::string(kCenterYField), payload.center_y },
        { std::string(kImageNameField), payload.image_name }, { std::string(kImagePathField), payload.image_path },
        { std::string(kResultField), payload.result },
    };
}

[[nodiscard]] inline Json SerializePayload(const ProductImagePayload& payload) {
    return {
        { std::string(kJobIdField), payload.job_id },
        { std::string(kImageNameField), payload.image_name },
        { std::string(kImagePathField), payload.image_path },
        { std::string(kMetadataField), payload.metadata },
    };
}

[[nodiscard]] inline Json SerializePayload(const ProductInfoPayload& payload) {
    return {
        { std::string(kJobIdField), payload.job_id },
        { std::string(kBarcodeField), payload.barcode },
        { std::string(kProductNameField), payload.product_name },
        { std::string(kCategoryField), payload.category },
        { std::string(kDestinationField), payload.destination },
        { std::string(kImagePathField), payload.image_path },
    };
}

[[nodiscard]] inline Json SerializePayload(const DestinationSetPayload& payload) {
    return {
        { std::string(kRequestIdField), payload.request_id },
        { std::string(kJobIdField), payload.job_id },
        { std::string(kCommandField), std::string(ToString(payload.command)) },
        { std::string(kTargetDeviceIdField), payload.target_device_id },
        { std::string(kDestinationField), payload.destination },
    };
}

[[nodiscard]] inline Json SerializePayload(const DeviceStatusPayload& payload) {
    Json data = {
        { std::string(kStatusField), std::string(ToString(payload.status)) },
        { std::string(kCurrentStateField), payload.current_state },
    };

    WriteOptionalString(data, kJobIdField, payload.job_id);
    WriteOptionalString(data, kErrorCodeField, payload.error_code);
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
    return ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredBoolean(data, kDetectedField, payload.detected, status) &&
           ReadRequiredString(data, kImageNameField, payload.image_name, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, PositionDetectedPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredSignedInteger(data, kBoxXField, payload.box_x, status) &&
           ReadRequiredSignedInteger(data, kBoxYField, payload.box_y, status) &&
           ReadRequiredSignedInteger(data, kBoxWidthField, payload.box_width, status) &&
           ReadRequiredSignedInteger(data, kBoxHeightField, payload.box_height, status) &&
           ReadRequiredSignedInteger(data, kCenterXField, payload.center_x, status) &&
           ReadRequiredSignedInteger(data, kCenterYField, payload.center_y, status) &&
           ReadRequiredSignedInteger(data, kOffsetXField, payload.offset_x, status) &&
           ReadRequiredSignedInteger(data, kOffsetYField, payload.offset_y, status) &&
           ReadRequiredString(data, kPositionStatusField, payload.position_status, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, BarcodeDetectedPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredString(data, kBarcodeField, payload.barcode, status) &&
           ReadRequiredSignedInteger(data, kCenterXField, payload.center_x, status) &&
           ReadRequiredSignedInteger(data, kCenterYField, payload.center_y, status) &&
           ReadRequiredString(data, kImageNameField, payload.image_name, status) &&
           ReadRequiredString(data, kImagePathField, payload.image_path, status) &&
           ReadRequiredString(data, kResultField, payload.result, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ProductImagePayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredString(data, kImageNameField, payload.image_name, status) &&
           ReadRequiredString(data, kImagePathField, payload.image_path, status) &&
           ReadOptionalObjectValue(data, kMetadataField, payload.metadata, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ProductInfoPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredString(data, kBarcodeField, payload.barcode, status) &&
           ReadRequiredString(data, kProductNameField, payload.product_name, status) &&
           ReadRequiredString(data, kCategoryField, payload.category, status) &&
           ReadRequiredString(data, kDestinationField, payload.destination, status) &&
           ReadRequiredString(data, kImagePathField, payload.image_path, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, DestinationSetPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kRequestIdField, payload.request_id, status) &&
           ReadRequiredString(data, kJobIdField, payload.job_id, status) &&
           ReadControlCommand(data, kCommandField, payload.command, status) &&
           ReadRequiredString(data, kTargetDeviceIdField, payload.target_device_id, status) &&
           ReadRequiredString(data, kDestinationField, payload.destination, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, DeviceStatusPayload& payload, CodecStatus& status) {
    return ReadConnectionState(data, kStatusField, payload.status, status) &&
           ReadRequiredString(data, kCurrentStateField, payload.current_state, status) &&
           ReadOptionalString(data, kJobIdField, payload.job_id, status) &&
           ReadOptionalString(data, kErrorCodeField, payload.error_code, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ControlCommandPayload& payload, CodecStatus& status) {
    return ReadRequiredString(data, kRequestIdField, payload.request_id, status) &&
           ReadControlCommand(data, kCommandField, payload.command, status) &&
           ReadRequiredString(data, kTargetDeviceIdField, payload.target_device_id, status) &&
           ReadOptionalPlainString(data, kComponentIdField, payload.component_id, status) &&
           ReadOptionalObjectValue(data, kParamsField, payload.params, status);
}

[[nodiscard]] inline bool DeserializePayload(const Json& data, ErrorOccurredPayload& payload, CodecStatus& status) {
    return ReadOptionalString(data, kJobIdField, payload.job_id, status) &&
           ReadRequiredString(data, kErrorCodeField, payload.error_code, status) &&
           ReadRequiredString(data, kErrorLevelField, payload.error_level, status) &&
           ReadRequiredString(data, kCurrentStateField, payload.current_state, status) &&
           ReadRequiredString(data, kMessageField, payload.message, status) &&
           ReadOptionalSignedInteger(data, kDistanceField, payload.distance, status);
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

        if (!envelope.IsValid()) {
            error = CodecError::kInvalidEnvelope;
            field = "";
            description = "MQTT envelope is invalid";
        } else if (!PayloadMatchesMessageType(message.message_type, message.data)) {
            error = CodecError::kUnexpectedPayloadType;
            description = "Payload type does not match messageType";
        }

        return {
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
