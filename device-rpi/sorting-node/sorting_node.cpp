#include "logistics/device/sorting_node.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/uart/conveyor_events.h"
#include "logistics/device/device_control_policy.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

inline constexpr auto kControllerHeartbeatTimeout = std::chrono::seconds{ 3 };

[[nodiscard]] SortingCommandStatus ToCommandStatus(UartSessionSendStatus status) noexcept {
    switch (status) {
        case UartSessionSendStatus::kSent:
            return SortingCommandStatus::kSent;
        case UartSessionSendStatus::kNotOpen:
            return SortingCommandStatus::kUartNotOpen;
        case UartSessionSendStatus::kBusy:
            return SortingCommandStatus::kUartBusy;
        case UartSessionSendStatus::kInvalidArgument:
        case UartSessionSendStatus::kEncodeError:
        case UartSessionSendStatus::kTransportError:
            return SortingCommandStatus::kUartError;
    }
    return SortingCommandStatus::kUartError;
}

[[nodiscard]] std::string UartSendErrorCode(UartSessionSendStatus status) {
    switch (status) {
        case UartSessionSendStatus::kNotOpen:
            return "ERR-UART-DISCONNECTED";
        case UartSessionSendStatus::kBusy:
            return "ERR-UART-BUSY";
        case UartSessionSendStatus::kInvalidArgument:
        case UartSessionSendStatus::kEncodeError:
        case UartSessionSendStatus::kTransportError:
            return "ERR-UART-IO";
        case UartSessionSendStatus::kSent:
            break;
    }
    return "ERR-UART-IO";
}

[[nodiscard]] std::string NormalizeIdentifier(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return normalized;
}

[[nodiscard]] std::string DestinationSuffix(std::uint8_t destination) {
    return destination == UART_SORTING_DESTINATION_NONE ? std::string{} : "_DEST_" + std::to_string(destination);
}

[[nodiscard]] std::string GateStateName(std::uint8_t state, std::uint8_t destination) {
    switch (state) {
        case UART_SORTING_GATE_HOME:
            return "IDLE";
        case UART_SORTING_GATE_MOVING:
            return "GATE_MOVING" + DestinationSuffix(destination);
        case UART_SORTING_GATE_WAIT_ITEM:
            return "WAITING_ITEM" + DestinationSuffix(destination);
        case UART_SORTING_GATE_RETURNING:
            return "RETURNING_HOME";
        case UART_SORTING_GATE_FAULT:
            return "FAULT";
        default:
            return "UNKNOWN";
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
            return "FAULT";
        case UART_DEVICE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

// Measurement health only (IsValidMeasurementStatus in mqtt_codec.hpp). Box
// arrival is decided by the central server from distanceCm, so this node just
// relays the reading it was handed.
[[nodiscard]] std::string SensorStateName(std::uint8_t state) {
    switch (state) {
        case UART_SENSOR_FAULT:
            return "FAULT";
        case UART_SENSOR_OK:
        default:
            return "OK";
    }
}

[[nodiscard]] std::string UartErrorCode(std::uint8_t error) {
    switch (error) {
        case UART_ERROR_NONE:
            return {};
        case UART_ERROR_INVALID_VERSION:
            return "ERR-UART-VERSION";
        case UART_ERROR_INVALID_COMMAND:
        case UART_ERROR_UNSUPPORTED_COMMAND:
            return "ERR-UART-COMMAND";
        case UART_ERROR_INVALID_LENGTH:
        case UART_ERROR_INVALID_PAYLOAD:
            return "ERR-UART-PAYLOAD";
        case UART_ERROR_SPEED_NOT_CONFIGURED:
            return "ERR-SPEED-NOT-CONFIGURED";
        case UART_ERROR_CRC_MISMATCH:
            return "ERR-UART-CRC";
        case UART_ERROR_SEQUENCE:
            return "ERR-UART-SEQUENCE";
        case UART_ERROR_TIMEOUT:
            return "ERR-UART-TIMEOUT";
        case UART_ERROR_BUSY:
            return "ERR-UART-BUSY";
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

[[nodiscard]] mqtt::CommandResult CommandResultFromUartStatus(std::uint8_t status, std::uint8_t error) noexcept {
    switch (status) {
        case UART_STATUS_ACK:
        case UART_STATUS_SUCCESS:
            return mqtt::CommandResult::kSuccess;
        case UART_STATUS_NACK:
        case UART_STATUS_BUSY:
            return mqtt::CommandResult::kRejected;
        case UART_STATUS_ERROR:
            return error == UART_ERROR_EMERGENCY_STOP ? mqtt::CommandResult::kRejected : mqtt::CommandResult::kFailed;
        default:
            return mqtt::CommandResult::kFailed;
    }
}

[[nodiscard]] std::uint16_t ReadLittleEndianU16(const std::uint8_t* payload, std::size_t low_index) noexcept {
    return static_cast<std::uint16_t>(payload[low_index]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(payload[low_index + 1U]) << 8U);
}

}  // namespace

bool SortingCommandResult::Succeeded() const noexcept {
    return status == SortingCommandStatus::kSent || status == SortingCommandStatus::kSentNoReply;
}

SortingNode::SortingNode(std::string device_id, UartSession& uart_session, const std::uint8_t default_speed)
    : device_id_(std::move(device_id)), uart_session_(uart_session), default_speed_(default_speed) {
    if (!mqtt::IsValidTopicLevel(device_id_) || default_speed == 0U ||
        default_speed > UART_SORTING_CONVEYOR_SPEED_MAX) {
        throw std::invalid_argument("sorting device ID must be one non-wildcard MQTT topic level");
    }
}

void SortingNode::SetReportHandler(SortingReportHandler handler) {
    report_handler_ = std::move(handler);
}

SortingCommandResult SortingNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (!message.IsValid()) {
        return { .status = SortingCommandStatus::kInvalidMessage };
    }
    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return HandleDestinationSet(*destination);
    }
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return HandleControlCommand(*command);
    }
    if (const auto* emergency = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        return HandleEmergencyStop(*emergency);
    }
    return { .status = SortingCommandStatus::kUnsupportedMessage };
}

SortingCommandResult SortingNode::RequestControllerStatus() {
    SortingCommandResult result{
        .mqtt_command = mqtt::ControlCommand::kUnknown,
        .uart_command = UART_CMD_SORTING_GET_STATUS,
    };
    result = Send(std::move(result), UART_CMD_SORTING_GET_STATUS, nullptr, UART_SORTING_GET_STATUS_PAYLOAD_SIZE);
    if (result.Succeeded()) {
        RememberPending(PendingEffect::kNone, result);
    }
    return result;
}

void SortingNode::HandleUartEvent(const UartSessionEvent& event) noexcept {
    if (event.type == UartSessionEventType::kFrameReceived) {
        HandleSortingFrame(event.frame);
        return;
    }
    if (event.type == UartSessionEventType::kParserError) {
        const bool crc_error = event.parser_result == UART_PARSER_CRC_ERROR;
        EmitError(crc_error ? "ERR-UART-CRC" : "ERR-UART-PARSER", "ERROR", "UART_ERROR",
                  "sorting UART parser rejected a frame");
        return;
    }
    if (!pending_.active || event.pending_sequence != pending_.sequence) {
        return;
    }
    if (event.type == UartSessionEventType::kCommandResponseReceived ||
        event.type == UartSessionEventType::kAckReceived) {
        HandleCommandResponse(event);
        return;
    }
    if (event.type == UartSessionEventType::kAckTimeout) {
        if (pending_.effect == PendingEffect::kActivateCycle) {
            active_work_id_ = pending_.work_id;
            active_cycle_id_ = pending_.uart_cycle_id;
            active_destination_ = pending_.uart_destination;
        }
        EmitPendingResponse(mqtt::CommandResult::kTimeout, std::string("ERR-UART-RESPONSE-TIMEOUT"),
                            "sorting controller response timed out after retries");
        EmitError("ERR-UART-RESPONSE-TIMEOUT", "ERROR", "UART_TIMEOUT",
                  "sorting controller did not respond after UART retries");
        ClearPending();
        return;
    }
    if (event.type == UartSessionEventType::kTransportDisconnected ||
        event.type == UartSessionEventType::kTransportError) {
        EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-UART-DISCONNECTED"),
                            "UART transport disconnected before the sorting response");
        ClearPending();
    }
}

bool SortingNode::HasActiveCycle() const noexcept {
    return active_cycle_id_ != UART_SORTING_CYCLE_ID_NONE;
}

bool SortingNode::HasPendingSafetyCommand() const noexcept {
    return pending_safety_.active;
}

std::string_view SortingNode::ActiveWorkId() const noexcept {
    return active_work_id_;
}

std::uint16_t SortingNode::ActiveCycleId() const noexcept {
    return active_cycle_id_;
}

std::uint8_t SortingNode::ActiveDestination() const noexcept {
    return active_destination_;
}

std::optional<std::uint8_t> SortingNode::DestinationFromString(std::string_view destination) {
    const std::string normalized = NormalizeIdentifier(destination);
    if (normalized == "1" || normalized == "A" || normalized == "DEST1" || normalized == "DEST01" ||
        normalized == "DESTA" || normalized == "ZONEA") {
        return UART_SORTING_DESTINATION_1;
    }
    if (normalized == "2" || normalized == "B" || normalized == "DEST2" || normalized == "DEST02" ||
        normalized == "DESTB" || normalized == "ZONEB") {
        return UART_SORTING_DESTINATION_2;
    }
    if (normalized == "3" || normalized == "C" || normalized == "DEST3" || normalized == "DEST03" ||
        normalized == "DESTC" || normalized == "ZONEC") {
        return UART_SORTING_DESTINATION_3;
    }
    return std::nullopt;
}

SortingCommandResult SortingNode::HandleDestinationSet(const mqtt::DestinationSetPayload& command) {
    SortingCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = command.work_id,
        .uart_command = UART_CMD_SORTING_ROUTE_ITEM,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = SortingCommandStatus::kInvalidTarget;
        return result;
    }

    const auto destination = DestinationFromString(command.destination);
    if (!destination.has_value()) {
        result.status = SortingCommandStatus::kInvalidDestination;
        return result;
    }
    if (HasActiveCycle()) {
        if (active_work_id_ == command.work_id && active_destination_ == *destination) {
            result.status = SortingCommandStatus::kDuplicate;
            result.uart_cycle_id = active_cycle_id_;
            result.uart_destination = active_destination_;
            return result;
        }
        result.status = SortingCommandStatus::kActiveCycleConflict;
        return result;
    }

    const std::uint16_t cycle_id = AllocateCycleId();
    const std::array<std::uint8_t, UART_SORTING_ROUTE_PAYLOAD_SIZE> payload{
        static_cast<std::uint8_t>(cycle_id & 0xffU), static_cast<std::uint8_t>((cycle_id >> 8U) & 0xffU), *destination
    };
    result.uart_cycle_id = cycle_id;
    result.uart_destination = *destination;
    result = Send(std::move(result), UART_CMD_SORTING_ROUTE_ITEM, payload.data(), payload.size());
    if (result.Succeeded()) {
        RememberPending(PendingEffect::kActivateCycle, result);
    }
    return result;
}

SortingCommandResult SortingNode::HandleControlCommand(const mqtt::ControlCommandPayload& command) {
    SortingCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = active_work_id_,
        .uart_cycle_id = active_cycle_id_,
        .uart_destination = active_destination_,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = SortingCommandStatus::kInvalidTarget;
        return result;
    }

    const DeviceControlAction action = ResolveDeviceControlAction(command.command, command.component_id);
    if (pending_safety_.active && action != DeviceControlAction::kStatusRequest) {
        result.status = SortingCommandStatus::kSafetyCommandPending;
        return result;
    }

    std::uint8_t uart_command = UART_CMD_NONE;
    PendingEffect effect = PendingEffect::kNone;
    std::array<std::uint8_t, UART_SORTING_CANCEL_PAYLOAD_SIZE> cycle_payload{};
    const std::uint8_t* payload = nullptr;
    std::size_t payload_length = 0U;
    std::uint8_t requested_speed = 0U;

    switch (action) {
        case DeviceControlAction::kStart: {
            bool invalid_speed = false;
            std::optional<std::uint8_t> speed =
                ReadControlSpeed(command.params, UART_SORTING_CONVEYOR_SPEED_MAX, invalid_speed);
            if (invalid_speed) {
                result.status = SortingCommandStatus::kInvalidSpeed;
                return result;
            }
            requested_speed = speed.value_or(configured_speed_ == 0U ? default_speed_ : configured_speed_);
            cycle_payload[0] = requested_speed;
            uart_command = UART_CMD_SORTING_CONVEYOR_SET_SPEED;
            effect = PendingEffect::kStartAfterSpeed;
            payload = cycle_payload.data();
            payload_length = UART_SORTING_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
            break;
        }
        case DeviceControlAction::kStop:
            uart_command = UART_CMD_SORTING_CONVEYOR_STOP;
            break;
        case DeviceControlAction::kInitialize:
            uart_command = UART_CMD_SORTING_RESET;
            effect = PendingEffect::kClearCycle;
            break;
        case DeviceControlAction::kStatusRequest:
            uart_command = NormalizeIdentifier(command.component_id) == "CONVEYOR"
                               ? UART_CMD_SORTING_CONVEYOR_GET_STATUS
                               : UART_CMD_SORTING_GET_STATUS;
            break;
        case DeviceControlAction::kSafetyRecovery:
            result.uart_command = UART_CMD_RESET_DEVICE;
            result = SendOneWay(std::move(result), UART_CMD_RESET_DEVICE, nullptr, 0U);
            if (result.Succeeded()) {
                pending_safety_ = { .active = true,
                                    .expected = PendingSafetyEvent::kResetComplete,
                                    .command = result.mqtt_command,
                                    .request_id = result.request_id };
            }
            return result;
        case DeviceControlAction::kComponentRecovery:
            if (NormalizeControlComponent(command.component_id) != "GATE") {
                result.status = SortingCommandStatus::kUnsupportedCommand;
                return result;
            }
            if (!HasActiveCycle()) {
                result.status = SortingCommandStatus::kNoActiveCycle;
                return result;
            }
            uart_command = UART_CMD_SORTING_RETURN_HOME;
            cycle_payload[0] = static_cast<std::uint8_t>(active_cycle_id_ & 0xffU);
            cycle_payload[1] = static_cast<std::uint8_t>((active_cycle_id_ >> 8U) & 0xffU);
            payload = cycle_payload.data();
            payload_length = UART_SORTING_RETURN_HOME_PAYLOAD_SIZE;
            break;
        case DeviceControlAction::kEmergencyStop:
        case DeviceControlAction::kUnsupported:
            result.status = SortingCommandStatus::kUnsupportedCommand;
            return result;
    }

    result.uart_command = uart_command;
    result = Send(std::move(result), uart_command, payload, payload_length);
    if (result.Succeeded()) {
        RememberPending(effect, result, requested_speed);
    }
    return result;
}

SortingCommandResult SortingNode::HandleEmergencyStop(const mqtt::EmergencyStopPayload& command) {
    SortingCommandResult result{
        .mqtt_command = command.command,
        .request_id = command.request_id,
        .work_id = active_work_id_,
        .uart_cycle_id = active_cycle_id_,
        .uart_destination = active_destination_,
        .uart_command = UART_CMD_EMERGENCY_STOP,
    };
    if (!IsTargetedToThisNode(command.target_device_id)) {
        result.status = SortingCommandStatus::kInvalidTarget;
        return result;
    }
    if (pending_safety_.active) {
        result.status = SortingCommandStatus::kSafetyCommandPending;
        return result;
    }
    if (pending_.active) {
        EmitPendingResponse(mqtt::CommandResult::kFailed, std::string("ERR-EMERGENCY-STOP"),
                            "sorting command was preempted by an emergency stop");
        ClearPending();
    }
    static_cast<void>(uart_session_.CancelPendingCommand());
    result = SendOneWay(std::move(result), UART_CMD_EMERGENCY_STOP, nullptr, 0U);
    if (result.Succeeded()) {
        pending_safety_ = { .active = true,
                            .expected = PendingSafetyEvent::kEstopLatched,
                            .command = result.mqtt_command,
                            .request_id = result.request_id };
    }
    return result;
}

void SortingNode::ResetControllerHeartbeatMonitor() noexcept {
    controller_heartbeat_elapsed_ = std::chrono::milliseconds::zero();
    controller_heartbeat_timed_out_ = false;
    last_device_state_ = 0xffU;
    last_device_error_ = 0xffU;
}

void SortingNode::Tick(std::chrono::milliseconds elapsed) noexcept {
    if (elapsed <= std::chrono::milliseconds::zero()) {
        return;
    }
    if (pending_safety_.active) {
        pending_safety_.elapsed += elapsed;
        if (pending_safety_.elapsed >= mqtt::kEmergencyStopConfirmationTimeout) {
            EmitSafetyResponse(mqtt::CommandResult::kTimeout, std::string("ERR-SAFETY-CONFIRMATION-TIMEOUT"),
                               "sorting controller did not confirm the safety command");
            pending_safety_ = {};
        }
    }
    if (!controller_heartbeat_timed_out_) {
        controller_heartbeat_elapsed_ += elapsed;
        if (controller_heartbeat_elapsed_ >= kControllerHeartbeatTimeout) {
            controller_heartbeat_timed_out_ = true;
            EmitStatus("UART_HEARTBEAT_TIMEOUT", std::string("ERR-UART-HEARTBEAT-TIMEOUT"));
            EmitError("ERR-UART-HEARTBEAT-TIMEOUT", "ERROR", "UART_HEARTBEAT_TIMEOUT",
                      "sorting controller heartbeat timed out");
        }
    }
}

SortingCommandResult SortingNode::Send(SortingCommandResult result, std::uint8_t command, const std::uint8_t* payload,
                                       std::size_t payload_length) {
    const std::span<const std::uint8_t> payload_view =
        payload == nullptr ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>{ payload, payload_length };
    if (command >= UART_CMD_SORTING_MIN && command <= UART_CMD_SORTING_MAX &&
        UART_IS_VALID_SORTING_PAYLOAD(command, payload, payload_length) == 0U) {
        result.status = SortingCommandStatus::kUartError;
        return result;
    }
    result.uart_result = uart_session_.SendCommand(command, payload_view);
    result.uart_sequence = result.uart_result.sequence;
    result.status = ToCommandStatus(result.uart_result.status);
    return result;
}

SortingCommandResult SortingNode::SendOneWay(SortingCommandResult result, std::uint8_t command,
                                             const std::uint8_t* payload, std::size_t payload_length) {
    const std::span<const std::uint8_t> payload_view =
        payload == nullptr ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>{ payload, payload_length };
    result.uart_result = uart_session_.SendOneWayCommand(command, payload_view);
    result.uart_sequence = result.uart_result.sequence;
    result.status = result.uart_result.Succeeded() ? SortingCommandStatus::kSentNoReply
                                                   : ToCommandStatus(result.uart_result.status);
    return result;
}

bool SortingNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return IsControlTargetForDevice(target_device_id, device_id_);
}

std::uint16_t SortingNode::AllocateCycleId() noexcept {
    std::uint16_t allocated = UART_SORTING_CYCLE_ID_NONE;
    do {
        allocated = next_cycle_id_;
        ++next_cycle_id_;
        if (next_cycle_id_ == UART_SORTING_CYCLE_ID_NONE) {
            next_cycle_id_ = UART_SORTING_CYCLE_ID_MIN;
        }
    } while (HasActiveCycle() && allocated == active_cycle_id_);
    return allocated;
}

void SortingNode::RememberPending(PendingEffect effect, const SortingCommandResult& result,
                                  std::uint8_t requested_speed) {
    pending_ = {
        .active = true,
        .effect = effect,
        .sequence = result.uart_sequence,
        .uart_command = result.uart_command,
        .mqtt_command = result.mqtt_command,
        .request_id = result.request_id,
        .work_id = result.work_id,
        .uart_cycle_id = result.uart_cycle_id,
        .uart_destination = result.uart_destination,
        .requested_speed = requested_speed,
    };
}

void SortingNode::ClearPending() noexcept {
    pending_ = {};
}

void SortingNode::ClearActiveCycle() noexcept {
    active_work_id_.clear();
    active_cycle_id_ = UART_SORTING_CYCLE_ID_NONE;
    active_destination_ = UART_SORTING_DESTINATION_NONE;
}

void SortingNode::HandleCommandResponse(const UartSessionEvent& event) noexcept {
    std::uint8_t status = UART_STATUS_ERROR;
    std::uint8_t error = UART_ERROR_INTERNAL;
    if (event.frame.command == UART_CMD_ACK && event.frame.length == UART_ACK_PAYLOAD_SIZE) {
        status = event.frame.payload[UART_ACK_STATUS_INDEX];
        error = status == UART_STATUS_ACK || status == UART_STATUS_SUCCESS ? UART_ERROR_NONE : UART_ERROR_INTERNAL;
    } else if (event.frame.command == UART_CMD_OPERATION_RESULT &&
               event.frame.length == UART_OPERATION_RESULT_PAYLOAD_SIZE) {
        status = event.frame.payload[UART_OPERATION_RESULT_STATUS_INDEX];
        error = event.frame.payload[UART_OPERATION_RESULT_ERROR_INDEX];
    } else if (event.frame.command == UART_CMD_RESPONSE && event.frame.length >= UART_RESPONSE_HEADER_SIZE) {
        status = event.frame.payload[UART_RESPONSE_STATUS_INDEX];
        error = event.frame.payload[UART_RESPONSE_ERROR_INDEX];
    }

    mqtt::CommandResult command_result = CommandResultFromUartStatus(status, error);
    bool accepted = command_result == mqtt::CommandResult::kSuccess;
    if (accepted && event.frame.command == UART_CMD_RESPONSE && !HandleStatusResponse(event.frame)) {
        accepted = false;
        command_result = mqtt::CommandResult::kFailed;
        error = UART_ERROR_INVALID_PAYLOAD;
        EmitError("ERR-UART-PAYLOAD", "ERROR", "UART_ERROR", "invalid sorting status response payload");
    }
    if (accepted && pending_.effect == PendingEffect::kStartAfterSpeed) {
        configured_speed_ = pending_.requested_speed;
        const UartSessionSendResult start_result = uart_session_.SendCommand(UART_CMD_SORTING_CONVEYOR_START);
        if (start_result.Succeeded()) {
            pending_.effect = PendingEffect::kNone;
            pending_.sequence = start_result.sequence;
            pending_.uart_command = UART_CMD_SORTING_CONVEYOR_START;
            pending_.requested_speed = 0U;
            return;
        }

        EmitPendingResponse(mqtt::CommandResult::kFailed, UartSendErrorCode(start_result.status),
                            "sorting speed was configured but conveyor start could not be sent");
        ClearPending();
        return;
    }
    if (accepted && pending_.effect == PendingEffect::kActivateCycle) {
        active_work_id_ = pending_.work_id;
        active_cycle_id_ = pending_.uart_cycle_id;
        active_destination_ = pending_.uart_destination;
    } else if (accepted && pending_.effect == PendingEffect::kClearCycle) {
        ClearActiveCycle();
    }

    if (accepted && event.frame.command != UART_CMD_RESPONSE) {
        switch (pending_.uart_command) {
            case UART_CMD_SORTING_ROUTE_ITEM:
                EmitStatus("GATE_MOVING" + DestinationSuffix(pending_.uart_destination));
                break;
            case UART_CMD_SORTING_RETURN_HOME:
                EmitStatus("RETURNING_HOME");
                break;
            case UART_CMD_SORTING_RESET:
                EmitStatus("IDLE");
                break;
            case UART_CMD_SORTING_CONVEYOR_START:
                EmitStatus("RUNNING");
                break;
            case UART_CMD_SORTING_CONVEYOR_STOP:
                EmitStatus("STOPPED");
                break;
            case UART_CMD_EMERGENCY_STOP:
                EmitStatus("EMERGENCY_STOP");
                break;
            default:
                break;
        }
    }

    const std::string error_code = UartErrorCode(error);
    const char* response_message = accepted ? "sorting command completed by controller"
                                   : command_result == mqtt::CommandResult::kRejected
                                       ? "sorting controller rejected the command"
                                       : "sorting controller failed the command";
    EmitPendingResponse(command_result, error_code.empty() ? std::nullopt : std::optional<std::string>{ error_code },
                        response_message);
    ClearPending();
}

void SortingNode::HandleSortingFrame(const uart_frame_t& frame) noexcept {
    if (frame.command == UART_CMD_SENSOR_STATUS) {
        HandleSensorStatus(frame);
        return;
    }
    if (frame.command == UART_CMD_DEVICE_STATUS) {
        HandleDeviceStatus(frame);
        return;
    }
    if (frame.command != UART_CMD_EVENT || frame.length < UART_EVENT_HEADER_SIZE) {
        return;
    }

    const std::uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];
    if (event_id == UART_SORTING_EVENT_CYCLE_COMPLETE) {
        HandleCycleComplete(frame);
    } else if (event_id == APP_EVENT_HEARTBEAT) {
        HandleHeartbeat(frame);
    } else if (event_id == APP_EVENT_SAFETY) {
        HandleSafetyEvent(frame);
    } else if (event_id == APP_EVENT_HEALTH) {
        HandleHealthEvent(frame);
    }
}

bool SortingNode::HandleStatusResponse(const uart_frame_t& frame) noexcept {
    if (pending_.uart_command == UART_CMD_SORTING_GET_STATUS && frame.length == UART_SORTING_STATUS_PAYLOAD_SIZE) {
        const std::uint8_t gate_state = frame.payload[UART_SORTING_STATUS_GATE_STATE_INDEX];
        const std::uint16_t cycle_id = ReadLittleEndianU16(frame.payload, UART_SORTING_STATUS_CYCLE_ID_LOW_INDEX);
        const std::uint8_t destination = frame.payload[UART_SORTING_STATUS_DESTINATION_INDEX];
        const bool idle_cycle = cycle_id == UART_SORTING_CYCLE_ID_NONE && destination == UART_SORTING_DESTINATION_NONE;
        const bool active_cycle =
            uart_sorting_cycle_id_is_valid(cycle_id) != 0U && uart_sorting_destination_is_valid(destination) != 0U;
        if (uart_sorting_gate_state_is_valid(gate_state) == 0U || (!idle_cycle && !active_cycle)) {
            return false;
        }
        if (cycle_id == UART_SORTING_CYCLE_ID_NONE) {
            ClearActiveCycle();
        } else if (!HasActiveCycle() || cycle_id != active_cycle_id_ || destination != active_destination_) {
            active_work_id_.clear();
            active_cycle_id_ = cycle_id;
            active_destination_ = destination;
            EmitError("ERR-CYCLE-MAPPING-UNKNOWN", "WARNING", GateStateName(gate_state, destination),
                      "STM32 has an active cycle that is not mapped to a server work ID");
        } else {
            active_destination_ = destination;
        }
        EmitStatus(GateStateName(gate_state, destination),
                   gate_state == UART_SORTING_GATE_FAULT ? std::optional<std::string>{ "ERR-SERVO" } : std::nullopt);
        return true;
    }

    if (pending_.uart_command == UART_CMD_SORTING_CONVEYOR_GET_STATUS &&
        frame.length == UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE) {
        const std::uint8_t state = frame.payload[UART_SORTING_CONVEYOR_STATUS_STATE_INDEX];
        const std::uint8_t speed = frame.payload[UART_SORTING_CONVEYOR_STATUS_SPEED_INDEX];
        if (uart_sorting_conveyor_state_is_valid(state) == 0U || speed > UART_SORTING_CONVEYOR_SPEED_MAX) {
            return false;
        }
        configured_speed_ = speed;
        const std::string current_state = state == UART_SORTING_CONVEYOR_RUNNING   ? "RUNNING"
                                          : state == UART_SORTING_CONVEYOR_STOPPED ? "STOPPED"
                                                                                   : "ERROR";
        EmitStatus(std::move(current_state),
                   state == UART_SORTING_CONVEYOR_FAULT ? std::optional<std::string>{ "ERR-MOTOR" } : std::nullopt);
        return true;
    }
    return false;
}

void SortingNode::HandleCycleComplete(const uart_frame_t& frame) noexcept {
    if (UART_IS_VALID_SORTING_EVENT_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint16_t cycle_id = ReadLittleEndianU16(frame.payload, UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX);
    const std::uint8_t destination = frame.payload[UART_SORTING_EVENT_DESTINATION_INDEX];
    if (!HasActiveCycle() || cycle_id != active_cycle_id_ || destination != active_destination_) {
        return;
    }
    if (active_work_id_.empty()) {
        EmitError("ERR-CYCLE-MAPPING-UNKNOWN", "ERROR", "IDLE",
                  "sorting cycle completed without a server work ID mapping");
        ClearActiveCycle();
        EmitStatus("IDLE");
        return;
    }

    EmitStatus("CYCLE_COMPLETE");
    ClearActiveCycle();
    EmitStatus("IDLE");
}

void SortingNode::HandleHeartbeat(const uart_frame_t& frame) noexcept {
    if (UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint8_t state = frame.payload[APP_HEARTBEAT_STATE_INDEX];
    const std::uint8_t error = frame.payload[APP_HEARTBEAT_ERROR_INDEX];
    const bool recovered = controller_heartbeat_timed_out_;
    controller_heartbeat_elapsed_ = std::chrono::milliseconds::zero();
    controller_heartbeat_timed_out_ = false;
    ClearControllerEventDedupIfHealthy(state, error);
    if (!recovered && state == last_device_state_ && error == last_device_error_) {
        return;
    }
    last_device_state_ = state;
    last_device_error_ = error;
    const std::string error_code = state == UART_DEVICE_EMERGENCY_STOP ? std::string{} : UartErrorCode(error);
    EmitStatus(DeviceStateName(state), error_code.empty() ? std::nullopt : std::optional<std::string>{ error_code });
    if (!error_code.empty()) {
        EmitError(error_code, state == UART_DEVICE_EMERGENCY_STOP ? "CRITICAL" : "ERROR", DeviceStateName(state),
                  "sorting controller heartbeat reported an error");
    }
}

void SortingNode::HandleSensorStatus(const uart_frame_t& frame) noexcept {
    if (uart_sorting_sensor_status_is_valid(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint8_t sensor_id = frame.payload[UART_SENSOR_ID_INDEX];
    const std::uint8_t state = frame.payload[UART_SENSOR_STATE_INDEX];
    const std::uint16_t distance = ReadLittleEndianU16(frame.payload, UART_SENSOR_DISTANCE_CM_LOW_INDEX);
    const std::size_t index = static_cast<std::size_t>(sensor_id - UART_SORTING_SENSOR_ID_MIN);
    EmitReport({
        .channel = SortingReportChannel::kEvent,
        .message_type = mqtt::MessageType::kSensorStatus,
        .data =
            mqtt::SensorStatusPayload{
                .sensor_id = sensor_id,
                .measurement_status = SensorStateName(state),
                .distance_cm = distance,
            },
    });
    if (sensor_states_[index] == state) {
        return;
    }
    sensor_states_[index] = state;
    if (state == UART_SENSOR_FAULT) {
        EmitError("ERR-SENSOR", "ERROR", "SENSOR_" + std::to_string(sensor_id) + "_FAULT",
                  "sorting ultrasonic sensor reported a fault",
                  distance == UART_SENSOR_DISTANCE_UNKNOWN
                      ? std::nullopt
                      : std::optional<std::int32_t>{ static_cast<std::int32_t>(distance) });
    }
}

void SortingNode::HandleDeviceStatus(const uart_frame_t& frame) noexcept {
    if (frame.length != UART_DEVICE_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t state = frame.payload[UART_DEVICE_STATUS_STATE_INDEX];
    const std::uint8_t error = frame.payload[UART_DEVICE_STATUS_ERROR_INDEX];
    if (state > UART_DEVICE_EMERGENCY_STOP) {
        return;
    }
    ClearControllerEventDedupIfHealthy(state, error);
    const std::string error_code = state == UART_DEVICE_EMERGENCY_STOP ? std::string{} : UartErrorCode(error);
    EmitStatus(DeviceStateName(state), error_code.empty() ? std::nullopt : std::optional<std::string>{ error_code });
    if (!error_code.empty()) {
        EmitError(error_code, state == UART_DEVICE_EMERGENCY_STOP ? "CRITICAL" : "ERROR", DeviceStateName(state),
                  "sorting controller reported a device error");
    }
}

void SortingNode::HandleSafetyEvent(const uart_frame_t& frame) noexcept {
    if (UART_IS_VALID_APP_SAFETY_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint8_t kind = frame.payload[APP_SAFETY_EVENT_KIND_INDEX];
    const std::uint8_t cause = frame.payload[APP_SAFETY_EVENT_CAUSE_INDEX];
    const std::uint8_t result = frame.payload[APP_SAFETY_EVENT_RESULT_INDEX];
    if (pending_safety_.active) {
        const bool estopConfirmed =
            pending_safety_.expected == PendingSafetyEvent::kEstopLatched && kind == SAFETY_EVENT_ESTOP_LATCHED;
        const bool resetConfirmed =
            pending_safety_.expected == PendingSafetyEvent::kResetComplete && kind == SAFETY_EVENT_RESET_COMPLETE;
        const bool resetRejected =
            pending_safety_.expected == PendingSafetyEvent::kResetComplete && kind == SAFETY_EVENT_RESET_REJECTED;
        if (estopConfirmed || resetConfirmed || resetRejected) {
            EmitSafetyResponse(resetRejected ? mqtt::CommandResult::kRejected : mqtt::CommandResult::kSuccess,
                               resetRejected ? std::optional<std::string>{ "ERR-SAFETY-RESET-REJECTED" } : std::nullopt,
                               resetRejected
                                   ? "sorting controller rejected safety recovery result " + std::to_string(result)
                                   : "sorting controller confirmed the safety command");
            pending_safety_ = {};
        }
    }
    if (IsRepeatedControllerEvent(APP_EVENT_SAFETY, kind, cause, result)) {
        return;
    }
    if (kind == SAFETY_EVENT_ESTOP_LATCHED) {
        last_device_state_ = UART_DEVICE_EMERGENCY_STOP;
        last_device_error_ = UART_ERROR_NONE;
        EmitStatus("EMERGENCY_STOP");
    } else if (kind == SAFETY_EVENT_RESET_COMPLETE) {
        ClearActiveCycle();
        configured_speed_ = 0U;
        last_device_state_ = UART_DEVICE_STOPPED;
        last_device_error_ = UART_ERROR_NONE;
        EmitStatus("STOPPED");
    } else if (kind == SAFETY_EVENT_RESET_REJECTED) {
        EmitError("ERR-SAFETY-RESET-REJECTED", "ERROR", "EMERGENCY_STOP",
                  "sorting controller rejected safety reset result " + std::to_string(result));
    }
}

void SortingNode::HandleHealthEvent(const uart_frame_t& frame) noexcept {
    if (UART_IS_VALID_APP_HEALTH_PAYLOAD(frame.payload, frame.length) == 0U) {
        return;
    }
    const std::uint8_t kind = frame.payload[APP_HEALTH_EVENT_KIND_INDEX];
    const std::uint8_t cause = frame.payload[APP_HEALTH_EVENT_CAUSE_INDEX];
    const std::uint8_t sensor_id = frame.payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX];
    // UART timeout events are delivered to the opposite healthy channel.
    // Reporting one here would incorrectly mark this sorting Pi as faulty;
    // MQTT heartbeat expiry owns the affected node's offline state.
    if (kind == HEALTH_ISSUE_UART_CHANNEL_TIMEOUT) {
        return;
    }
    if (IsRepeatedControllerEvent(APP_EVENT_HEALTH, kind, cause, sensor_id)) {
        return;
    }

    std::string error_code;
    std::string message;
    switch (kind) {
        case HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT:
            error_code = "ERR-HEALTH-QUEUE-OVERFLOW";
            message = "sorting controller reported a transient queue overflow";
            break;
        case HEALTH_ISSUE_SENSOR_STALE:
            error_code = "ERR-HEALTH-SENSOR-STALE";
            message = "sorting controller reported a stale sensor";
            // cause only says which Pi channel (INPUT/SORTING); on the sorting side 3
            // sensors (US2/US3/US4) share that channel, so sensor_id is what actually
            // tells them apart.
            if (sensor_id != HEALTH_ISSUE_SENSOR_ID_NONE) {
                message += " sensorId=" + std::to_string(sensor_id);
            }
            break;
        case HEALTH_ISSUE_UART_RECOVERY:
            // Recovery started, not a confirmed loss - the frame is retried, and an
            // exhausted retry surfaces separately as a queue overflow. Still WARNING:
            // a line that keeps needing recovery is the precursor to those drops.
            error_code = "ERR-HEALTH-UART-RECOVERY";
            message = "sorting controller recovered from a UART error";
            break;
        default:
            error_code = "ERR-HEALTH-EVENT-" + std::to_string(kind);
            message = "sorting controller reported an unknown health event";
            break;
    }
    message += " (cause=" + std::to_string(cause) + ')';
    EmitError(std::move(error_code), "WARNING", "CONTROLLER_HEALTH", std::move(message));
}

bool SortingNode::IsRepeatedControllerEvent(std::uint8_t event_id, std::uint8_t kind, std::uint8_t cause,
                                            std::uint8_t result) noexcept {
    const std::uint32_t signature = (static_cast<std::uint32_t>(event_id) << 24U) |
                                    (static_cast<std::uint32_t>(kind) << 16U) |
                                    (static_cast<std::uint32_t>(cause) << 8U) | static_cast<std::uint32_t>(result);
    if (last_controller_event_signature_.has_value() && *last_controller_event_signature_ == signature) {
        return true;
    }
    last_controller_event_signature_ = signature;
    return false;
}

void SortingNode::ClearControllerEventDedupIfHealthy(std::uint8_t state, std::uint8_t error) noexcept {
    if (error == UART_ERROR_NONE && state != UART_DEVICE_EMERGENCY_STOP) {
        last_controller_event_signature_.reset();
    }
}

void SortingNode::EmitPendingResponse(mqtt::CommandResult result, std::optional<std::string> error_code,
                                      std::string message) const noexcept {
    if (!pending_.active || pending_.request_id.empty() || pending_.mqtt_command == mqtt::ControlCommand::kUnknown) {
        return;
    }
    EmitReport({
        .channel = SortingReportChannel::kResponse,
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

void SortingNode::EmitSafetyResponse(mqtt::CommandResult result, std::optional<std::string> error_code,
                                     std::string message) const noexcept {
    if (!pending_safety_.active || pending_safety_.request_id.empty() ||
        pending_safety_.command == mqtt::ControlCommand::kUnknown) {
        return;
    }
    EmitReport({
        .channel = SortingReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data = mqtt::CommandResponsePayload{ .request_id = pending_safety_.request_id,
                                              .command = pending_safety_.command,
                                              .result = result,
                                              .error_code = std::move(error_code),
                                              .message = std::move(message) },
    });
}

void SortingNode::EmitStatus(std::string current_state, std::optional<std::string> error_code) const noexcept {
    EmitReport({
        .channel = SortingReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = error_code.has_value() ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .job_id = active_work_id_.empty() ? std::nullopt : std::optional<std::string>{ active_work_id_ },
                .error_code = std::move(error_code),
            },
    });
}

void SortingNode::EmitError(std::string error_code, std::string level, std::string current_state, std::string message,
                            std::optional<std::int32_t> distance) const noexcept {
    EmitReport({
        .channel = SortingReportChannel::kError,
        .message_type = mqtt::MessageType::kErrorOccurred,
        .data =
            mqtt::ErrorOccurredPayload{
                .job_id = active_work_id_.empty() ? std::nullopt : std::optional<std::string>{ active_work_id_ },
                .error_code = std::move(error_code),
                .error_level = std::move(level),
                .current_state = std::move(current_state),
                .message = std::move(message),
                .distance = distance,
            },
    });
}

void SortingNode::EmitReport(SortingReport report) const noexcept {
    if (!report_handler_) {
        return;
    }
    try {
        report_handler_(report);
    } catch (...) {
        // Reporting failures must not unwind into the UART receive loop.
    }
}

}  // namespace logistics::device
