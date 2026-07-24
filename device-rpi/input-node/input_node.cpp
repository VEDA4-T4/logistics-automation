#include "logistics/device/input_node.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

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
        case InputCommandStatus::kInvalidTarget:
            return mqtt::CommandResult::kRejected;
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
        case UART_ERROR_EMERGENCY_STOP:
            return "ERR-EMERGENCY-STOP";
        case UART_ERROR_INVALID_PAYLOAD:
            return "ERR-UART-INVALID-PAYLOAD";
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
        case InputCommandStatus::kTimeout:
            return std::string("ERR-UART-ACK-TIMEOUT");
        case InputCommandStatus::kUartNotOpen:
            return std::string("ERR-UART-DISCONNECTED");
        case InputCommandStatus::kUartError:
            return std::string("ERR-UART-IO");
        case InputCommandStatus::kInvalidTarget:
            return std::string("ERR-MQTT-INVALID-TARGET");
        case InputCommandStatus::kUnsupportedMessage:
        case InputCommandStatus::kUnsupportedCommand:
            return std::string("ERR-UNSUPPORTED-COMMAND");
        case InputCommandStatus::kInvalidMessage:
            return std::string("ERR-MQTT-INVALID-MESSAGE");
    }
    return std::string("ERR-INTERNAL");
}

[[nodiscard]] std::optional<std::uint8_t> SpeedFromParams(const mqtt::Json& params) {
    const auto iterator = params.find("speed");
    if (iterator == params.end() || !iterator->is_number_integer()) {
        return std::nullopt;
    }
    const auto value = iterator->get<std::int64_t>();
    if (value < UART_INPUT_CONVEYOR_SPEED_MIN || value > UART_INPUT_CONVEYOR_SPEED_MAX) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::string SensorStateName(std::uint8_t state) {
    switch (state) {
        case UART_SENSOR_CLEAR:
            return "SENSOR_CLEAR";
        case UART_SENSOR_DETECTED:
            return "OBJECT_DETECTED";
        case UART_SENSOR_FAULT:
            return "SENSOR_FAULT";
        default:
            return "SENSOR_UNKNOWN";
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
    const std::uint16_t distance = static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_LOW_INDEX]) |
                                   static_cast<std::uint16_t>(
                                       static_cast<std::uint16_t>(frame.payload[UART_SENSOR_DISTANCE_HIGH_INDEX])
                                       << 8U);
    if (distance == UART_SENSOR_DISTANCE_UNKNOWN) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(distance);
}

}  // namespace

InputNode::InputNode(std::string device_id, InputUartSession& uart_session)
    : device_id_(std::move(device_id)), uart_session_(uart_session) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("input device ID must be one non-wildcard MQTT topic level");
    }
}

void InputNode::SetReportHandler(InputReportHandler handler) {
    report_handler_ = std::move(handler);
}

bool InputNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return target_device_id == device_id_ || target_device_id == "ALL";
}

InputCommandResult InputNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (!message.IsValid()) {
        InputCommandResult result{};
        result.status = InputCommandStatus::kInvalidMessage;
        return result;
    }

    if (const auto* emergency = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        return HandleEmergencyStop(*emergency);
    }
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return HandleControlCommand(*command);
    }

    InputCommandResult result{};
    result.status = InputCommandStatus::kUnsupportedMessage;
    EmitCommandResponse(result, "input node does not handle this MQTT message type");
    return result;
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

    result = Execute(std::move(result), UART_CMD_EMERGENCY_STOP, {});
    EmitCommandResponse(result, "input conveyor emergency stop");
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

    const std::array<std::uint8_t, UART_INPUT_CONVEYOR_COMMAND_PAYLOAD_SIZE> conveyor_payload{};

    switch (command.command) {
        case mqtt::ControlCommand::kStart: {
            if (const auto speed = SpeedFromParams(command.params); speed.has_value()) {
                const std::array<std::uint8_t, UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE> speed_payload{ *speed };
                InputCommandResult speed_result = Execute(result, UART_CMD_INPUT_CONVEYOR_SET_SPEED, speed_payload);
                if (!speed_result.Succeeded()) {
                    EmitCommandResponse(speed_result, "input conveyor speed could not be set");
                    return speed_result;
                }
            }
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_START, conveyor_payload);
            EmitCommandResponse(result, "input conveyor start");
            return result;
        }
        case mqtt::ControlCommand::kStop:
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_STOP, conveyor_payload);
            EmitCommandResponse(result, "input conveyor stop");
            return result;
        case mqtt::ControlCommand::kStatusRequest:
            result = Execute(std::move(result), UART_CMD_INPUT_CONVEYOR_GET_STATUS, conveyor_payload);
            if (result.Succeeded()) {
                EmitConveyorStatus(result.uart_result.response_frame);
            }
            EmitCommandResponse(result, "input conveyor status");
            return result;
        case mqtt::ControlCommand::kInitialize:
        case mqtt::ControlCommand::kRecovery:
            result = Execute(std::move(result), UART_CMD_INPUT_CONTROL_RESET, {});
            EmitCommandResponse(result, "input controller reset");
            return result;
        case mqtt::ControlCommand::kRestart:
        case mqtt::ControlCommand::kDestinationSet:
        case mqtt::ControlCommand::kEmergencyStop:
        case mqtt::ControlCommand::kUnknown:
        default:
            result.status = InputCommandStatus::kUnsupportedCommand;
            EmitCommandResponse(result, "input node does not support this control command");
            return result;
    }
}

InputCommandResult InputNode::Execute(InputCommandResult result, std::uint8_t command,
                                      std::span<const std::uint8_t> payload) {
    result.uart_command = command;
    result.uart_result = uart_session_.Transact(command, payload);
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
    const std::uint8_t sensor_state = frame.payload[UART_SENSOR_STATE_INDEX];
    if (last_sensor_state_.has_value() && *last_sensor_state_ == sensor_state) {
        return;  // report only on change to avoid flooding the broker
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
                    .current_state = "SENSOR_FAULT",
                    .message = "input ultrasonic sensor reported a fault",
                    .distance = SensorDistanceCm(frame),
                },
        });
        return;
    }

    EmitReport({
        .channel = InputReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = SensorStateName(sensor_state),
                .job_id = std::nullopt,
                .error_code = std::nullopt,
            },
    });
}

void InputNode::HandleDeviceStatus(const uart_frame_t& frame) {
    if (frame.length != UART_DEVICE_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t device_state = frame.payload[UART_DEVICE_STATUS_STATE_INDEX];
    const std::uint8_t error_code = frame.payload[UART_DEVICE_STATUS_ERROR_INDEX];
    const bool has_error = error_code != UART_ERROR_NONE;

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

    // APP_EVENT_HEARTBEAT (stm32/conveyor-controller/Application/Inc/app_comm_tx.h) is a
    // periodic CommTxTask liveness signal, not an operator-facing condition; there is no
    // shared UART contract for these app-level event ids yet, so the value is duplicated here.
    constexpr std::uint8_t kAppEventHeartbeat = 0x01U;
    if (event_id == kAppEventHeartbeat) {
        return;
    }

    // The controller safety/health EVENT payload layout is not yet shared with
    // the server team, so surface it as a generic operator-visible error rather
    // than dropping it. The event id is reported so it can be correlated later.
    EmitReport({
        .channel = InputReportChannel::kError,
        .message_type = mqtt::MessageType::kErrorOccurred,
        .data =
            mqtt::ErrorOccurredPayload{
                .job_id = std::nullopt,
                .error_code = "ERR-CONTROLLER-EVENT-" + std::to_string(static_cast<int>(event_id)),
                .error_level = "WARNING",
                .current_state = "CONTROLLER_EVENT",
                .message = "input controller reported an asynchronous event",
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
