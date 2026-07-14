#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::contracts::uart {

/*
 * UART frame
 * ----------
 * SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16
 *
 * LENGTH is the PAYLOAD byte count.
 * CRC16 is calculated over VERSION through the last PAYLOAD byte.
 * CRC16 is appended in little-endian order.
 *
 * The numeric command values below are kept in one block so that a future
 * contract change only requires editing this section and the UART README.
 */
namespace wire {

inline constexpr std::uint8_t kSof = 0xA5U;
inline constexpr std::uint8_t kProtocolVersion = 0x01U;
inline constexpr std::size_t kMaximumPayloadSize = 128U;

inline constexpr std::uint16_t kCrcPolynomial = 0x1021U;
inline constexpr std::uint16_t kCrcInitialValue = 0xFFFFU;
inline constexpr std::uint16_t kCrcXorOut = 0x0000U;

inline constexpr std::uint8_t kCommandConveyorStart = 0x10U;
inline constexpr std::uint8_t kCommandConveyorStop = 0x11U;
inline constexpr std::uint8_t kCommandRotateBox = 0x20U;
inline constexpr std::uint8_t kCommandResetRotator = 0x21U;
inline constexpr std::uint8_t kCommandGateSet = 0x30U;
inline constexpr std::uint8_t kCommandDeliveryStart = 0x40U;
inline constexpr std::uint8_t kCommandUnloadBox = 0x41U;
inline constexpr std::uint8_t kCommandTurnAround = 0x42U;
inline constexpr std::uint8_t kCommandReturnHome = 0x43U;
inline constexpr std::uint8_t kCommandStatusRequest = 0x70U;
inline constexpr std::uint8_t kCommandSensorStatus = 0x80U;
inline constexpr std::uint8_t kCommandCommandResult = 0x81U;
inline constexpr std::uint8_t kCommandErrorStatus = 0x82U;
inline constexpr std::uint8_t kCommandEmergencyStop = 0xF0U;

inline constexpr std::uint32_t kResponseTimeoutMilliseconds = 500U;
inline constexpr std::uint8_t kMaximumRetries = 3U;

}  // namespace wire

enum class UartCommand : std::uint8_t {
    kUnknown = 0xFFU,
    kConveyorStart = wire::kCommandConveyorStart,
    kConveyorStop = wire::kCommandConveyorStop,
    kRotateBox = wire::kCommandRotateBox,
    kResetRotator = wire::kCommandResetRotator,
    kGateSet = wire::kCommandGateSet,
    kDeliveryStart = wire::kCommandDeliveryStart,
    kUnloadBox = wire::kCommandUnloadBox,
    kTurnAround = wire::kCommandTurnAround,
    kReturnHome = wire::kCommandReturnHome,
    kStatusRequest = wire::kCommandStatusRequest,
    kSensorStatus = wire::kCommandSensorStatus,
    kCommandResult = wire::kCommandCommandResult,
    kErrorStatus = wire::kCommandErrorStatus,
    kEmergencyStop = wire::kCommandEmergencyStop,
};

enum class UartResult : std::uint8_t {
    kSuccess = 0x00U,
    kReceived = 0x01U,
    kProcessing = 0x02U,
    kInvalidCommand = 0x10U,
    kInvalidData = 0x11U,
    kDeviceBusy = 0x12U,
    kTimeout = 0x13U,
    kHardwareError = 0x14U,
    kCrcError = 0x15U,
    kEmergencyStop = 0x16U,
    kUnknown = 0xFFU,
};

/*
 * The detailed UART response contract states ERROR_CODE is zero when no error
 * exists and otherwise contains the related error code. Reusing the documented
 * RESULT error values avoids maintaining a second, conflicting numeric table.
 */
enum class UartErrorCode : std::uint8_t {
    kNone = 0x00U,
    kInvalidCommand = 0x10U,
    kInvalidData = 0x11U,
    kDeviceBusy = 0x12U,
    kTimeout = 0x13U,
    kHardwareError = 0x14U,
    kCrcError = 0x15U,
    kEmergencyStop = 0x16U,
    kUnknown = 0xFFU,
};

enum class CodecError : std::uint8_t {
    kNone,
    kFrameTooShort,
    kInvalidSof,
    kUnsupportedVersion,
    kUnknownCommand,
    kInvalidLength,
    kPayloadTooLarge,
    kCrcMismatch,
    kInvalidPayload,
    kTrailingBytes,
};

struct CodecStatus {
    CodecError error{ CodecError::kNone };
    std::string message;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error == CodecError::kNone;
    }
};

struct EncodeResult {
    std::vector<std::uint8_t> bytes;
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

struct UartFrame {
    std::uint8_t version{ wire::kProtocolVersion };
    std::uint8_t sequence{ 0U };
    UartCommand command{ UartCommand::kUnknown };
    std::vector<std::uint8_t> payload;

    [[nodiscard]] bool IsValid() const noexcept {
        return version == wire::kProtocolVersion && command != UartCommand::kUnknown &&
               payload.size() <= wire::kMaximumPayloadSize;
    }
};

/*
 * SEQUENCE is already present in the frame header, so it is not duplicated in
 * this payload.
 *
 * ORIGINAL_COMMAND | RESULT | ERROR_CODE
 */
struct CommandResultPayload {
    UartCommand original_command{ UartCommand::kUnknown };
    UartResult result{ UartResult::kUnknown };
    UartErrorCode error_code{ UartErrorCode::kNone };

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] constexpr std::string_view ToString(UartCommand command) noexcept {
    switch (command) {
        case UartCommand::kConveyorStart:
            return "CONVEYOR_START";
        case UartCommand::kConveyorStop:
            return "CONVEYOR_STOP";
        case UartCommand::kRotateBox:
            return "ROTATE_BOX";
        case UartCommand::kResetRotator:
            return "RESET_ROTATOR";
        case UartCommand::kGateSet:
            return "GATE_SET";
        case UartCommand::kDeliveryStart:
            return "DELIVERY_START";
        case UartCommand::kUnloadBox:
            return "UNLOAD_BOX";
        case UartCommand::kTurnAround:
            return "TURN_AROUND";
        case UartCommand::kReturnHome:
            return "RETURN_HOME";
        case UartCommand::kStatusRequest:
            return "STATUS_REQUEST";
        case UartCommand::kSensorStatus:
            return "SENSOR_STATUS";
        case UartCommand::kCommandResult:
            return "COMMAND_RESULT";
        case UartCommand::kErrorStatus:
            return "ERROR_STATUS";
        case UartCommand::kEmergencyStop:
            return "EMERGENCY_STOP";
        case UartCommand::kUnknown:
            break;
    }

    return "UNKNOWN";
}

[[nodiscard]] constexpr UartCommand UartCommandFromCode(std::uint8_t code) noexcept {
    constexpr std::array values = {
        UartCommand::kConveyorStart, UartCommand::kConveyorStop,  UartCommand::kRotateBox,
        UartCommand::kResetRotator,  UartCommand::kGateSet,       UartCommand::kDeliveryStart,
        UartCommand::kUnloadBox,     UartCommand::kTurnAround,    UartCommand::kReturnHome,
        UartCommand::kStatusRequest, UartCommand::kSensorStatus,  UartCommand::kCommandResult,
        UartCommand::kErrorStatus,   UartCommand::kEmergencyStop,
    };

    for (const UartCommand value : values) {
        if (static_cast<std::uint8_t>(value) == code) {
            return value;
        }
    }

    return UartCommand::kUnknown;
}

[[nodiscard]] constexpr std::string_view ToString(UartResult result) noexcept {
    switch (result) {
        case UartResult::kSuccess:
            return "RESULT_SUCCESS";
        case UartResult::kReceived:
            return "RESULT_RECEIVED";
        case UartResult::kProcessing:
            return "RESULT_PROCESSING";
        case UartResult::kInvalidCommand:
            return "RESULT_INVALID_COMMAND";
        case UartResult::kInvalidData:
            return "RESULT_INVALID_DATA";
        case UartResult::kDeviceBusy:
            return "RESULT_DEVICE_BUSY";
        case UartResult::kTimeout:
            return "RESULT_TIMEOUT";
        case UartResult::kHardwareError:
            return "RESULT_HARDWARE_ERROR";
        case UartResult::kCrcError:
            return "RESULT_CRC_ERROR";
        case UartResult::kEmergencyStop:
            return "RESULT_EMERGENCY_STOP";
        case UartResult::kUnknown:
            break;
    }

    return "RESULT_UNKNOWN";
}

[[nodiscard]] constexpr UartResult UartResultFromCode(std::uint8_t code) noexcept {
    constexpr std::array values = {
        UartResult::kSuccess,     UartResult::kReceived,      UartResult::kProcessing, UartResult::kInvalidCommand,
        UartResult::kInvalidData, UartResult::kDeviceBusy,    UartResult::kTimeout,    UartResult::kHardwareError,
        UartResult::kCrcError,    UartResult::kEmergencyStop,
    };

    for (const UartResult value : values) {
        if (static_cast<std::uint8_t>(value) == code) {
            return value;
        }
    }

    return UartResult::kUnknown;
}

[[nodiscard]] constexpr std::string_view ToString(UartErrorCode error) noexcept {
    switch (error) {
        case UartErrorCode::kNone:
            return "NONE";
        case UartErrorCode::kInvalidCommand:
            return "ERR_INVALID_COMMAND";
        case UartErrorCode::kInvalidData:
            return "ERR_INVALID_DATA";
        case UartErrorCode::kDeviceBusy:
            return "ERR_DEVICE_BUSY";
        case UartErrorCode::kTimeout:
            return "ERR_TIMEOUT";
        case UartErrorCode::kHardwareError:
            return "ERR_HARDWARE_ERROR";
        case UartErrorCode::kCrcError:
            return "ERR_CRC";
        case UartErrorCode::kEmergencyStop:
            return "ERR_EMERGENCY_STOP";
        case UartErrorCode::kUnknown:
            break;
    }

    return "ERR_UNKNOWN";
}

[[nodiscard]] constexpr UartErrorCode UartErrorCodeFromCode(std::uint8_t code) noexcept {
    constexpr std::array values = {
        UartErrorCode::kNone,       UartErrorCode::kInvalidCommand, UartErrorCode::kInvalidData,
        UartErrorCode::kDeviceBusy, UartErrorCode::kTimeout,        UartErrorCode::kHardwareError,
        UartErrorCode::kCrcError,   UartErrorCode::kEmergencyStop,
    };

    for (const UartErrorCode value : values) {
        if (static_cast<std::uint8_t>(value) == code) {
            return value;
        }
    }

    return UartErrorCode::kUnknown;
}

[[nodiscard]] constexpr std::string_view ToString(CodecError error) noexcept {
    switch (error) {
        case CodecError::kNone:
            return "NONE";
        case CodecError::kFrameTooShort:
            return "FRAME_TOO_SHORT";
        case CodecError::kInvalidSof:
            return "INVALID_SOF";
        case CodecError::kUnsupportedVersion:
            return "UNSUPPORTED_VERSION";
        case CodecError::kUnknownCommand:
            return "UNKNOWN_COMMAND";
        case CodecError::kInvalidLength:
            return "INVALID_LENGTH";
        case CodecError::kPayloadTooLarge:
            return "PAYLOAD_TOO_LARGE";
        case CodecError::kCrcMismatch:
            return "CRC_MISMATCH";
        case CodecError::kInvalidPayload:
            return "INVALID_PAYLOAD";
        case CodecError::kTrailingBytes:
            return "TRAILING_BYTES";
    }

    return "UNKNOWN_CODEC_ERROR";
}

[[nodiscard]] constexpr bool IsFailureResult(UartResult result) noexcept {
    return result == UartResult::kInvalidCommand || result == UartResult::kInvalidData ||
           result == UartResult::kDeviceBusy || result == UartResult::kTimeout ||
           result == UartResult::kHardwareError || result == UartResult::kCrcError ||
           result == UartResult::kEmergencyStop;
}

inline bool CommandResultPayload::IsValid() const noexcept {
    if (original_command == UartCommand::kUnknown || original_command == UartCommand::kCommandResult ||
        result == UartResult::kUnknown || error_code == UartErrorCode::kUnknown) {
        return false;
    }

    if (IsFailureResult(result)) {
        return error_code != UartErrorCode::kNone;
    }

    return error_code == UartErrorCode::kNone;
}

namespace codec_detail {

inline constexpr std::size_t kHeaderSize = 5U;
inline constexpr std::size_t kCrcSize = 2U;
inline constexpr std::size_t kMinimumFrameSize = kHeaderSize + kCrcSize;
inline constexpr std::size_t kCommandResultPayloadSize = 3U;

[[nodiscard]] inline CodecStatus MakeError(CodecError error, std::string_view message) {
    return {
        .error = error,
        .message = std::string(message),
    };
}

inline void AppendUint16LittleEndian(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0x00FFU));
}

[[nodiscard]] inline std::uint16_t ReadUint16LittleEndian(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

}  // namespace codec_detail

[[nodiscard]] inline std::uint16_t CalculateCrc16(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (bytes == nullptr && size != 0U) {
        return 0U;
    }

    std::uint16_t crc = wire::kCrcInitialValue;

    for (std::size_t index = 0U; index < size; ++index) {
        crc ^= static_cast<std::uint16_t>(bytes[index]) << 8U;

        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1U) ^ wire::kCrcPolynomial);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1U);
            }
        }
    }

    return static_cast<std::uint16_t>(crc ^ wire::kCrcXorOut);
}

[[nodiscard]] inline std::uint16_t CalculateCrc16(const std::vector<std::uint8_t>& bytes) noexcept {
    return CalculateCrc16(bytes.data(), bytes.size());
}

[[nodiscard]] inline EncodeResult EncodeFrame(const UartFrame& frame) {
    EncodeResult result;

    if (frame.payload.size() > wire::kMaximumPayloadSize) {
        result.status = codec_detail::MakeError(CodecError::kPayloadTooLarge, "UART payload is too large");
        return result;
    }

    if (!frame.IsValid()) {
        result.status = codec_detail::MakeError(CodecError::kInvalidPayload, "UART frame is invalid");
        return result;
    }

    result.bytes.reserve(codec_detail::kMinimumFrameSize + frame.payload.size());
    result.bytes.push_back(wire::kSof);
    result.bytes.push_back(frame.version);
    result.bytes.push_back(frame.sequence);
    result.bytes.push_back(static_cast<std::uint8_t>(frame.command));
    result.bytes.push_back(static_cast<std::uint8_t>(frame.payload.size()));
    result.bytes.insert(result.bytes.end(), frame.payload.begin(), frame.payload.end());

    const std::uint16_t crc = CalculateCrc16(result.bytes.data() + 1U, result.bytes.size() - 1U);
    codec_detail::AppendUint16LittleEndian(result.bytes, crc);
    return result;
}

[[nodiscard]] inline DecodeResult<UartFrame> DecodeFrame(const std::uint8_t* bytes, std::size_t size) {
    DecodeResult<UartFrame> result;

    if (bytes == nullptr || size < codec_detail::kMinimumFrameSize) {
        result.status = codec_detail::MakeError(CodecError::kFrameTooShort, "UART frame is too short");
        return result;
    }

    if (bytes[0] != wire::kSof) {
        result.status = codec_detail::MakeError(CodecError::kInvalidSof, "UART SOF does not match");
        return result;
    }

    if (bytes[1] != wire::kProtocolVersion) {
        result.status = codec_detail::MakeError(CodecError::kUnsupportedVersion, "Unsupported UART protocol version");
        return result;
    }

    const std::size_t payload_size = bytes[4];

    if (payload_size > wire::kMaximumPayloadSize) {
        result.status = codec_detail::MakeError(CodecError::kPayloadTooLarge, "UART payload is too large");
        return result;
    }

    const std::size_t expected_size = codec_detail::kMinimumFrameSize + payload_size;

    if (size < expected_size) {
        result.status = codec_detail::MakeError(CodecError::kInvalidLength, "UART frame is truncated");
        return result;
    }

    if (size > expected_size) {
        result.status = codec_detail::MakeError(CodecError::kTrailingBytes, "UART frame has trailing bytes");
        return result;
    }

    const UartCommand command = UartCommandFromCode(bytes[3]);

    if (command == UartCommand::kUnknown) {
        result.status = codec_detail::MakeError(CodecError::kUnknownCommand, "Unknown UART command code");
        return result;
    }

    const std::size_t crc_offset = codec_detail::kHeaderSize + payload_size;
    const std::uint16_t received_crc = codec_detail::ReadUint16LittleEndian(bytes + crc_offset);
    const std::uint16_t calculated_crc = CalculateCrc16(bytes + 1U, 4U + payload_size);

    if (received_crc != calculated_crc) {
        result.status = codec_detail::MakeError(CodecError::kCrcMismatch, "UART CRC16 does not match");
        return result;
    }

    result.value.version = bytes[1];
    result.value.sequence = bytes[2];
    result.value.command = command;
    result.value.payload.assign(bytes + codec_detail::kHeaderSize, bytes + crc_offset);
    return result;
}

[[nodiscard]] inline DecodeResult<UartFrame> DecodeFrame(const std::vector<std::uint8_t>& bytes) {
    return DecodeFrame(bytes.data(), bytes.size());
}

[[nodiscard]] inline EncodeResult EncodeCommandResultPayload(const CommandResultPayload& payload) {
    EncodeResult result;

    if (!payload.IsValid()) {
        result.status = codec_detail::MakeError(CodecError::kInvalidPayload, "COMMAND_RESULT payload is invalid");
        return result;
    }

    result.bytes.reserve(codec_detail::kCommandResultPayloadSize);
    result.bytes.push_back(static_cast<std::uint8_t>(payload.original_command));
    result.bytes.push_back(static_cast<std::uint8_t>(payload.result));
    result.bytes.push_back(static_cast<std::uint8_t>(payload.error_code));
    return result;
}

[[nodiscard]] inline DecodeResult<CommandResultPayload> DecodeCommandResultPayload(const std::uint8_t* bytes,
                                                                                   std::size_t size) {
    DecodeResult<CommandResultPayload> result;

    if (bytes == nullptr || size != codec_detail::kCommandResultPayloadSize) {
        result.status = codec_detail::MakeError(CodecError::kInvalidLength, "COMMAND_RESULT payload must be 3 bytes");
        return result;
    }

    result.value.original_command = UartCommandFromCode(bytes[0]);
    result.value.result = UartResultFromCode(bytes[1]);
    result.value.error_code = UartErrorCodeFromCode(bytes[2]);

    if (!result.value.IsValid()) {
        result.status =
            codec_detail::MakeError(CodecError::kInvalidPayload, "COMMAND_RESULT payload contains invalid values");
    }

    return result;
}

[[nodiscard]] inline DecodeResult<CommandResultPayload> DecodeCommandResultPayload(
    const std::vector<std::uint8_t>& bytes) {
    return DecodeCommandResultPayload(bytes.data(), bytes.size());
}

namespace bridge {

enum class Error : std::uint8_t {
    kNone,
    kInvalidMqttMessage,
    kUnsupportedMessageType,
    kUnsupportedCommand,
    kCommandResolverRequired,
    kPayloadBuilderRequired,
    kInvalidPayload,
    kUnexpectedUartCommand,
    kMissingRequestContext,
    kInvalidTimestamp,
};

struct Status {
    Error error{ Error::kNone };
    std::string message;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error == Error::kNone;
    }
};

struct MqttToUartResult {
    UartFrame frame;
    Status status;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return status.IsSuccess();
    }
};

struct UartToMqttResult {
    mqtt::MqttMessage message;
    Status status;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return status.IsSuccess();
    }
};

struct PendingRequestContext {
    std::string request_id;
    mqtt::ControlCommand mqtt_command{ mqtt::ControlCommand::kUnknown };
};

using CommandResolver = UartCommand (*)(const mqtt::MqttMessage& message);
using PayloadBuilder = bool (*)(const mqtt::MqttMessage& message, UartCommand command,
                                std::vector<std::uint8_t>& output_payload);

[[nodiscard]] constexpr std::string_view ToString(Error error) noexcept {
    switch (error) {
        case Error::kNone:
            return "NONE";
        case Error::kInvalidMqttMessage:
            return "INVALID_MQTT_MESSAGE";
        case Error::kUnsupportedMessageType:
            return "UNSUPPORTED_MESSAGE_TYPE";
        case Error::kUnsupportedCommand:
            return "UNSUPPORTED_COMMAND";
        case Error::kCommandResolverRequired:
            return "COMMAND_RESOLVER_REQUIRED";
        case Error::kPayloadBuilderRequired:
            return "PAYLOAD_BUILDER_REQUIRED";
        case Error::kInvalidPayload:
            return "INVALID_PAYLOAD";
        case Error::kUnexpectedUartCommand:
            return "UNEXPECTED_UART_COMMAND";
        case Error::kMissingRequestContext:
            return "MISSING_REQUEST_CONTEXT";
        case Error::kInvalidTimestamp:
            return "INVALID_TIMESTAMP";
    }

    return "UNKNOWN_BRIDGE_ERROR";
}

[[nodiscard]] constexpr bool RequiresPayload(UartCommand command) noexcept {
    return command == UartCommand::kConveyorStart || command == UartCommand::kRotateBox ||
           command == UartCommand::kGateSet || command == UartCommand::kDeliveryStart;
}

[[nodiscard]] inline UartCommand ResolveStandardCommand(const mqtt::MqttMessage& message) noexcept {
    if (message.message_type == mqtt::MessageType::kEmergencyStop) {
        return UartCommand::kEmergencyStop;
    }

    if (message.message_type != mqtt::MessageType::kControlCommand) {
        return UartCommand::kUnknown;
    }

    const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(message);

    if (payload == nullptr) {
        return UartCommand::kUnknown;
    }

    switch (payload->command) {
        case mqtt::ControlCommand::kStatusRequest:
            return UartCommand::kStatusRequest;
        case mqtt::ControlCommand::kEmergencyStop:
            return UartCommand::kEmergencyStop;
        default:
            break;
    }

    return UartCommand::kUnknown;
}

[[nodiscard]] inline mqtt::CommandResult ToMqttCommandResult(UartResult result) noexcept {
    switch (result) {
        case UartResult::kReceived:
            return mqtt::CommandResult::kReceived;
        case UartResult::kProcessing:
            return mqtt::CommandResult::kProcessing;
        case UartResult::kSuccess:
            return mqtt::CommandResult::kSuccess;
        case UartResult::kInvalidCommand:
        case UartResult::kInvalidData:
        case UartResult::kDeviceBusy:
            return mqtt::CommandResult::kRejected;
        case UartResult::kTimeout:
            return mqtt::CommandResult::kTimeout;
        case UartResult::kHardwareError:
        case UartResult::kCrcError:
        case UartResult::kEmergencyStop:
            return mqtt::CommandResult::kFailed;
        case UartResult::kUnknown:
            break;
    }

    return mqtt::CommandResult::kUnknown;
}

[[nodiscard]] inline std::optional<std::string> ToMqttErrorCode(UartErrorCode error) {
    if (error == UartErrorCode::kNone) {
        return std::nullopt;
    }

    if (error == UartErrorCode::kUnknown) {
        return std::string("ERR_UNKNOWN");
    }

    return std::string(::logistics::contracts::uart::ToString(error));
}

[[nodiscard]] inline MqttToUartResult ConvertMqttToUart(const mqtt::MqttMessage& message, std::uint8_t sequence,
                                                        CommandResolver command_resolver = nullptr,
                                                        PayloadBuilder payload_builder = nullptr) {
    MqttToUartResult result;

    if (!message.IsValid()) {
        result.status.error = Error::kInvalidMqttMessage;
        result.status.message = "MQTT message is invalid";
        return result;
    }

    if (message.message_type != mqtt::MessageType::kControlCommand &&
        message.message_type != mqtt::MessageType::kDestinationSet &&
        message.message_type != mqtt::MessageType::kEmergencyStop) {
        result.status.error = Error::kUnsupportedMessageType;
        result.status.message = "MQTT message type cannot be converted to UART";
        return result;
    }

    UartCommand uart_command = ResolveStandardCommand(message);

    if (uart_command == UartCommand::kUnknown && command_resolver != nullptr) {
        uart_command = command_resolver(message);
    }

    if (uart_command == UartCommand::kUnknown) {
        result.status.error =
            command_resolver == nullptr ? Error::kCommandResolverRequired : Error::kUnsupportedCommand;
        result.status.message = "A device-specific MQTT-to-UART command mapping is required";
        return result;
    }

    std::vector<std::uint8_t> uart_payload;

    if (payload_builder != nullptr) {
        if (!payload_builder(message, uart_command, uart_payload)) {
            result.status.error = Error::kInvalidPayload;
            result.status.message = "UART payload builder rejected the MQTT message";
            return result;
        }
    } else if (RequiresPayload(uart_command)) {
        result.status.error = Error::kPayloadBuilderRequired;
        result.status.message = "A payload builder is required for this UART command";
        return result;
    }

    if (uart_payload.size() > wire::kMaximumPayloadSize) {
        result.status.error = Error::kInvalidPayload;
        result.status.message = "Converted UART payload is too large";
        return result;
    }

    result.frame.version = wire::kProtocolVersion;
    result.frame.sequence = sequence;
    result.frame.command = uart_command;
    result.frame.payload = std::move(uart_payload);
    return result;
}

[[nodiscard]] inline UartToMqttResult ConvertUartCommandResultToMqtt(const UartFrame& frame,
                                                                     const PendingRequestContext& context,
                                                                     std::string message_id, std::string source_id,
                                                                     std::string timestamp) {
    UartToMqttResult result;

    if (frame.command != UartCommand::kCommandResult) {
        result.status.error = Error::kUnexpectedUartCommand;
        result.status.message = "UART frame is not COMMAND_RESULT";
        return result;
    }

    if (context.request_id.empty() || context.mqtt_command == mqtt::ControlCommand::kUnknown) {
        result.status.error = Error::kMissingRequestContext;
        result.status.message = "MQTT request context is missing";
        return result;
    }

    if (!mqtt::IsValidIso8601Timestamp(timestamp)) {
        result.status.error = Error::kInvalidTimestamp;
        result.status.message = "MQTT timestamp is not a valid ISO 8601 timestamp";
        return result;
    }

    const DecodeResult<CommandResultPayload> decoded = DecodeCommandResultPayload(frame.payload);

    if (!decoded.IsSuccess()) {
        result.status.error = Error::kInvalidPayload;
        result.status.message = decoded.status.message;
        return result;
    }

    const mqtt::CommandResult mqtt_result = ToMqttCommandResult(decoded.value.result);

    if (mqtt_result == mqtt::CommandResult::kUnknown) {
        result.status.error = Error::kInvalidPayload;
        result.status.message = "UART result cannot be mapped to MQTT";
        return result;
    }

    mqtt::CommandResponsePayload response_payload;
    response_payload.request_id = context.request_id;
    response_payload.command = context.mqtt_command;
    response_payload.result = mqtt_result;
    response_payload.error_code = ToMqttErrorCode(decoded.value.error_code);
    response_payload.message = std::string(::logistics::contracts::uart::ToString(decoded.value.result));

    result.message.protocol_version = std::string(mqtt::kCurrentProtocolVersion);
    result.message.message_id = std::move(message_id);
    result.message.message_type = mqtt::MessageType::kCommandResponse;
    result.message.source_id = std::move(source_id);
    result.message.timestamp = std::move(timestamp);
    result.message.data = std::move(response_payload);

    if (!result.message.IsValid()) {
        result.status.error = Error::kInvalidPayload;
        result.status.message = "Converted MQTT COMMAND_RESPONSE is invalid";
    }

    return result;
}

}  // namespace bridge

}  // namespace logistics::contracts::uart
