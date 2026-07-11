#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::contracts::mqtt {

    inline constexpr std::string_view kMessageTypeField = "messageType";
    inline constexpr std::string_view kDeviceIdField = "deviceId";
    inline constexpr std::string_view kTimestampField = "timestamp";
    inline constexpr std::string_view kDataField = "data";
    inline constexpr std::string_view kRequestIdField = "requestId";

    enum class MessageType : std::uint8_t {
        kUnknown,
        kDeviceRegister,
        kHeartbeat,
        kBoxDetected,
        kPositionDetected,
        kBarcodeDetected,
        kProductImage,
        kProductInfo,
        kDestinationSet,
        kDeviceStatus,
        kControlCommand,
        kErrorOccurred,
        kEmergencyStop,
        kCommandResponse,
    };

    enum class ControlCommand : std::uint8_t {
        kUnknown,
        kStart,
        kStop,
        kRestart,
        kInitialize,
        kStatusRequest,
        kEmergencyStop,
        kRecovery,
        kDestinationSet,
    };

    enum class CommandResult : std::uint8_t {
        kUnknown,
        kReceived,
        kProcessing,
        kSuccess,
        kFailed,
        kRejected,
        kTimeout,
        kDuplicated,
    };

    enum class ConnectionState : std::uint8_t {
        kUnknown,
        kOnline,
        kDelayed,
        kOffline,
        kReconnecting,
        kRtspError,
        kMqttError,
        kMqttAuthError,
        kTlsError,
        kUartError,
    };

    enum class Qos : std::uint8_t { kAtMostOnce = 0, kAtLeastOnce = 1 };

    enum class RetainPolicy : std::uint8_t {
        kNever,
        kLatestStateAllowed,
        kAllowed,
    };

    struct DeliveryPolicy {
        Qos minimum_qos{ Qos::kAtLeastOnce };
        Qos maximum_qos{ Qos::kAtLeastOnce };
        RetainPolicy retain{ RetainPolicy::kNever };
    };

    struct EnvelopeView {
        MessageType message_type{ MessageType::kUnknown };
        std::string_view device_id{};
        std::string_view timestamp{};
        std::string_view data_json{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return message_type != MessageType::kUnknown && IsValidTopicLevel(device_id) && !timestamp.empty() &&
                !data_json.empty();
        }
    };

    struct CommandRequestView {
        std::string_view request_id{};
        ControlCommand command{ ControlCommand::kUnknown };
        std::string_view target_device{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return IsValidTopicLevel(request_id) && command != ControlCommand::kUnknown && IsValidTopicLevel(target_device);
        }
    };

    struct CommandResponseView {
        std::string_view request_id{};
        ControlCommand command{ ControlCommand::kUnknown };
        CommandResult result{ CommandResult::kUnknown };

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return IsValidTopicLevel(request_id) && command != ControlCommand::kUnknown &&
                result != CommandResult::kUnknown;
        }
    };

    inline constexpr auto kHeartbeatInterval = std::chrono::seconds{ 5 };
    inline constexpr auto kHeartbeatDelayedAfter = std::chrono::seconds{ 10 };
    inline constexpr auto kHeartbeatOfflineAfter = std::chrono::seconds{ 15 };
    inline constexpr auto kMqttResponseTimeout = std::chrono::seconds{ 3 };
    inline constexpr auto kEmergencyStopConfirmationTimeout = std::chrono::seconds{ 1 };
    inline constexpr std::uint8_t kMqttMaximumRetries = 3;

    [[nodiscard]] constexpr std::string_view ToString(MessageType type) noexcept {
        switch (type) {
        case MessageType::kDeviceRegister:
            return "DEVICE_REGISTER";
        case MessageType::kHeartbeat:
            return "HEARTBEAT";
        case MessageType::kBoxDetected:
            return "BOX_DETECTED";
        case MessageType::kPositionDetected:
            return "POSITION_DETECTED";
        case MessageType::kBarcodeDetected:
            return "BARCODE_DETECTED";
        case MessageType::kProductImage:
            return "PRODUCT_IMAGE";
        case MessageType::kProductInfo:
            return "PRODUCT_INFO";
        case MessageType::kDestinationSet:
            return "DESTINATION_SET";
        case MessageType::kDeviceStatus:
            return "DEVICE_STATUS";
        case MessageType::kControlCommand:
            return "CONTROL_COMMAND";
        case MessageType::kErrorOccurred:
            return "ERROR_OCCURRED";
        case MessageType::kEmergencyStop:
            return "EMERGENCY_STOP";
        case MessageType::kCommandResponse:
            return "COMMAND_RESPONSE";
        case MessageType::kUnknown:
            break;
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr MessageType MessageTypeFromString(std::string_view value) noexcept {
        constexpr MessageType values[] = {
            MessageType::kDeviceRegister,   MessageType::kHeartbeat,       MessageType::kBoxDetected,
            MessageType::kPositionDetected, MessageType::kBarcodeDetected, MessageType::kProductImage,
            MessageType::kProductInfo,      MessageType::kDestinationSet,  MessageType::kDeviceStatus,
            MessageType::kControlCommand,   MessageType::kErrorOccurred,   MessageType::kEmergencyStop,
            MessageType::kCommandResponse,
        };
        for (const auto type : values) {
            if (ToString(type) == value)
                return type;
        }
        return MessageType::kUnknown;
    }

    [[nodiscard]] constexpr std::string_view ToString(ControlCommand command) noexcept {
        switch (command) {
        case ControlCommand::kStart:
            return "START";
        case ControlCommand::kStop:
            return "STOP";
        case ControlCommand::kRestart:
            return "RESTART";
        case ControlCommand::kInitialize:
            return "INITIALIZE";
        case ControlCommand::kStatusRequest:
            return "STATUS_REQUEST";
        case ControlCommand::kEmergencyStop:
            return "EMERGENCY_STOP";
        case ControlCommand::kRecovery:
            return "RECOVERY";
        case ControlCommand::kDestinationSet:
            return "DESTINATION_SET";
        case ControlCommand::kUnknown:
            break;
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr ControlCommand ControlCommandFromString(std::string_view value) noexcept {
        constexpr ControlCommand values[] = {
            ControlCommand::kStart,      ControlCommand::kStop,           ControlCommand::kRestart,
            ControlCommand::kInitialize, ControlCommand::kStatusRequest,  ControlCommand::kEmergencyStop,
            ControlCommand::kRecovery,   ControlCommand::kDestinationSet,
        };
        for (const auto command : values) {
            if (ToString(command) == value)
                return command;
        }
        return ControlCommand::kUnknown;
    }

    [[nodiscard]] constexpr std::string_view ToString(CommandResult result) noexcept {
        switch (result) {
        case CommandResult::kReceived:
            return "RECEIVED";
        case CommandResult::kProcessing:
            return "PROCESSING";
        case CommandResult::kSuccess:
            return "SUCCESS";
        case CommandResult::kFailed:
            return "FAILED";
        case CommandResult::kRejected:
            return "REJECTED";
        case CommandResult::kTimeout:
            return "TIMEOUT";
        case CommandResult::kDuplicated:
            return "DUPLICATED";
        case CommandResult::kUnknown:
            break;
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr CommandResult CommandResultFromString(std::string_view value) noexcept {
        constexpr CommandResult values[] = {
            CommandResult::kReceived, CommandResult::kProcessing, CommandResult::kSuccess,    CommandResult::kFailed,
            CommandResult::kRejected, CommandResult::kTimeout,    CommandResult::kDuplicated,
        };
        for (const auto result : values) {
            if (ToString(result) == value)
                return result;
        }
        return CommandResult::kUnknown;
    }

    [[nodiscard]] constexpr std::string_view ToString(ConnectionState state) noexcept {
        switch (state) {
        case ConnectionState::kOnline:
            return "ONLINE";
        case ConnectionState::kDelayed:
            return "DELAYED";
        case ConnectionState::kOffline:
            return "OFFLINE";
        case ConnectionState::kReconnecting:
            return "RECONNECTING";
        case ConnectionState::kRtspError:
            return "RTSP_ERROR";
        case ConnectionState::kMqttError:
            return "MQTT_ERROR";
        case ConnectionState::kMqttAuthError:
            return "MQTT_AUTH_ERROR";
        case ConnectionState::kTlsError:
            return "TLS_ERROR";
        case ConnectionState::kUartError:
            return "UART_ERROR";
        case ConnectionState::kUnknown:
            break;
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr DeliveryPolicy PolicyFor(MessageType type) noexcept {
        switch (type) {
        case MessageType::kHeartbeat:
            return { Qos::kAtMostOnce, Qos::kAtMostOnce, RetainPolicy::kNever };
        case MessageType::kDeviceStatus:
            return { Qos::kAtMostOnce, Qos::kAtLeastOnce, RetainPolicy::kLatestStateAllowed };
        case MessageType::kErrorOccurred:
            return { Qos::kAtLeastOnce, Qos::kAtLeastOnce, RetainPolicy::kAllowed };
        default:
            return { Qos::kAtLeastOnce, Qos::kAtLeastOnce, RetainPolicy::kNever };
        }
    }

    [[nodiscard]] constexpr bool IsTerminal(CommandResult result) noexcept {
        return result == CommandResult::kSuccess || result == CommandResult::kFailed ||
            result == CommandResult::kRejected || result == CommandResult::kTimeout ||
            result == CommandResult::kDuplicated;
    }

    [[nodiscard]] constexpr ConnectionState ConnectionStateForHeartbeatAge(std::chrono::seconds age) noexcept {
        if (age >= kHeartbeatOfflineAfter) {
            return ConnectionState::kOffline;
        }
        if (age >= kHeartbeatDelayedAfter) {
            return ConnectionState::kDelayed;
        }
        return ConnectionState::kOnline;
    }

}  // namespace logistics::contracts::mqtt
