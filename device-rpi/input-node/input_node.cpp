#include "logistics/device/input_node.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/uart/conveyor_events.h"
#include "logistics/device/device_control_policy.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] mqtt::CommandResult CommandResultForStatus(InputCommandStatus status) noexcept {
    switch (status) {
        case InputCommandStatus::kSuccess:
            return mqtt::CommandResult::kSuccess;
        case InputCommandStatus::kRejected:
        case InputCommandStatus::kUnsupportedMessage:
        case InputCommandStatus::kUnsupportedCommand:
        case InputCommandStatus::kInvalidSpeed:
        case InputCommandStatus::kInvalidTarget:
            return mqtt::CommandResult::kRejected;
        case InputCommandStatus::kControllerFailure:
            return mqtt::CommandResult::kFailed;
        case InputCommandStatus::kTimeout:
            return mqtt::CommandResult::kTimeout;
        case InputCommandStatus::kInvalidMessage:
        case InputCommandStatus::kUartNotOpen:
        case InputCommandStatus::kUartError:
            return mqtt::CommandResult::kFailed;
    }
    return mqtt::CommandResult::kFailed;
}

[[nodiscard]] InputCommandStatus CommandStatusFromTransact(InputTransactStatus status) noexcept {
    switch (status) {
        case InputTransactStatus::kSuccess:
        case InputTransactStatus::kAccepted:
        case InputTransactStatus::kSent:
            return InputCommandStatus::kSuccess;
        case InputTransactStatus::kRejected:
            return InputCommandStatus::kRejected;
        case InputTransactStatus::kTimeout:
            return InputCommandStatus::kTimeout;
        case InputTransactStatus::kNotOpen:
            return InputCommandStatus::kUartNotOpen;
        case InputTransactStatus::kTransportError:
        case InputTransactStatus::kInvalidArgument:
        case InputTransactStatus::kEncodeError:
            return InputCommandStatus::kUartError;
    }
    return InputCommandStatus::kUartError;
}

[[nodiscard]] bool IsControllerExecutionFailure(std::uint8_t error) noexcept {
    switch (error) {
        case UART_ERROR_TIMEOUT:
        case UART_ERROR_SENSOR:
        case UART_ERROR_MOTOR:
        case UART_ERROR_SERVO:
        case UART_ERROR_INTERNAL:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::string UartErrorCode(std::uint8_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
            return "ERR-UART-TIMEOUT";
        case UART_ERROR_BUSY:
            return "ERR-UART-BUSY";
        case UART_ERROR_SEQUENCE:
            return "ERR-UART-SEQUENCE";
        case UART_ERROR_SENSOR:
            return "ERR-SENSOR";
        case UART_ERROR_MOTOR:
            return "ERR-MOTOR";
        case UART_ERROR_SERVO:
            return "ERR-SERVO";
        case UART_ERROR_EMERGENCY_STOP:
            return "ERR-EMERGENCY-STOP";
        case UART_ERROR_INVALID_PAYLOAD:
            return "ERR-UART-INVALID-PAYLOAD";
        case UART_ERROR_SPEED_NOT_CONFIGURED:
            return "ERR-SPEED-NOT-CONFIGURED";
        case UART_ERROR_UNSUPPORTED_COMMAND:
            return "ERR-UART-UNSUPPORTED";
        case UART_ERROR_INTERNAL:
            return "ERR-INTERNAL";
        default:
            return "ERR-UART-CONTROLLER";
    }
}

[[nodiscard]] std::optional<std::string> ErrorCodeForResult(const InputCommandResult& result) {
    switch (result.status) {
        case InputCommandStatus::kSuccess:
            return std::nullopt;
        case InputCommandStatus::kRejected:
            return UartErrorCode(result.uart_result.response_error);
        case InputCommandStatus::kControllerFailure:
            return UartErrorCode(result.uart_result.response_error);
        case InputCommandStatus::kTimeout:
            return std::string("ERR-UART-ACK-TIMEOUT");
        case InputCommandStatus::kUartNotOpen:
            return std::string("ERR-UART-DISCONNECTED");
        case InputCommandStatus::kUartError:
            return std::string("ERR-UART-IO");
        case InputCommandStatus::kInvalidTarget:
            return std::string("ERR-MQTT-INVALID-TARGET");
        case InputCommandStatus::kInvalidSpeed:
            return std::string("ERR-SPEED-INVALID");
        case InputCommandStatus::kUnsupportedMessage:
        case InputCommandStatus::kUnsupportedCommand:
            return std::string("ERR-UNSUPPORTED-COMMAND");
        case InputCommandStatus::kInvalidMessage:
            return std::string("ERR-MQTT-INVALID-MESSAGE");
    }
    return std::string("ERR-INTERNAL");
}

// SensorStatusPayload.measurement_status only accepts these two values
// (IsValidMeasurementStatus in mqtt_codec.hpp). It reports whether the reading
// is trustworthy - box presence is derived by the central server from
// distanceCm, so this node relays the measurement without judging it.
[[nodiscard]] std::string MeasurementStatusName(std::uint8_t state) {
    switch (state) {
        case UART_SENSOR_FAULT:
            return "FAULT";
        case UART_SENSOR_OK:
        default:
            return "OK";
    }
}

[[nodiscard]] std::string DeviceStateName(std::uint8_t state) {
    switch (state) {
        case UART_DEVICE_IDLE:
            return "IDLE";
        case UART_DEVICE_READY:
            return "READY";
        case UART_DEVICE_RUNNING:
            return "RUNNING";
        case UART_DEVICE_STOPPED:
            return "STOPPED";
        case UART_DEVICE_BUSY:
            return "BUSY";
        case UART_DEVICE_ERROR:
            return "ERROR";
        case UART_DEVICE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] std::string ConveyorStateName(std::uint8_t state) {
    switch (state) {
        case UART_INPUT_CONVEYOR_STOPPED:
            return "STOPPED";
        case UART_INPUT_CONVEYOR_RUNNING:
            return "RUNNING";
        case UART_INPUT_CONVEYOR_FAULT:
            return "FAULT";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] std::optional<std::int32_t> SensorDistanceCm(const uart_frame_t& frame) {
    const std::uint16_t distance =
        static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_LOW_INDEX]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_HIGH_INDEX]) << 8U);
    if (distance == UART_SENSOR_DISTANCE_UNKNOWN) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(distance);
}

constexpr auto kControllerHeartbeatTimeout = std::chrono::seconds{ 3 };

struct ControllerEventDescription {
    std::string error_code;
    std::string error_level;
    std::string message;
};

[[nodiscard]] ControllerEventDescription DescribeControllerEvent(std::uint8_t event_id, std::uint8_t kind,
                                                                 std::uint8_t cause,
                                                                 std::optional<std::uint8_t> sensor_id) {
    const std::string cause_suffix = " (cause=" + std::to_string(static_cast<int>(cause)) + ")";
    switch (event_id) {
        case APP_EVENT_SAFETY:
            switch (kind) {
                case 1U:  // SAFETY_EVENT_ESTOP_LATCHED
                    return { "ERR-SAFETY-ESTOP-LATCHED", "ERROR", "input controller latched an emergency stop" };
                // SAFETY_EVENT_RESET_COMPLETE (2) is a success notification and is
                // reported as a status by the caller, so it never reaches here.
                case 3U:  // SAFETY_EVENT_RESET_REJECTED
                    return { "ERR-SAFETY-RESET-REJECTED", "ERROR", "input controller rejected safety reset" };
                default:
                    return { "ERR-SAFETY-EVENT-" + std::to_string(static_cast<int>(kind)), "WARNING",
                             "input controller safety event" + cause_suffix };
            }
        case APP_EVENT_HEALTH:
            switch (kind) {
                case 1U:  // HEALTH_ISSUE_UART_CHANNEL_TIMEOUT
                    return { "ERR-HEALTH-UART-CHANNEL-TIMEOUT", "WARNING",
                             "input controller reported a UART channel timeout" + cause_suffix };
                case 2U:  // HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT
                    return { "ERR-HEALTH-QUEUE-OVERFLOW", "WARNING",
                             "input controller reported a transient queue overflow" + cause_suffix };
                case HEALTH_ISSUE_SENSOR_STALE: {
                    const std::string sensor_suffix =
                        sensor_id.has_value() ? " sensorId=" + std::to_string(static_cast<int>(*sensor_id)) : "";
                    return { "ERR-HEALTH-SENSOR-STALE", "WARNING",
                             "input controller reported a stale sensor" + sensor_suffix + cause_suffix };
                }
                case 4U:  // HEALTH_ISSUE_UART_RECOVERY
                    // Recovery started, not a confirmed loss - the frame is retried, and an
                    // exhausted retry surfaces separately as a queue overflow. Still WARNING:
                    // a line that keeps needing recovery is the precursor to those drops.
                    return { "ERR-HEALTH-UART-RECOVERY", "WARNING",
                             "input controller recovered from a UART error" + cause_suffix };
                default:
                    return { "ERR-HEALTH-EVENT-" + std::to_string(static_cast<int>(kind)), "WARNING",
                             "input controller health event" + cause_suffix };
            }
        default:
            return { "ERR-CONTROLLER-EVENT-" + std::to_string(static_cast<int>(event_id)), "WARNING",
                     "input controller reported an asynchronous event" };
    }
}

}  // namespace

InputNode::InputNode(std::string device_id, InputUartSession& uart_session, const std::uint8_t default_speed)
    : device_id_(std::move(device_id)), uart_session_(uart_session), default_speed_(default_speed) {
    if (!mqtt::IsValidTopicLevel(device_id_) || default_speed == 0U || default_speed > UART_INPUT_CONVEYOR_SPEED_MAX) {
        throw std::invalid_argument("input device ID or default speed is invalid");
    }
}

void InputNode::SetReportHandler(InputReportHandler handler) {
    report_handler_ = std::move(handler);
}

bool InputNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return IsControlTargetForDevice(target_device_id, device_id_);
}

InputCommandResult InputNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (!message.IsValid()) {
        std::cerr << "[input][command][ERROR] rejected invalid MQTT command; messageId=" << message.message_id << '\n';
        InputCommandResult result{};
        result.status = InputCommandStatus::kInvalidMessage;
        return result;
    }

    if (const auto* emergency = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        std::clog << "[input][command][INFO] received; messageId=" << message.message_id
                  << "; requestId=" << emergency->request_id << "; command=" << mqtt::ToString(emergency->command)
                  << "; target=" << emergency->target_device_id << '\n';
        const auto result = HandleEmergencyStop(*emergency);
        std::clog << "[input][command][INFO] completed; requestId=" << result.request_id
                  << "; command=" << mqtt::ToString(result.mqtt_command)
                  << "; status=" << (result.Succeeded() ? "success" : "failure")
                  << "; statusCode=" << static_cast<int>(result.status) << '\n';
        return result;
    }
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        std::string work_id;
        if (command->params.is_object() && command->params.contains("workId") &&
            command->params.at("workId").is_string()) {
            work_id = command->params.at("workId").get<std::string>();
        }
        std::clog << "[input][command][INFO] received; messageId=" << message.message_id
                  << "; requestId=" << command->request_id << "; command=" << mqtt::ToString(command->command)
                  << "; target=" << command->target_device_id << "; component=" << command->component_id
                  << "; workId=" << work_id << '\n';
        const auto result = HandleControlCommand(*command);
        std::clog << "[input][command][INFO] completed; requestId=" << result.request_id
                  << "; command=" << mqtt::ToString(result.mqtt_command)
                  << "; status=" << (result.Succeeded() ? "success" : "failure")
                  << "; statusCode=" << static_cast<int>(result.status) << '\n';
        return result;
    }

    std::cerr << "[input][command][ERROR] unsupported MQTT command message; messageId=" << message.message_id
              << "; messageType=" << mqtt::ToString(message.message_type) << '\n';
    InputCommandResult result{};
    result.status = InputCommandStatus::kUnsupportedMessage;
    EmitCommandResponse(result, "input node does not handle this MQTT message type");
    return result;
}

bool InputNode::RecordConveyorState(const uart_frame_t& response) noexcept {
    if (response.command != UART_CMD_RESPONSE || response.length != UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE) {
        return false;
    }
    const std::uint8_t conveyor_state = response.payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX];
    const bool changed = !last_conveyor_state_.has_value() || *last_conveyor_state_ != conveyor_state;
    last_conveyor_state_ = conveyor_state;
    return changed;
}

InputCommandResult InputNode::RequestControllerStatus() {
    InputCommandResult result{
        .mqtt_command = mqtt::ControlCommand::kUnknown,
        .uart_command = UART_CMD_INPUT_CONVEYOR_GET_STATUS,
    };
    result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_GET_STATUS, {});
    // This is the link keepalive, not a request from anyone: it runs every two
    // seconds only to prove the UART still answers. Publishing each identical
    // reply put one unchanged DEVICE_STATUS on the bus every two seconds, so
    // report the state only when it actually moves.
    if (result.Succeeded() && RecordConveyorState(result.uart_result.response_frame)) {
        EmitConveyorStatus(result.uart_result.response_frame);
    }
    return result;
}

bool InputNode::HasPendingSafetyCommand() const noexcept {
    return pending_safety_.active;
}

InputCommandResult InputNode::HandleEmergencyStop(const mqtt::EmergencyStopPayload& command) {
    InputCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .uart_command = UART_CMD_EMERGENCY_STOP,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = InputCommandStatus::kInvalidTarget;
        EmitCommandResponse(result, "emergency stop was not addressed to this device");
        return result;
    }
    if (pending_safety_.active) {
        result.status = InputCommandStatus::kRejected;
        EmitCommandResponse(result, "another safety command is waiting for controller confirmation");
        return result;
    }

    result = ExecuteAsync(std::move(result), UART_CMD_EMERGENCY_STOP, {});
    if (result.Succeeded()) {
        pending_safety_ = {
            .active = true,
            .expected = PendingSafetyEvent::kEstopLatched,
            .command = result.mqtt_command,
            .request_id = result.request_id,
        };
        EmitCommandResponse(result.request_id, result.mqtt_command, mqtt::CommandResult::kProcessing, std::nullopt,
                            "input emergency stop sent; waiting for controller confirmation");
    } else {
        EmitCommandResponse(result, "input conveyor emergency stop could not be sent");
    }
    return result;
}

InputCommandResult InputNode::HandleControlCommand(const mqtt::ControlCommandPayload& command) {
    InputCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = InputCommandStatus::kInvalidTarget;
        EmitCommandResponse(result, "control command was not addressed to this device");
        return result;
    }

    const DeviceControlAction action = ResolveDeviceControlAction(command.command, command.component_id);
    if (pending_safety_.active && action != DeviceControlAction::kStatusRequest) {
        result.status = InputCommandStatus::kRejected;
        EmitCommandResponse(result, "another safety command is waiting for controller confirmation");
        return result;
    }

    const std::array<std::uint8_t, UART_INPUT_CONVEYOR_COMMAND_PAYLOAD_SIZE> conveyor_payload{};

    switch (action) {
        case DeviceControlAction::kStart: {
            bool invalid_speed = false;
            const auto speed = ReadControlSpeed(command.params, UART_INPUT_CONVEYOR_SPEED_MAX, invalid_speed);
            if (invalid_speed) {
                result.status = InputCommandStatus::kInvalidSpeed;
                EmitCommandResponse(result, "input conveyor speed is invalid");
                return result;
            }
            const std::array<std::uint8_t, UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE> speed_payload{ speed.value_or(
                default_speed_) };
            InputCommandResult speed_result = Execute(result, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed_payload);
            if (!speed_result.Succeeded()) {
                EmitCommandResponse(speed_result, "input conveyor speed could not be set");
                return speed_result;
            }
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_START, conveyor_payload);
            EmitCommandResponse(result, "input conveyor start");
            if (result.Succeeded()) {
                EmitDeviceStatus("RUNNING");
            }
            return result;
        }
        case DeviceControlAction::kStop:
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_STOP, conveyor_payload);
            EmitCommandResponse(result, "input conveyor stop");
            if (result.Succeeded()) {
                EmitDeviceStatus("STOPPED");
            }
            return result;
        case DeviceControlAction::kStatusRequest:
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_GET_STATUS, conveyor_payload);
            if (result.Succeeded()) {
                // The server asked, so answer regardless; recording the state keeps
                // the keepalive from repeating this same report two seconds later.
                static_cast<void>(RecordConveyorState(result.uart_result.response_frame));
                EmitConveyorStatus(result.uart_result.response_frame);
            }
            EmitCommandResponse(result, "input conveyor status");
            return result;
        case DeviceControlAction::kInitialize:
            // Soft reset: clears control-level errors and stops the conveyor. The
            // controller answers this synchronously and rejects it while an
            // emergency stop is latched (use RECOVERY to clear the latch).
            result = Execute(std::move(result), UART_CMD_INPUT_CONTROL_RESET, {});
            EmitCommandResponse(result, "input controller reset");
            return result;
        case DeviceControlAction::kSafetyRecovery:
            // Full device recovery: releases the emergency-stop latch via the
            // controller's SafetyTask. Like EMERGENCY_STOP, RESET_DEVICE is
            // answered with an asynchronous EVENT/DEVICE_STATUS broadcast rather
            // than a sequence-matched reply, so it must be fire-and-forget.
            result = ExecuteAsync(std::move(result), UART_CMD_RESET_DEVICE, {});
            if (result.Succeeded()) {
                pending_safety_ = {
                    .active = true,
                    .expected = PendingSafetyEvent::kResetComplete,
                    .command = result.mqtt_command,
                    .request_id = result.request_id,
                };
                EmitCommandResponse(result.request_id, result.mqtt_command, mqtt::CommandResult::kProcessing,
                                    std::nullopt, "input recovery sent; waiting for controller confirmation");
            } else {
                EmitCommandResponse(result, "input controller recovery could not be sent");
            }
            return result;
        case DeviceControlAction::kComponentRecovery:
        case DeviceControlAction::kEmergencyStop:
        case DeviceControlAction::kUnsupported:
        default:
            result.status = InputCommandStatus::kUnsupportedCommand;
            EmitCommandResponse(result, "input node does not support this control command");
            return result;
    }
}

InputCommandResult InputNode::Execute(InputCommandResult result, std::uint8_t command,
                                      std::span<const std::uint8_t> payload) {
    result.uart_command = command;
    const bool log_command = !result.request_id.empty() || result.mqtt_command != mqtt::ControlCommand::kUnknown;
    if (log_command) {
        std::clog << "[input][uart][INFO] transmit; requestId=" << result.request_id
                  << "; mqttCommand=" << mqtt::ToString(result.mqtt_command)
                  << "; uartCommand=" << static_cast<int>(command) << '\n';
    }
    result.uart_result = uart_session_.Transact(command, payload);
    result.status = CommandStatusFromTransact(result.uart_result.status);
    if (result.status == InputCommandStatus::kRejected &&
        IsControllerExecutionFailure(result.uart_result.response_error)) {
        result.status = InputCommandStatus::kControllerFailure;
    }
    if (log_command) {
        std::clog << "[input][uart][INFO] completed; requestId=" << result.request_id
                  << "; uartCommand=" << static_cast<int>(command)
                  << "; status=" << (result.Succeeded() ? "success" : "failure")
                  << "; statusCode=" << static_cast<int>(result.status)
                  << "; sequence=" << static_cast<int>(result.uart_result.sequence)
                  << "; responseStatus=" << static_cast<int>(result.uart_result.response_status)
                  << "; responseError=" << static_cast<int>(result.uart_result.response_error)
                  << "; retries=" << static_cast<int>(result.uart_result.retries) << '\n';
    }
    return result;
}

InputCommandResult InputNode::ExecuteAsync(InputCommandResult result, std::uint8_t command,
                                           std::span<const std::uint8_t> payload) {
    result.uart_command = command;
    result.uart_result = uart_session_.SendCommand(command, payload);
    result.status = CommandStatusFromTransact(result.uart_result.status);
    return result;
}

void InputNode::EmitConveyorStatus(const uart_frame_t& response) const {
    if (response.command != UART_CMD_RESPONSE || response.length != UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t conveyor_state = response.payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX];
    const bool fault = conveyor_state == UART_INPUT_CONVEYOR_FAULT;
    EmitReport({
        .channel = InputReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = fault ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = ConveyorStateName(conveyor_state),
                .job_id = std::nullopt,
                .error_code = fault ? std::optional<std::string>{ "ERR-CONVEYOR-FAULT" } : std::nullopt,
            },
    });
}

void InputNode::EmitCommandResponse(const InputCommandResult& result, std::string message) const {
    if (result.request_id.empty() || result.mqtt_command == mqtt::ControlCommand::kUnknown) {
        return;
    }

    EmitReport({
        .channel = InputReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data =
            mqtt::CommandResponsePayload{
                .request_id = result.request_id,
                .command = result.mqtt_command,
                .result = CommandResultForStatus(result.status),
                .error_code = ErrorCodeForResult(result),
                .message = std::move(message),
            },
    });
}

void InputNode::EmitCommandResponse(std::string request_id, mqtt::ControlCommand command, mqtt::CommandResult result,
                                    std::optional<std::string> error_code, std::string message) const {
    if (request_id.empty() || command == mqtt::ControlCommand::kUnknown) {
        return;
    }
    EmitReport({
        .channel = InputReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data = mqtt::CommandResponsePayload{ .request_id = std::move(request_id),
                                              .command = command,
                                              .result = result,
                                              .error_code = std::move(error_code),
                                              .message = std::move(message) },
    });
}

void InputNode::EmitDeviceStatus(std::string current_state, std::optional<std::string> error_code) const {
    EmitReport({
        .channel = InputReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data = mqtt::DeviceStatusPayload{ .status = error_code.has_value() ? mqtt::ConnectionState::kUartError
                                                                            : mqtt::ConnectionState::kOnline,
                                           .current_state = std::move(current_state),
                                           .job_id = std::nullopt,
                                           .error_code = std::move(error_code) },
    });
}

void InputNode::ResetControllerHeartbeatMonitor() noexcept {
    controller_heartbeat_elapsed_ = std::chrono::milliseconds::zero();
    controller_heartbeat_timed_out_ = false;
    last_heartbeat_state_.reset();
}

void InputNode::Tick(std::chrono::milliseconds elapsed) {
    if (elapsed <= std::chrono::milliseconds::zero()) {
        return;
    }
    if (pending_safety_.active) {
        pending_safety_.elapsed += elapsed;
        if (pending_safety_.elapsed >= mqtt::kEmergencyStopConfirmationTimeout) {
            EmitCommandResponse(pending_safety_.request_id, pending_safety_.command, mqtt::CommandResult::kTimeout,
                                std::string("ERR-SAFETY-CONFIRMATION-TIMEOUT"),
                                "input controller did not confirm the safety command");
            pending_safety_ = {};
        }
    }
    if (!controller_heartbeat_timed_out_) {
        controller_heartbeat_elapsed_ += elapsed;
        if (controller_heartbeat_elapsed_ >= kControllerHeartbeatTimeout) {
            controller_heartbeat_timed_out_ = true;
            EmitDeviceStatus("UART_HEARTBEAT_TIMEOUT", std::string("ERR-UART-HEARTBEAT-TIMEOUT"));
            EmitReport({
                .channel = InputReportChannel::kError,
                .message_type = mqtt::MessageType::kErrorOccurred,
                .data = mqtt::ErrorOccurredPayload{ .job_id = std::nullopt,
                                                    .error_code = "ERR-UART-HEARTBEAT-TIMEOUT",
                                                    .error_level = "ERROR",
                                                    .current_state = "UART_HEARTBEAT_TIMEOUT",
                                                    .message = "input controller heartbeat timed out",
                                                    .distance = std::nullopt },
            });
        }
    }
}

void InputNode::HandleUartFrame(const uart_frame_t& frame) {
    switch (frame.command) {
        case UART_CMD_SENSOR_STATUS:
            HandleSensorStatus(frame);
            return;
        case UART_CMD_DEVICE_STATUS:
            HandleDeviceStatus(frame);
            return;
        case UART_CMD_EVENT:
            HandleControllerEvent(frame);
            return;
        default:
            return;
    }
}

void InputNode::HandleSensorStatus(const uart_frame_t& frame) {
    if (frame.length != UART_SENSOR_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t sensor_id = frame.payload[UART_SENSOR_ID_INDEX];
    const std::uint8_t sensor_state = frame.payload[UART_SENSOR_STATE_INDEX];
    const std::uint16_t distance_cm =
        static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_LOW_INDEX]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_HIGH_INDEX]) << 8U);

    // Sensor readings are telemetry, not device state: publish every measurement as
    // a SENSOR_STATUS event so the server gets the sensor id and distance, and so
    // sensor activity no longer overwrites the device's operational current_state.
    EmitReport({
        .channel = InputReportChannel::kEvent,
        .message_type = mqtt::MessageType::kSensorStatus,
        .data =
            mqtt::SensorStatusPayload{
                .sensor_id = static_cast<std::int32_t>(sensor_id),
                .measurement_status = MeasurementStatusName(sensor_state),
                .distance_cm = static_cast<std::int32_t>(distance_cm),
            },
    });

    if (last_sensor_state_.has_value() && *last_sensor_state_ == sensor_state) {
        return;  // only a transition raises the fault alert below
    }
    last_sensor_state_ = sensor_state;

    if (sensor_state == UART_SENSOR_FAULT) {
        EmitReport({
            .channel = InputReportChannel::kError,
            .message_type = mqtt::MessageType::kErrorOccurred,
            .data =
                mqtt::ErrorOccurredPayload{
                    .job_id = std::nullopt,
                    .error_code = "ERR-SENSOR",
                    .error_level = "ERROR",
                    .current_state = "SENSOR_" + std::to_string(sensor_id) + "_FAULT",
                    .message = "input ultrasonic sensor reported a fault",
                    .distance = SensorDistanceCm(frame),
                },
        });
    }
}

void InputNode::HandleDeviceStatus(const uart_frame_t& frame) {
    if (frame.length != UART_DEVICE_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t device_state = frame.payload[UART_DEVICE_STATUS_STATE_INDEX];
    const std::uint8_t error_code = frame.payload[UART_DEVICE_STATUS_ERROR_INDEX];
    const bool emergency_stopped = device_state == UART_DEVICE_EMERGENCY_STOP;
    const bool has_error = error_code != UART_ERROR_NONE && !emergency_stopped;

    EmitReport({
        .channel = InputReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = has_error ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = DeviceStateName(device_state),
                .job_id = std::nullopt,
                .error_code = has_error ? std::optional<std::string>{ UartErrorCode(error_code) } : std::nullopt,
            },
    });
}

void InputNode::HandleControllerHeartbeat(const uart_frame_t& frame) {
    // The controller's periodic heartbeat carries the authoritative device state,
    // active error and input sensor state, so decode it rather than dropping it.
    // It arrives about once a second; report only on change to avoid flooding.
    if (UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint8_t device_state = frame.payload[APP_HEARTBEAT_STATE_INDEX];
    const std::uint8_t error_code = frame.payload[APP_HEARTBEAT_ERROR_INDEX];
    const std::uint8_t sensor_state = frame.payload[APP_HEARTBEAT_INPUT_SENSOR_INDEX];
    const bool recovered = controller_heartbeat_timed_out_;
    controller_heartbeat_elapsed_ = std::chrono::milliseconds::zero();
    controller_heartbeat_timed_out_ = false;

    // The sensor state travels in every heartbeat too, but UART_CMD_SENSOR_STATUS
    // (HandleSensorStatus) already reports its transitions with the correct
    // SENSOR_CLEAR/OBJECT_DETECTED naming. Track it here to keep state consistent
    // (e.g. for the cross-path update in HandleControllerEvent) but do not let it
    // gate this report on its own, or every sensor transition would also emit a
    // misleading device_state-only report ("READY") right alongside the correct
    // sensor report.
    const bool device_state_changed = recovered || !last_heartbeat_state_.has_value() ||
                                      last_heartbeat_state_->device_state != device_state ||
                                      last_heartbeat_state_->error_code != error_code;
    last_heartbeat_state_ = HeartbeatState{ device_state, error_code, sensor_state };
    if (!device_state_changed) {
        return;
    }

    const bool emergency_stopped = device_state == UART_DEVICE_EMERGENCY_STOP;
    const bool has_error = error_code != UART_ERROR_NONE && !emergency_stopped;
    EmitReport({
        .channel = InputReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = has_error ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = DeviceStateName(device_state),
                .job_id = std::nullopt,
                .error_code = has_error ? std::optional<std::string>{ UartErrorCode(error_code) } : std::nullopt,
            },
    });
}

void InputNode::HandleControllerEvent(const uart_frame_t& frame) {
    if (frame.length < UART_EVENT_HEADER_SIZE) {
        return;
    }
    const std::uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];

    if (event_id == APP_EVENT_HEARTBEAT) {
        HandleControllerHeartbeat(frame);
        return;
    }

    if ((event_id == APP_EVENT_SAFETY && UART_IS_VALID_APP_SAFETY_PAYLOAD(frame.payload, frame.length) == 0U) ||
        (event_id == APP_EVENT_HEALTH && UART_IS_VALID_APP_HEALTH_PAYLOAD(frame.payload, frame.length) == 0U)) {
        return;
    }
    const bool has_detail = frame.length > APP_SAFETY_EVENT_CAUSE_INDEX;
    const std::uint8_t kind = has_detail ? frame.payload[APP_SAFETY_EVENT_KIND_INDEX] : 0U;
    const std::uint8_t cause = has_detail ? frame.payload[APP_SAFETY_EVENT_CAUSE_INDEX] : 0U;

    // A timed-out UART cannot receive its own health event, so the STM32 sends
    // this event over the opposite, still healthy channel. The central server
    // already determines the affected Pi's availability from MQTT heartbeats.
    if (event_id == APP_EVENT_HEALTH && kind == HEALTH_ISSUE_UART_CHANNEL_TIMEOUT) {
        return;
    }

    if ((event_id == APP_EVENT_SAFETY) && pending_safety_.active) {
        const bool estopConfirmed =
            (pending_safety_.expected == PendingSafetyEvent::kEstopLatched) && (kind == SAFETY_EVENT_ESTOP_LATCHED);
        const bool resetConfirmed =
            (pending_safety_.expected == PendingSafetyEvent::kResetComplete) && (kind == SAFETY_EVENT_RESET_COMPLETE);
        const bool resetRejected =
            (pending_safety_.expected == PendingSafetyEvent::kResetComplete) && (kind == SAFETY_EVENT_RESET_REJECTED);
        if (estopConfirmed || resetConfirmed || resetRejected) {
            const std::uint8_t resetResult =
                (frame.length > APP_SAFETY_EVENT_RESULT_INDEX) ? frame.payload[APP_SAFETY_EVENT_RESULT_INDEX] : 0U;
            EmitCommandResponse(
                pending_safety_.request_id, pending_safety_.command,
                resetRejected ? mqtt::CommandResult::kRejected : mqtt::CommandResult::kSuccess,
                resetRejected ? std::optional<std::string>{ "ERR-SAFETY-RESET-REJECTED" } : std::nullopt,
                resetRejected ? "input controller rejected safety recovery result " + std::to_string(resetResult)
                              : "input controller confirmed the safety command");
            pending_safety_ = {};
        }
    }

    // sensor_id is only meaningful for a HEALTH/SENSOR_STALE event; every other event
    // (including all SAFETY events) has a timestamp byte at that same offset instead.
    const bool is_health_sensor_stale = event_id == APP_EVENT_HEALTH && kind == HEALTH_ISSUE_SENSOR_STALE;
    const std::optional<std::uint8_t> sensor_id =
        (is_health_sensor_stale && frame.length > APP_HEALTH_EVENT_SENSOR_ID_INDEX)
            ? std::optional<std::uint8_t>{ frame.payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] }
            : std::nullopt;
    const std::uint8_t sensor_id_for_signature = sensor_id.value_or(HEALTH_ISSUE_SENSOR_ID_NONE);

    // Report only when the (event, kind, cause, sensorId) changes. The controller
    // re-latches and re-emits some health conditions (e.g. an oscillating
    // sensor-stale) every few seconds; without this, each re-emission would flood
    // device/{id}/error. sensorId is folded in so a different stale sensor on the
    // same (kind, cause) is never mistaken for a repeat of the previous one.
    const std::uint32_t signature =
        (static_cast<std::uint32_t>(event_id) << 24U) | (static_cast<std::uint32_t>(kind) << 16U) |
        (static_cast<std::uint32_t>(cause) << 8U) | static_cast<std::uint32_t>(sensor_id_for_signature);
    if (last_controller_event_signature_.has_value() && *last_controller_event_signature_ == signature) {
        return;
    }
    last_controller_event_signature_ = signature;

    if (event_id == APP_EVENT_SAFETY && kind == SAFETY_EVENT_ESTOP_LATCHED) {
        EmitReport({
            .channel = InputReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = DeviceStateName(UART_DEVICE_EMERGENCY_STOP),
                    .job_id = std::nullopt,
                    .error_code = std::nullopt,
                },
        });
        if (last_heartbeat_state_.has_value()) {
            last_heartbeat_state_->device_state = UART_DEVICE_EMERGENCY_STOP;
            last_heartbeat_state_->error_code = UART_ERROR_NONE;
        } else {
            last_heartbeat_state_ = HeartbeatState{ UART_DEVICE_EMERGENCY_STOP, UART_ERROR_NONE, UART_SENSOR_OK };
        }
        return;
    }

    if (event_id == APP_EVENT_SAFETY && kind == SAFETY_EVENT_RESET_COMPLETE) {
        // Recovery releases the safety latch but does not start the process.
        // Report STOPPED so an explicit START is still required.
        EmitReport({
            .channel = InputReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = DeviceStateName(UART_DEVICE_STOPPED),
                    .job_id = std::nullopt,
                    .error_code = std::nullopt,
                },
        });
        // The next ~1 Hz heartbeat will carry this same READY/NONE state; record it
        // now so that heartbeat's own change detection does not re-report it a
        // second later. The sensor field is left as whatever heartbeat last saw (or
        // OK if none yet) since this event carries no sensor information.
        if (last_heartbeat_state_.has_value()) {
            last_heartbeat_state_->device_state = UART_DEVICE_STOPPED;
            last_heartbeat_state_->error_code = UART_ERROR_NONE;
        } else {
            last_heartbeat_state_ = HeartbeatState{ UART_DEVICE_STOPPED, UART_ERROR_NONE, UART_SENSOR_OK };
        }
        return;
    }

    const ControllerEventDescription description = DescribeControllerEvent(event_id, kind, cause, sensor_id);
    EmitReport({
        .channel = InputReportChannel::kError,
        .message_type = mqtt::MessageType::kErrorOccurred,
        .data =
            mqtt::ErrorOccurredPayload{
                .job_id = std::nullopt,
                .error_code = description.error_code,
                .error_level = description.error_level,
                .current_state = "CONTROLLER_EVENT",
                .message = description.message,
                .distance = std::nullopt,
            },
    });
}

void InputNode::EmitReport(InputReport report) const {
    if (!report_handler_) {
        return;
    }
    try {
        report_handler_(report);
    } catch (...) {
        // A reporting failure must not unwind into the caller.
    }
}

}  // namespace logistics::device
