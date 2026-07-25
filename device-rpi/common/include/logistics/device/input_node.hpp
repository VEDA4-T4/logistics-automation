#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/device/input_uart_session.hpp"

namespace logistics::device {

enum class InputCommandStatus {
    kSuccess,
    kRejected,
    kTimeout,
    kUnsupportedMessage,
    kUnsupportedCommand,
    kInvalidMessage,
    kInvalidTarget,
    kUartNotOpen,
    kUartError,
};

struct InputCommandResult {
    InputCommandStatus status{ InputCommandStatus::kInvalidMessage };
    contracts::mqtt::ControlCommand mqtt_command{ contracts::mqtt::ControlCommand::kUnknown };
    std::string request_id;
    std::uint8_t uart_command{};
    InputTransactResult uart_result{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == InputCommandStatus::kSuccess;
    }
};

enum class InputReportChannel {
    kResponse,
    kStatus,
    kEvent,
    kError,
};

struct InputReport {
    InputReportChannel channel{ InputReportChannel::kStatus };
    contracts::mqtt::MessageType message_type{ contracts::mqtt::MessageType::kUnknown };
    contracts::mqtt::MessagePayload data;
};

using InputReportHandler = std::function<void(const InputReport& report)>;

/*
 * Translates validated MQTT control commands into input-conveyor UART commands
 * and turns the controller's spontaneous SENSOR_STATUS/DEVICE_STATUS/EVENT
 * frames into MQTT status, error and command-response reports.
 *
 * Because the input controller answers each command synchronously (see
 * InputUartSession), HandleMqttCommand performs the UART transaction inline and
 * emits the COMMAND_RESPONSE report itself; the caller only needs to relay the
 * emitted reports.
 */
class InputNode final {
public:
    InputNode(std::string device_id, InputUartSession& uart_session);

    void SetReportHandler(InputReportHandler handler);

    [[nodiscard]] InputCommandResult HandleMqttCommand(const contracts::mqtt::MqttMessage& message);
    void HandleUartFrame(const uart_frame_t& frame);

private:
    [[nodiscard]] InputCommandResult HandleControlCommand(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] InputCommandResult HandleEmergencyStop(const contracts::mqtt::EmergencyStopPayload& command);
    [[nodiscard]] InputCommandResult Execute(InputCommandResult result, std::uint8_t command,
                                             std::span<const std::uint8_t> payload);
    // For commands the controller answers with an asynchronous EVENT/DEVICE_STATUS
    // broadcast instead of a sequence-matched reply (EMERGENCY_STOP, RESET_DEVICE):
    // write once and do not wait, so the session does not time out and retry.
    [[nodiscard]] InputCommandResult ExecuteAsync(InputCommandResult result, std::uint8_t command,
                                                  std::span<const std::uint8_t> payload);
    void EmitConveyorStatus(const uart_frame_t& response) const;
    void EmitCommandResponse(const InputCommandResult& result, std::string message) const;
    void EmitReport(InputReport report) const;
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;

    void HandleSensorStatus(const uart_frame_t& frame);
    void HandleDeviceStatus(const uart_frame_t& frame);
    void HandleControllerEvent(const uart_frame_t& frame);

    std::string device_id_;
    InputUartSession& uart_session_;
    InputReportHandler report_handler_;
    std::optional<std::uint8_t> last_sensor_state_;
};

}  // namespace logistics::device
