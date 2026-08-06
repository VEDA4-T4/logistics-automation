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

[[nodiscard]] mqtt::LineTracerPositionPayload PositionPayload(std::string area, std::uint8_t route_id) {
    return {
        .area = std::move(area),
        .location = std::string(1U, RouteName(route_id)),
    };
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
    if (state == UART_LINETRACER_STATE_FAULT) {
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
        ResetStatusKeepalive();
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

    if (keepalive_pending_ && event.pending_sequence == keepalive_sequence_ &&
        event.pending_command == UART_CMD_LINETRACER_GET_STATUS) {
        if (event.type == UartSessionEventType::kCommandResponseReceived) {
            if (event.frame.command == UART_CMD_RESPONSE && event.frame.length == UART_LINETRACER_STATUS_PAYLOAD_SIZE &&
                event.frame.payload[UART_RESPONSE_COMMAND_INDEX] == UART_CMD_LINETRACER_GET_STATUS &&
                (event.frame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_ACK ||
                 event.frame.payload[UART_RESPONSE_STATUS_INDEX] == UART_STATUS_SUCCESS) &&
                uart_linetracer_state_is_valid(event.frame.payload[UART_LINETRACER_STATUS_STATE_INDEX]) != 0U) {
                last_uart_state_ = event.frame.payload[UART_LINETRACER_STATUS_STATE_INDEX];
            }
            ResetStatusKeepalive();
            return;
        }
        if (event.type == UartSessionEventType::kAckTimeout) {
            ResetStatusKeepalive();
            return;
        }
    }

    if (!pending_.active || event.pending_sequence != pending_.sequence) {
        return;
    }

    if (event.type == UartSessionEventType::kAckReceived) {
        const std::uint8_t status = event.frame.payload[UART_ACK_STATUS_INDEX];
        const bool accepted = status == UART_STATUS_ACK || status == UART_STATUS_SUCCESS;
        if (accepted && pending_.stage == PendingStage::kResetBeforePosition) {
            active_work_id_.clear();
            active_uart_job_id_ = UART_LINETRACER_JOB_ID_NONE;
            active_route_id_ = UART_LINETRACER_ROUTE_NONE;
            current_position_ = UART_LINETRACER_POSITION_NONE;
            departure_position_.reset();
            target_position_.reset();
            confirmed_position_.reset();
            movement_state_ = "IDLE";
            ResetStatusKeepalive();

            const std::array<std::uint8_t, UART_LINETRACER_SET_POSITION_PAYLOAD_SIZE> payload{
                pending_.requested_position
            };
            const UartSessionSendResult send_result =
                uart_session_.SendCommand(UART_CMD_LINETRACER_SET_CURRENT_POSITION, payload);
            if (send_result.Succeeded()) {
                pending_.sequence = send_result.sequence;
                pending_.stage = PendingStage::kSetPosition;
                return;
            }

            const LineTracerCommandStatus send_status = ToCommandStatus(send_result.status);
            if (send_status == LineTracerCommandStatus::kUartBusy) {
                EmitPendingResponse(mqtt::CommandResult::kRejected, std::string("ERR-UART-BUSY"),
                                    "UART is busy before current position could be set");
            } else if (send_status == LineTracerCommandStatus::kUartNotOpen) {
                EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-DISCONNECTED"),
                                    "UART disconnected before current position could be set");
            } else {
                EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-IO"),
                                    "failed to send current position after reset");
            }
            ClearPending();
            return;
        }

        if (accepted && pending_.effect == PendingEffect::kActivateJob) {
            active_work_id_ = pending_.work_id;
            active_uart_job_id_ = pending_.uart_job_id;
            active_route_id_ = pending_.route_id;
            departure_position_ = confirmed_position_;
            target_position_ = PositionPayload("DESTINATION", pending_.route_id);
            movement_state_ = "MOVING";
            ResetStatusKeepalive();
        } else if (accepted && pending_.effect == PendingEffect::kClearJob) {
            active_work_id_.clear();
            active_uart_job_id_ = UART_LINETRACER_JOB_ID_NONE;
            active_route_id_ = UART_LINETRACER_ROUTE_NONE;
            current_position_ = UART_LINETRACER_POSITION_NONE;
            departure_position_.reset();
            target_position_.reset();
            confirmed_position_.reset();
            movement_state_ = "IDLE";
            ResetStatusKeepalive();
        }
        if (accepted && pending_.stage == PendingStage::kSetPosition) {
            current_position_ = pending_.requested_position;
            confirmed_position_ = PositionPayload(pending_.requested_area, pending_.requested_position);
            departure_position_ = confirmed_position_;
            target_position_ = confirmed_position_;
            movement_state_ = "IDLE";
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
        if (accepted && pending_.stage == PendingStage::kSetPosition) {
            EmitDeviceStatus(mqtt::ConnectionState::kOnline, "IDLE", std::nullopt, std::nullopt);
        } else if (accepted && pending_.effect == PendingEffect::kActivateJob) {
            EmitDeviceStatus(mqtt::ConnectionState::kOnline, "MOVING", std::optional<std::string>{ active_work_id_ },
                             std::nullopt);
        } else if (accepted && pending_.effect == PendingEffect::kClearJob) {
            EmitDeviceStatus(mqtt::ConnectionState::kOnline, "POSITION_UNKNOWN", std::nullopt, std::nullopt);
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
    if (elapsed <= std::chrono::milliseconds::zero()) {
        return;
    }

    if (uart_session_.IsOpen()) {
        keepalive_elapsed_ += elapsed;
        if (keepalive_elapsed_ > kStatusKeepaliveInterval) {
            keepalive_elapsed_ = kStatusKeepaliveInterval;
        }
    } else {
        ResetStatusKeepalive();
    }

    if (pending_safety_.active) {
        pending_safety_.elapsed += elapsed;
        if (pending_safety_.elapsed >= mqtt::kEmergencyStopConfirmationTimeout) {
            EmitSafetyResponse(mqtt::CommandResult::kTimeout, std::string("ERR-SAFETY-CONFIRMATION-TIMEOUT"),
                               "line tracer controller did not confirm the emergency stop");
            pending_safety_ = {};
        }
    }
}

bool LineTracerNode::TrySendStatusKeepalive() noexcept {
    if (keepalive_elapsed_ < kStatusKeepaliveInterval || keepalive_pending_ || pending_.active ||
        pending_safety_.active || !uart_session_.IsOpen() || uart_session_.HasPendingCommand()) {
        return false;
    }

    const UartSessionSendResult result = uart_session_.SendCommand(UART_CMD_LINETRACER_GET_STATUS);
    if (!result.Succeeded()) {
        return false;
    }

    keepalive_pending_ = true;
    keepalive_sequence_ = result.sequence;
    keepalive_elapsed_ = std::chrono::milliseconds::zero();
    return true;
}

bool LineTracerNode::HasActiveJob() const noexcept {
    return active_uart_job_id_ != UART_LINETRACER_JOB_ID_NONE;
}

bool LineTracerNode::HasPendingSafetyCommand() const noexcept {
    return pending_safety_.active;
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

std::uint8_t LineTracerNode::CurrentPosition() const noexcept {
    return current_position_;
}

mqtt::DeviceStatusPayload LineTracerNode::MakeDeviceStatusPayload(mqtt::ConnectionState status,
                                                                  std::string current_state,
                                                                  std::optional<std::string> job_id,
                                                                  std::optional<std::string> error_code) const {
    return {
        .status = status,
        .current_state = std::move(current_state),
        .job_id = std::move(job_id),
        .error_code = std::move(error_code),
        .departure_position = departure_position_,
        .target_position = target_position_,
        .confirmed_position = confirmed_position_,
        .movement_state =
            departure_position_.has_value() && target_position_.has_value() && confirmed_position_.has_value()
                ? std::optional<std::string>{ movement_state_ }
                : std::nullopt,
        .position_reset = !departure_position_.has_value() && !target_position_.has_value() &&
                          !confirmed_position_.has_value() && current_position_ == UART_LINETRACER_POSITION_NONE,
    };
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

std::optional<std::uint8_t> LineTracerNode::PositionFromDestination(std::string_view destination) {
    const std::optional<std::uint8_t> route_id = RouteFromDestination(destination);
    if (!route_id.has_value()) {
        return std::nullopt;
    }

    switch (*route_id) {
        case UART_LINETRACER_ROUTE_A:
            return UART_LINETRACER_POSITION_DEST_A;
        case UART_LINETRACER_ROUTE_B:
            return UART_LINETRACER_POSITION_DEST_B;
        case UART_LINETRACER_ROUTE_C:
            return UART_LINETRACER_POSITION_DEST_C;
        default:
            return std::nullopt;
    }
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
    if (current_position_ == UART_LINETRACER_POSITION_NONE) {
        result.status = LineTracerCommandStatus::kCurrentPositionUnknown;
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

    const DeviceControlAction action = ResolveDeviceControlAction(command.command, command.component_id);
    if (pending_safety_.active && action != DeviceControlAction::kStatusRequest) {
        result.status = LineTracerCommandStatus::kSafetyCommandPending;
        return result;
    }

    std::uint8_t uart_command = UART_CMD_NONE;
    PendingEffect effect = PendingEffect::kNone;
    std::optional<std::uint8_t> requested_position;
    std::string requested_area;
    std::array<std::uint8_t, UART_LINETRACER_STOP_PAYLOAD_SIZE> payload{};
    std::size_t payload_length = 0U;

    switch (action) {
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
            if (const auto position = command.params.find("currentPosition"); position != command.params.end()) {
                std::string location;
                requested_area = "DEPARTURE";
                if (position->is_string()) {
                    location = position->get<std::string>();
                } else if (position->is_object()) {
                    const auto area = position->find("area");
                    const auto nested_location = position->find("location");
                    if (area == position->end() || nested_location == position->end() || !area->is_string() ||
                        !nested_location->is_string()) {
                        result.status = LineTracerCommandStatus::kInvalidPosition;
                        return result;
                    }
                    requested_area = area->get<std::string>();
                    location = nested_location->get<std::string>();
                    const mqtt::LineTracerPositionPayload requested{
                        .area = requested_area,
                        .location = location,
                    };
                    if (!requested.IsValid()) {
                        result.status = LineTracerCommandStatus::kInvalidPosition;
                        return result;
                    }
                } else {
                    result.status = LineTracerCommandStatus::kInvalidPosition;
                    return result;
                }
                requested_position = PositionFromDestination(location);
                if (!requested_position.has_value()) {
                    result.status = LineTracerCommandStatus::kInvalidPosition;
                    return result;
                }
            }
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
        if (requested_position.has_value()) {
            pending_.stage = PendingStage::kResetBeforePosition;
            pending_.requested_position = *requested_position;
            pending_.requested_area = std::move(requested_area);
        }
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
    if (pending_safety_.active) {
        result.status = LineTracerCommandStatus::kSafetyCommandPending;
        return result;
    }
    if (pending_.active) {
        EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-EMERGENCY-STOP"),
                            "line tracer command was preempted by an emergency stop");
        ClearPending();
    }
    static_cast<void>(uart_session_.CancelPendingCommand());
    ResetStatusKeepalive();
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

void LineTracerNode::ResetStatusKeepalive() noexcept {
    keepalive_elapsed_ = std::chrono::milliseconds::zero();
    keepalive_pending_ = false;
    keepalive_sequence_ = 0U;
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
            .data = MakeDeviceStatusPayload(mqtt::ConnectionState::kOnline, "EMERGENCY_STOP", active_job, std::nullopt),
        });
        return;
    }
    if (!HasActiveJob() || job_id != active_uart_job_id_ || route_id != active_route_id_) {
        return;
    }

    const auto active_job = std::optional<std::string>{ active_work_id_ };

    if (event_id == UART_LINETRACER_EVENT_STATE_CHANGED) {
        last_uart_state_ = frame.payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX];
        if (last_uart_state_ == UART_LINETRACER_STATE_FOLLOWING_LINE ||
            last_uart_state_ == UART_LINETRACER_STATE_CORRECTING ||
            last_uart_state_ == UART_LINETRACER_STATE_LOAD_WAIT ||
            last_uart_state_ == UART_LINETRACER_STATE_UNLOADING) {
            movement_state_ = "MOVING";
        } else if (last_uart_state_ == UART_LINETRACER_STATE_ARRIVED) {
            confirmed_position_ = target_position_;
            movement_state_ = "ARRIVED";
        }
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data = MakeDeviceStatusPayload(StateConnection(last_uart_state_),
                                            UartStateName(last_uart_state_, route_id), active_job, std::nullopt),
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_ARRIVED) {
        const bool pickup_ready = last_uart_state_ == UART_LINETRACER_STATE_LOAD_WAIT;
        const std::string state = pickup_ready ? RouteState("PICKUP_READY", route_id) : RouteState("ARRIVED", route_id);
        if (!pickup_ready) {
            confirmed_position_ = target_position_;
            movement_state_ = "ARRIVED";
        }
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data = MakeDeviceStatusPayload(mqtt::ConnectionState::kOnline, state, active_job, std::nullopt),
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_LOAD_DETECTED) {
        movement_state_ = "MOVING";
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data = MakeDeviceStatusPayload(mqtt::ConnectionState::kOnline, RouteState("LOAD_ON", route_id), active_job,
                                            std::nullopt),
        });
        return;
    }

    if (event_id == UART_LINETRACER_EVENT_UNLOAD_COMPLETE) {
        const std::string completed_work_id = active_work_id_;
        confirmed_position_ = target_position_;
        movement_state_ = "ARRIVED";
        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data = MakeDeviceStatusPayload(mqtt::ConnectionState::kOnline, RouteState("LOAD_OFF", route_id),
                                            active_job, std::nullopt),
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

        current_position_ = route_id;
        active_work_id_.clear();
        active_uart_job_id_ = UART_LINETRACER_JOB_ID_NONE;
        active_route_id_ = UART_LINETRACER_ROUTE_NONE;
        last_uart_state_ = UART_LINETRACER_STATE_IDLE;
        movement_state_ = "IDLE";
        ClearPending();
        ResetStatusKeepalive();

        EmitReport({
            .channel = LineTracerReportChannel::kStatus,
            .message_type = mqtt::MessageType::kDeviceStatus,
            .data = MakeDeviceStatusPayload(mqtt::ConnectionState::kOnline, RouteState("PARKED", route_id),
                                            std::nullopt, std::nullopt),
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

void LineTracerNode::EmitDeviceStatus(mqtt::ConnectionState status, std::string current_state,
                                      std::optional<std::string> job_id,
                                      std::optional<std::string> error_code) const noexcept {
    EmitReport({
        .channel = LineTracerReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data = MakeDeviceStatusPayload(status, std::move(current_state), std::move(job_id), std::move(error_code)),
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
