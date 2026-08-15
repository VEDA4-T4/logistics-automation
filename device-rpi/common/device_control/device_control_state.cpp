#include "logistics/device/device_control_state.hpp"

#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/device/device_control_policy.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

}  // namespace

std::string_view ToString(const DeviceOperatingState state) noexcept {
    switch (state) {
        case DeviceOperatingState::kStopped:
            return "STOPPED";
        case DeviceOperatingState::kRunning:
            return "RUNNING";
        case DeviceOperatingState::kEmergencyStop:
            return "EMERGENCY_STOP";
        case DeviceOperatingState::kRecovering:
            return "RECOVERY";
        case DeviceOperatingState::kError:
            return "ERROR";
    }
    return "ERROR";
}

DeviceControlState::DeviceControlState(DeviceControlConfig config) : config_(std::move(config)) {
    if (!mqtt::IsValidTopicLevel(config_.device_id) || config_.component_name.empty() ||
        config_.not_ready_error_code.empty()) {
        throw std::invalid_argument("invalid device control configuration");
    }
}

std::optional<DeviceControlDecision> DeviceControlState::HandleCommand(const mqtt::MqttMessage& message,
                                                                       std::string response_message_id,
                                                                       std::string timestamp) {
    const auto request = ReadDeviceControlRequest(message);
    if (!request.has_value()) {
        return std::nullopt;
    }

    mqtt::CommandResult result = mqtt::CommandResult::kSuccess;
    std::optional<std::string> error_code;
    std::string response_text;
    bool clear_work = false;
    bool state_changed = false;

    {
        std::lock_guard lock(mutex_);
        if (!IsTargetedToThisNode(request->target_device_id)) {
            result = mqtt::CommandResult::kRejected;
            error_code = "ERR-MQTT-INVALID-TARGET";
            response_text = config_.component_name + " command targets a different device";
        } else {
            switch (ResolveDeviceControlAction(request->command, request->component_id)) {
                case DeviceControlAction::kStart:
                    if (state_ == DeviceOperatingState::kRunning) {
                        response_text = config_.component_name + " is already running";
                    } else if (state_ != DeviceOperatingState::kStopped) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = config_.component_name + " can only start from STOPPED";
                    } else if (!ready_) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = config_.not_ready_error_code;
                        response_text = config_.component_name + " is not ready";
                    } else {
                        state_ = DeviceOperatingState::kRunning;
                        state_changed = true;
                        response_text = config_.component_name + " started";
                    }
                    break;
                case DeviceControlAction::kStop:
                    if (state_ == DeviceOperatingState::kStopped) {
                        response_text = config_.component_name + " is already stopped";
                    } else if (state_ != DeviceOperatingState::kRunning) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = config_.component_name + " cannot stop from the current state";
                    } else {
                        state_ = DeviceOperatingState::kStopped;
                        state_changed = true;
                        response_text = config_.component_name + " stopped";
                    }
                    break;
                case DeviceControlAction::kEmergencyStop:
                    state_changed = state_ != DeviceOperatingState::kEmergencyStop;
                    state_ = DeviceOperatingState::kEmergencyStop;
                    pending_recovery_request_id_.reset();
                    response_text = config_.component_name + " emergency-stopped";
                    break;
                case DeviceControlAction::kSafetyRecovery:
                    if (state_ == DeviceOperatingState::kStopped && ready_) {
                        response_text = config_.component_name + " is already recovered and stopped";
                    } else if (state_ == DeviceOperatingState::kRecovering) {
                        clear_work = true;
                        if (ready_) {
                            state_ = DeviceOperatingState::kStopped;
                            state_changed = true;
                            pending_recovery_request_id_.reset();
                            response_text = config_.component_name + " recovery completed";
                        } else {
                            result = mqtt::CommandResult::kProcessing;
                            pending_recovery_request_id_ = std::string(request->request_id);
                            reset_requested_ = true;
                            response_text = config_.component_name + " recovery is in progress";
                        }
                    } else if (state_ != DeviceOperatingState::kEmergencyStop &&
                               state_ != DeviceOperatingState::kError) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = config_.component_name + " recovery requires ERROR or EMERGENCY_STOP";
                    } else {
                        state_ = DeviceOperatingState::kRecovering;
                        state_changed = true;
                        ready_ = false;
                        reset_requested_ = true;
                        pending_recovery_request_id_ = std::string(request->request_id);
                        clear_work = true;
                        result = mqtt::CommandResult::kProcessing;
                        response_text = config_.component_name + " recovery started";
                    }
                    break;
                case DeviceControlAction::kInitialize:
                    if (state_ == DeviceOperatingState::kStopped) {
                        response_text = config_.component_name + " is already recovered and stopped";
                    } else if (state_ == DeviceOperatingState::kRecovering) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = config_.not_ready_error_code;
                        response_text = config_.component_name + " has not recovered";
                    } else {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-UNSUPPORTED-COMMAND";
                        response_text = config_.component_name + " does not require initialization";
                    }
                    break;
                case DeviceControlAction::kStatusRequest:
                    response_text = config_.component_name + " state is " + std::string(ToString(state_));
                    break;
                case DeviceControlAction::kComponentRecovery:
                case DeviceControlAction::kUnsupported:
                    result = mqtt::CommandResult::kRejected;
                    error_code = "ERR-UNSUPPORTED-COMMAND";
                    response_text = config_.component_name + " command is not supported";
                    break;
            }
        }
    }

    return DeviceControlDecision{
        .response = MakeResponse(request->request_id, request->command, result, std::move(error_code),
                                 std::move(response_text), std::move(response_message_id), std::move(timestamp)),
        .clear_work = clear_work,
        .state_changed = state_changed,
    };
}

std::optional<mqtt::MqttMessage> DeviceControlState::CompleteRecovery(std::string response_message_id,
                                                                      std::string timestamp) {
    std::lock_guard lock(mutex_);
    if (state_ != DeviceOperatingState::kRecovering || !ready_ || !pending_recovery_request_id_.has_value()) {
        return std::nullopt;
    }

    auto response = MakeResponse(
        *pending_recovery_request_id_, mqtt::ControlCommand::kRecovery, mqtt::CommandResult::kSuccess, std::nullopt,
        config_.component_name + " recovery completed", std::move(response_message_id), std::move(timestamp));
    state_ = DeviceOperatingState::kStopped;
    pending_recovery_request_id_.reset();
    return response;
}

void DeviceControlState::SetReady(const bool ready) {
    std::lock_guard lock(mutex_);
    ready_ = ready;
    if (!ready && (state_ == DeviceOperatingState::kRunning || state_ == DeviceOperatingState::kStopped)) {
        state_ = DeviceOperatingState::kError;
    }
}

void DeviceControlState::SetFault() {
    std::lock_guard lock(mutex_);
    if (state_ == DeviceOperatingState::kRunning) {
        state_ = DeviceOperatingState::kError;
        pending_recovery_request_id_.reset();
    }
}

bool DeviceControlState::IsOperational() const {
    std::lock_guard lock(mutex_);
    return state_ == DeviceOperatingState::kRunning && ready_;
}

bool DeviceControlState::ConsumeResetRequest() {
    std::lock_guard lock(mutex_);
    const bool requested = reset_requested_;
    reset_requested_ = false;
    return requested;
}

DeviceOperatingState DeviceControlState::State() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::string DeviceControlState::CurrentState() const {
    std::lock_guard lock(mutex_);
    return std::string(ToString(state_));
}

bool DeviceControlState::IsTargetedToThisNode(const std::string_view target_device_id) const noexcept {
    return IsControlTargetForDevice(target_device_id, config_.device_id);
}

mqtt::MqttMessage DeviceControlState::MakeResponse(std::string_view request_id, const mqtt::ControlCommand command,
                                                   const mqtt::CommandResult result,
                                                   std::optional<std::string> error_code, std::string message,
                                                   std::string response_message_id, std::string timestamp) const {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(response_message_id),
        .message_type = mqtt::MessageType::kCommandResponse,
        .source_id = config_.device_id,
        .timestamp = std::move(timestamp),
        .data =
            mqtt::CommandResponsePayload{
                .request_id = std::string(request_id),
                .command = command,
                .result = result,
                .error_code = std::move(error_code),
                .message = std::move(message),
            },
    };
}

}  // namespace logistics::device
