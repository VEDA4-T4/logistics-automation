#include "logistics/device/linetracer_node.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/device/device_control_policy.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] LineTracerCommandStatus ToCommandStatus(UartSessionSendStatus status) noexcept {
    switch (status) {
        case UartSessionSendStatus::kSent:
            return LineTracerCommandStatus::kSent;
        case UartSessionSendStatus::kNotOpen:
            return LineTracerCommandStatus::kUartNotOpen;
        case UartSessionSendStatus::kBusy:
            return LineTracerCommandStatus::kUartBusy;
        case UartSessionSendStatus::kInvalidArgument:
        case UartSessionSendStatus::kEncodeError:
        case UartSessionSendStatus::kTransportError:
            return LineTracerCommandStatus::kUartError;
    }
    return LineTracerCommandStatus::kUartError;
}

[[nodiscard]] std::string NormalizeDestination(std::string_view destination) {
    std::string normalized;
    normalized.reserve(destination.size());
    for (const unsigned char character : destination) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return normalized;
}

[[nodiscard]] char RouteName(std::uint8_t route_id) noexcept {
    switch (route_id) {
        case UART_LINETRACER_ROUTE_A:
            return 'A';
        case UART_LINETRACER_ROUTE_B:
            return 'B';
        case UART_LINETRACER_ROUTE_C:
            return 'C';
        default:
            return '?';
    }
}

[[nodiscard]] std::string RouteState(std::string_view state, std::uint8_t route_id) {
    std::string result(state);
    result.push_back('_');
    result.push_back(RouteName(route_id));
    return result;
}

[[nodiscard]] std::string UartStateName(std::uint8_t state, std::uint8_t route_id) {
    switch (state) {
        case UART_LINETRACER_STATE_IDLE:
            return "IDLE";
        case UART_LINETRACER_STATE_LOAD_WAIT:
            return RouteState("PICKUP_READY", route_id);
        case UART_LINETRACER_STATE_FOLLOWING_LINE:
            return "FOLLOWING_LINE";
        case UART_LINETRACER_STATE_CORRECTING:
            return "CORRECTING";
        case UART_LINETRACER_STATE_ARRIVED:
            return RouteState("ARRIVED", route_id);
        case UART_LINETRACER_STATE_UNLOADING:
            return RouteState("UNLOADING", route_id);
        case UART_LINETRACER_STATE_STOPPED:
            return "STOPPED";
        case UART_LINETRACER_STATE_FAULT:
            return "FAULT";
        case UART_LINETRACER_STATE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] std::string UartErrorCode(std::uint8_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
            return "ERR-UART-TIMEOUT";
        case UART_ERROR_SENSOR:
            return "ERR-SENSOR";
        case UART_ERROR_MOTOR:
            return "ERR-MOTOR";
        case UART_ERROR_SERVO:
            return "ERR-SERVO";
        case UART_ERROR_EMERGENCY_STOP:
            return "ERR-EMERGENCY-STOP";
        case UART_ERROR_INTERNAL:
            return "ERR-INTERNAL";
        default:
            return "ERR-UART-UNKNOWN";
    }
}

[[nodiscard]] mqtt::ConnectionState StateConnection(std::uint8_t state) noexcept {
    if (state == UART_LINETRACER_STATE_FAULT || state == UART_LINETRACER_STATE_EMERGENCY_STOP) {
        return mqtt::ConnectionState::kUartError;
    }
    return mqtt::ConnectionState::kOnline;
}

}  // namespace

bool LineTracerCommandResult::Succeeded() const noexcept {
    return status == LineTracerCommandStatus::kSent || status == LineTracerCommandStatus::kSentNoReply;
}

LineTracerNode::LineTracerNode(std::string device_id, UartSession& uart_session)
    : device_id_(std::move(device_id)), uart_session_(uart_session) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("line tracer device ID must be one non-wildcard MQTT topic level");
    }
}

void LineTracerNode::SetReportHandler(LineTracerReportHandler handler) {
    report_handler_ = std::move(handler);
}

LineTracerCommandResult LineTracerNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (!message.IsValid()) {
        return { .status = LineTracerCommandStatus::kInvalidMessage, .request_id = {}, .work_id = {} };
    }

    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return HandleDestinationSet(*destination);
    }
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return HandleControlCommand(*command);
    }
    if (const auto* command = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        return HandleEmergencyStop(*command);
    }
    return { .status = LineTracerCommandStatus::kUnsupportedMessage, .request_id = {}, .work_id = {} };
}

void LineTracerNode::HandleUartEvent(const UartSessionEvent& event) noexcept {
    if (event.type == UartSessionEventType::kFrameReceived) {
        HandleLineTracerFrame(event.frame);
        return;
    }

    if (event.type == UartSessionEventType::kTransportDisconnected ||
        event.type == UartSessionEventType::kTransportError) {
        if (pending_.active) {
            EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-DISCONNECTED"),
                                "UART transport disconnected before acknowledgement");
            ClearPending();
        }
        if (pending_safety_.active) {
            EmitSafetyResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-DISCONNECTED"),
                               "UART transport disconnected before emergency stop confirmation");
            pending_safety_ = {};
        }
        return;
    }

    if (!pending_.active || event.pending_sequence != pending_.sequence) {
        return;
    }

    if (event.type == UartSessionEventType::kAckReceived) {
        const std::uint8_t status = event.frame.payload[UART_ACK_STATUS_INDEX];
        const bool accepted = status == UART_STATUS_ACK || status == UART_STATUS_SUCCESS;
        if (accepted && pending_.effect == PendingEffect::kActivateJob) {
            active_work_id_ = pending_.work_id;
            active_uart_job_id_ = pending_.uart_job_id;
            active_route_id_ = pending_.route_id;
        } else if (accepted && pending_.effect == PendingEffect::kClearJob) {
            active_work_id_.clear();
            active_uart_job_id_ = UART_LINETRACER_JOB_ID_NONE;
            active_route_id_ = UART_LINETRACER_ROUTE_NONE;
        }
        if (accepted) {
            EmitPendingResponse(mqtt::CommandResult::kSuccess, std::nullopt,
                                "line tracer command acknowledged by controller");
        } else if (status == UART_STATUS_BUSY) {
            EmitPendingResponse(mqtt::CommandResult::kRejected, std::string("ERR-UART-BUSY"),
                                "line tracer controller is busy");
        } else if (status == UART_STATUS_NACK) {
            EmitPendingResponse(mqtt::CommandResult::kRejected, std::string("ERR-UART-NACK"),
                                "line tracer controller rejected the command");
        } else {
            EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-CONTROLLER"),
                                "line tracer controller reported a command error");
        }
        ClearPending();
        return;
    }

    if (event.type == UartSessionEventType::kAckTimeout) {
        EmitPendingResponse(mqtt::CommandResult::kTimeout, std::string("ERR-UART-ACK-TIMEOUT"),
                            "line tracer controller acknowledgement timed out");
        ClearPending();
        return;
    }
}

void LineTracerNode::Tick(const std::chrono::milliseconds elapsed) noexcept {
    if (!pending_safety_.active || elapsed <= std::chrono::milliseconds::zero()) {
        return;
    }
    pending_safety_.elapsed += elapsed;
    if (pending_safety_.elapsed >= mqtt::kEmergencyStopConfirmationTimeout) {
        EmitSafetyResponse(mqtt::CommandResult::kTimeout, std::string("ERR-SAFETY-CONFIRMATION-TIMEOUT"),
                           "line tracer controller did not confirm the emergency stop");
        pending_safety_ = {};
    }
}

bool LineTracerNode::HasActiveJob() const noexcept {
    return active_uart_job_id_ != UART_LINETRACER_JOB_ID_NONE;
}

std::string_view LineTracerNode::ActiveWorkId() const noexcept {
    return active_work_id_;
}

std::uint16_t LineTracerNode::ActiveUartJobId() const noexcept {
    return active_uart_job_id_;
}

std::uint8_t LineTracerNode::ActiveRouteId() const noexcept {
    return active_route_id_;
}

std::optional<std::uint8_t> LineTracerNode::RouteFromDestination(std::string_view destination) {
    const std::string normalized = NormalizeDestination(destination);
    if (normalized == "A" || normalized == "1" || normalized == "DESTA" || normalized == "DEST01" ||
        normalized == "ROUTEA" || normalized == "ROUTE1") {
        return UART_LINETRACER_ROUTE_A;
    }
    if (normalized == "B" || normalized == "2" || normalized == "DESTB" || normalized == "DEST02" ||
        normalized == "ROUTEB" || normalized == "ROUTE2") {
        return UART_LINETRACER_ROUTE_B;
    }
    if (normalized == "C" || normalized == "3" || normalized == "DESTC" || normalized == "DEST03" ||
        normalized == "ROUTEC" || normalized == "ROUTE3") {
        return UART_LINETRACER_ROUTE_C;
    }
    return std::nullopt;
}

LineTracerCommandResult LineTracerNode::HandleDestinationSet(const mqtt::DestinationSetPayload& command) {
    LineTracerCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = command.work_id,
        .uart_command = UART_CMD_LINETRACER_ASSIGN_ROUTE,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = LineTracerCommandStatus::kInvalidTarget;
        return result;
    }

    const std::optional<std::uint8_t> route_id = RouteFromDestination(command.destination);
    if (!route_id.has_value()) {
        result.status = LineTracerCommandStatus::kInvalidDestination;
        return result;
    }

    const bool reuses_active_job = HasActiveJob() && active_work_id_ == command.work_id;
    const std::uint16_t uart_job_id = reuses_active_job ? active_uart_job_id_ : AllocateJobId();
    const std::array<std::uint8_t, UART_LINETRACER_START_PAYLOAD_SIZE> payload{
        static_cast<std::uint8_t>(uart_job_id & 0xffU), static_cast<std::uint8_t>((uart_job_id >> 8U) & 0xffU),
        *route_id
    };

    result.uart_job_id = uart_job_id;
    result.uart_route_id = *route_id;
    result = Send(std::move(result), UART_CMD_LINETRACER_ASSIGN_ROUTE, payload.data(), payload.size());
    if (result.Succeeded()) {
        RememberPending(PendingEffect::kActivateJob, result);
    }
    return result;
}

LineTracerCommandResult LineTracerNode::HandleControlCommand(const mqtt::ControlCommandPayload& command) {
    LineTracerCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = active_work_id_,
        .uart_job_id = active_uart_job_id_,
        .uart_route_id = active_route_id_,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = LineTracerCommandStatus::kInvalidTarget;
        return result;
    }

    std::uint8_t uart_command = UART_CMD_NONE;
    PendingEffect effect = PendingEffect::kNone;
    std::array<std::uint8_t, UART_LINETRACER_STOP_PAYLOAD_SIZE> payload{};
    std::size_t payload_length = 0U;

    switch (ResolveDeviceControlAction(command.command, command.component_id)) {
        case DeviceControlAction::kStop:
            if (!HasActiveJob()) {
                result.status = LineTracerCommandStatus::kNoActiveJob;
                return result;
            }
            uart_command = UART_CMD_LINETRACER_STOP_DRIVE;
            payload[0] = static_cast<std::uint8_t>(active_uart_job_id_ & 0xffU);
            payload[1] = static_cast<std::uint8_t>((active_uart_job_id_ >> 8U) & 0xffU);
            payload_length = UART_LINETRACER_STOP_PAYLOAD_SIZE;
            break;
        case DeviceControlAction::kStart:
            if (!HasActiveJob()) {
                result.status = LineTracerCommandStatus::kNoActiveJob;
                return result;
            }
            uart_command = UART_CMD_LINETRACER_RESUME_DRIVE;
            break;
        case DeviceControlAction::kInitialize:
            uart_command = UART_CMD_LINETRACER_RESET_SYSTEM;
            effect = PendingEffect::kClearJob;
            break;
        case DeviceControlAction::kSafetyRecovery:
            uart_command = UART_CMD_RESET_DEVICE;
            effect = PendingEffect::kClearJob;
            break;
        case DeviceControlAction::kStatusRequest:
        case DeviceControlAction::kComponentRecovery:
        case DeviceControlAction::kEmergencyStop:
        case DeviceControlAction::kUnsupported:
            result.status = LineTracerCommandStatus::kUnsupportedCommand;
            return result;
    }

    result.uart_command = uart_command;
    result = Send(std::move(result), uart_command, payload.data(), payload_length);
    if (result.Succeeded()) {
        RememberPending(effect, result);
    }
    return result;
}

LineTracerCommandResult LineTracerNode::HandleEmergencyStop(const mqtt::EmergencyStopPayload& command) {
    LineTracerCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = active_work_id_,
        .uart_job_id = active_uart_job_id_,
        .uart_route_id = active_route_id_,
        .uart_command = UART_CMD_EMERGENCY_STOP,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = LineTracerCommandStatus::kInvalidTarget;
        return result;
    }
    if (pending_.active) {
        EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-EMERGENCY-STOP"),
                            "line tracer command was preempted by an emergency stop");
        ClearPending();
    }
    static_cast<void>(uart_session_.CancelPendingCommand());
    result = SendOneWay(std::move(result), UART_CMD_EMERGENCY_STOP);
    if (result.Succeeded()) {
        pending_safety_ = {
            .active = true,
            .command = result.mqtt_command,
            .request_id = result.request_id,
        };
    }
    return result;
}

LineTracerCommandResult LineTracerNode::Send(LineTracerCommandResult result, std::uint8_t command,
                                             const std::uint8_t* payload, std::size_t payload_length) {
    const std::span<const std::uint8_t> payload_view =
        payload == nullptr ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>{ payload, payload_length };
    result.uart_result = uart_session_.SendCommand(command, payload_view);
    result.uart_sequence = result.uart_result.sequence;
    result.status = ToCommandStatus(result.uart_result.status);
    return result;
}

LineTracerCommandResult LineTracerNode::SendOneWay(LineTracerCommandResult result, const std::uint8_t command) {
    result.uart_result = uart_session_.SendOneWayCommand(command);
    result.uart_sequence = result.uart_result.sequence;
    result.status = result.uart_result.Succeeded() ? LineTracerCommandStatus::kSentNoReply
                                                   : ToCommandStatus(result.uart_result.status);
    return result;
}

bool LineTracerNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return IsControlTargetForDevice(target_device_id, device_id_);
}

std::uint16_t LineTracerNode::AllocateJobId() noexcept {
    std::uint16_t allocated = UART_LINETRACER_JOB_ID_NONE;
    do {
        allocated = next_uart_job_id_;
        ++next_uart_job_id_;
        if (next_uart_job_id_ == UART_LINETRACER_JOB_ID_NONE) {
            next_uart_job_id_ = UART_LINETRACER_JOB_ID_MIN;
        }
    } while (HasActiveJob() && allocated == active_uart_job_id_);
    return allocated;
}

void LineTracerNode::RememberPending(PendingEffect effect, const LineTracerCommandResult& result) {
    pending_ = {
        .active = true,
        .effect = effect,
        .sequence = result.uart_sequence,
        .mqtt_command = result.mqtt_command,
        .request_id = result.request_id,
        .work_id = result.work_id,
        .uart_job_id = result.uart_job_id,
        .route_id = result.uart_route_id,
    };
}

void LineTracerNode::ClearPending() noexcept {
    pending_ = {};
}

void LineTracerNode::HandleLineTracerFrame(const uart_frame_t& frame) noexcept {
    if (frame.command != UART_CMD_EVENT || UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }

    const std::uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];
    const std::uint16_t job_id = uart_linetracer_event_job_id(frame.payload);
    const std::uint8_t route_id = frame.payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX];
    if (event_id == UART_LINETRACER_EVENT_FAULT &&
        frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX] == UART_ERROR_EMERGENCY_STOP) {
        last_uart_state_ = UART_LINETRACER_STATE_EMERGENCY_STOP;
        if (pending_safety_.active) {
            EmitSafetyResponse(mqtt::CommandResult::kSuccess, std::nullopt,
                               "line tracer controller confirmed the emergency stop");
            pending_safety_ = {};
        }
        const auto active_job =
            HasActiveJob() ? std::optional<std::string>{ active_work_id_ } : std::optional<std::string>{};
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kUartError,
                    .current_state = "EMERGENCY_STOP",
                    .job_id = active_job,
                    .error_code = std::string("ERR-EMERGENCY-STOP"),
                },
        });
        EmitReport({
            .channel = LineTracerReportChannel::kError,
            .message_type = mqtt::MessageType::kErrorOccurred,
            .data =
                mqtt::ErrorOccurredPayload{
                    .job_id = active_job,
                    .error_code = "ERR-EMERGENCY-STOP",
                    .error_level = "CRITICAL",
                    .current_state = "EMERGENCY_STOP",
                    .message = "line tracer controller reported an emergency stop",
                    .distance = std::nullopt,
                },
        });
        return;
    }
    if (!HasActiveJob() || job_id != active_uart_job_id_ || route_id != active_route_id_) {
        return;
    }

    const auto active_job = std::optional<std::string>{ active_work_id_ };

    if (event_id == UART_LINETRACER_EVENT_STATE_CHANGED) {
        last_uart_state_ = frame.payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX];
        const std::optional<std::string> error_code = last_uart_state_ == UART_LINETRACER_STATE_EMERGENCY_STOP
                                                          ? std::optional<std::string>{ "ERR-EMERGENCY-STOP" }
                                                          : std::nullopt;
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = StateConnection(last_uart_state_),
                    .current_state = UartStateName(last_uart_state_, route_id),
                    .job_id = active_job,
                    .error_code = error_code,
                },
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_ARRIVED) {
        const std::string state = last_uart_state_ == UART_LINETRACER_STATE_LOAD_WAIT
                                      ? RouteState("PICKUP_READY", route_id)
                                      : RouteState("ARRIVED", route_id);
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = state,
                    .job_id = active_job,
                    .error_code = std::nullopt,
                },
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_LOAD_DETECTED) {
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = RouteState("LOAD_ON", route_id),
                    .job_id = active_job,
                    .error_code = std::nullopt,
                },
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_UNLOAD_COMPLETE) {
        const std::string completed_work_id = active_work_id_;
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = RouteState("LOAD_OFF", route_id),
                    .job_id = active_job,
                    .error_code = std::nullopt,
                },
        });
        EmitReport({
            .channel = LineTracerReportChannel::kEvent,
            .message_type = mqtt::MessageType::kWorkCompleted,
            .data =
                mqtt::WorkCompletedPayload{
                    .work_id = completed_work_id,
                    .result = "SUCCESS",
                    .message = std::string("line tracer unload completed"),
                },
        });

        active_work_id_.clear();
        active_uart_job_id_ = UART_LINETRACER_JOB_ID_NONE;
        active_route_id_ = UART_LINETRACER_ROUTE_NONE;
        last_uart_state_ = UART_LINETRACER_STATE_IDLE;
        ClearPending();

        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = RouteState("PARKED", route_id),
                    .job_id = std::nullopt,
                    .error_code = std::nullopt,
                },
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_FAULT) {
        const std::uint8_t error = frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX];
        const std::string error_code = UartErrorCode(error);
        EmitReport({
            .channel = LineTracerReportChannel::kError,
            .message_type = mqtt::MessageType::kErrorOccurred,
            .data =
                mqtt::ErrorOccurredPayload{
                    .job_id = active_job,
                    .error_code = error_code,
                    .error_level = error == UART_ERROR_EMERGENCY_STOP ? "CRITICAL" : "ERROR",
                    .current_state = error == UART_ERROR_EMERGENCY_STOP ? "EMERGENCY_STOP" : "FAULT",
                    .message = "line tracer controller reported a fault",
                    .distance = std::nullopt,
                },
        });
    }
}

void LineTracerNode::EmitPendingResponse(mqtt::CommandResult result, std::optional<std::string> error_code,
                                         std::string message) const noexcept {
    if (!pending_.active || pending_.request_id.empty() || pending_.mqtt_command == mqtt::ControlCommand::kUnknown) {
        return;
    }

    EmitReport({
        .channel = LineTracerReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data =
            mqtt::CommandResponsePayload{
                .request_id = pending_.request_id,
                .command = pending_.mqtt_command,
                .result = result,
                .error_code = std::move(error_code),
                .message = std::move(message),
            },
    });
}

void LineTracerNode::EmitSafetyResponse(mqtt::CommandResult result, std::optional<std::string> error_code,
                                        std::string message) const noexcept {
    if (!pending_safety_.active || pending_safety_.request_id.empty() ||
        pending_safety_.command == mqtt::ControlCommand::kUnknown) {
        return;
    }
    EmitReport({
        .channel = LineTracerReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data =
            mqtt::CommandResponsePayload{
                .request_id = pending_safety_.request_id,
                .command = pending_safety_.command,
                .result = result,
                .error_code = std::move(error_code),
                .message = std::move(message),
            },
    });
}

void LineTracerNode::EmitReport(LineTracerReport report) const noexcept {
    if (!report_handler_) {
        return;
    }
    try {
        report_handler_(report);
    } catch (...) {
        // A reporting failure must not unwind into the UART receive loop.
    }
}

}  // namespace logistics::device
