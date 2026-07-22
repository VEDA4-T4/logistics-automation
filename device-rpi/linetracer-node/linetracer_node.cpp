#include "logistics/device/linetracer_node.hpp"

#include <array>
#include <cctype>
#include <span>
#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

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

}  // namespace

bool LineTracerCommandResult::Succeeded() const noexcept {
    return status == LineTracerCommandStatus::kSent;
}

LineTracerNode::LineTracerNode(std::string device_id, UartSession& uart_session)
    : device_id_(std::move(device_id)), uart_session_(uart_session) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("line tracer device ID must be one non-wildcard MQTT topic level");
    }
}

LineTracerCommandResult LineTracerNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (!message.IsValid()) {
        return { .status = LineTracerCommandStatus::kInvalidMessage };
    }

    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return HandleDestinationSet(*destination);
    }
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return HandleControlCommand(*command);
    }
    return { .status = LineTracerCommandStatus::kUnsupportedMessage };
}

void LineTracerNode::HandleUartEvent(const UartSessionEvent& event) noexcept {
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
        ClearPending();
        return;
    }

    if (event.type == UartSessionEventType::kAckTimeout || event.type == UartSessionEventType::kTransportDisconnected ||
        event.type == UartSessionEventType::kTransportError) {
        ClearPending();
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

    if (command.command == mqtt::ControlCommand::kStop) {
        result.uart_command = UART_CMD_LINETRACER_STOP_DRIVE;
        if (!HasActiveJob()) {
            result.status = LineTracerCommandStatus::kNoActiveJob;
            return result;
        }
        const std::array<std::uint8_t, UART_LINETRACER_STOP_PAYLOAD_SIZE> payload{
            static_cast<std::uint8_t>(active_uart_job_id_ & 0xffU),
            static_cast<std::uint8_t>((active_uart_job_id_ >> 8U) & 0xffU)
        };
        result = Send(std::move(result), UART_CMD_LINETRACER_STOP_DRIVE, payload.data(), payload.size());
        if (result.Succeeded()) {
            RememberPending(PendingEffect::kNone, result);
        }
        return result;
    }

    if (command.command == mqtt::ControlCommand::kStart || command.command == mqtt::ControlCommand::kRestart) {
        result.uart_command = UART_CMD_LINETRACER_RESUME_DRIVE;
        if (!HasActiveJob()) {
            result.status = LineTracerCommandStatus::kNoActiveJob;
            return result;
        }
        result = Send(std::move(result), UART_CMD_LINETRACER_RESUME_DRIVE, nullptr,
                      UART_LINETRACER_RESUME_DRIVE_PAYLOAD_SIZE);
        if (result.Succeeded()) {
            RememberPending(PendingEffect::kNone, result);
        }
        return result;
    }

    if (command.command == mqtt::ControlCommand::kInitialize || command.command == mqtt::ControlCommand::kRecovery) {
        result.uart_command = UART_CMD_LINETRACER_RESET_SYSTEM;
        result = Send(std::move(result), UART_CMD_LINETRACER_RESET_SYSTEM, nullptr, UART_LINETRACER_RESET_PAYLOAD_SIZE);
        if (result.Succeeded()) {
            RememberPending(PendingEffect::kClearJob, result);
        }
        return result;
    }

    result.status = LineTracerCommandStatus::kUnsupportedCommand;
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

bool LineTracerNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return target_device_id == device_id_ || target_device_id == "ALL";
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
        .work_id = result.work_id,
        .uart_job_id = result.uart_job_id,
        .route_id = result.uart_route_id,
    };
}

void LineTracerNode::ClearPending() noexcept {
    pending_ = {};
}

}  // namespace logistics::device
