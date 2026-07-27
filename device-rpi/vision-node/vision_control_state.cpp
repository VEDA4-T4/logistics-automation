#include "vision_control_state.hpp"

#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::vision {
namespace {

namespace mqtt = contracts::mqtt;

struct CommandRequest final {
    std::string_view request_id;
    mqtt::ControlCommand command{ mqtt::ControlCommand::kUnknown };
    std::string_view target_device_id;
};

[[nodiscard]] std::optional<CommandRequest> ReadCommand(const mqtt::MqttMessage& message) {
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return CommandRequest{
            .request_id = command->request_id,
            .command = command->command,
            .target_device_id = command->target_device_id,
        };
    }
    if (const auto* emergency_stop = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        return CommandRequest{
            .request_id = emergency_stop->request_id,
            .command = emergency_stop->command,
            .target_device_id = emergency_stop->target_device_id,
        };
    }
    return std::nullopt;
}

}  // namespace

std::string_view ToString(const VisionOperatingState state) noexcept {
    switch (state) {
        case VisionOperatingState::kStopped:
            return "STOPPED";
        case VisionOperatingState::kRunning:
            return "RUNNING";
        case VisionOperatingState::kEmergencyStop:
            return "EMERGENCY_STOP";
        case VisionOperatingState::kRecovering:
            return "RECOVERY";
        case VisionOperatingState::kError:
            return "ERROR";
    }
    return "ERROR";
}

VisionControlState::VisionControlState(std::string device_id) : device_id_(std::move(device_id)) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("invalid vision device ID");
    }
}

std::optional<VisionControlDecision> VisionControlState::HandleCommand(const mqtt::MqttMessage& message,
                                                                       std::string response_message_id,
                                                                       std::string timestamp) {
    const auto request = ReadCommand(message);
    if (!request.has_value()) {
        return std::nullopt;
    }

    mqtt::CommandResult result = mqtt::CommandResult::kSuccess;
    std::optional<std::string> error_code;
    std::string response_text;
    bool clear_work = false;

    {
        std::lock_guard lock(mutex_);
        if (!IsTargetedToThisNode(request->target_device_id)) {
            result = mqtt::CommandResult::kRejected;
            error_code = "ERR-MQTT-INVALID-TARGET";
            response_text = "vision command targets a different device";
        } else {
            switch (request->command) {
                case mqtt::ControlCommand::kStart:
                case mqtt::ControlCommand::kRestart:
                    if (state_ == VisionOperatingState::kRunning) {
                        response_text = "vision processing is already running";
                    } else if (state_ != VisionOperatingState::kStopped) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = "vision processing can only start from STOPPED";
                    } else if (!camera_available_) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-CAMERA-UNAVAILABLE";
                        response_text = "vision camera is not available";
                    } else {
                        state_ = VisionOperatingState::kRunning;
                        response_text = "vision processing started";
                    }
                    break;
                case mqtt::ControlCommand::kStop:
                    if (state_ == VisionOperatingState::kStopped) {
                        response_text = "vision processing is already stopped";
                    } else if (state_ != VisionOperatingState::kRunning) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = "vision processing cannot stop from the current state";
                    } else {
                        state_ = VisionOperatingState::kStopped;
                        clear_work = true;
                        response_text = "vision processing stopped";
                    }
                    break;
                case mqtt::ControlCommand::kEmergencyStop:
                    state_ = VisionOperatingState::kEmergencyStop;
                    clear_work = true;
                    response_text = "vision processing emergency-stopped";
                    break;
                case mqtt::ControlCommand::kRecovery:
                    if (state_ == VisionOperatingState::kRecovering) {
                        response_text = "vision recovery is already in progress";
                    } else if (state_ != VisionOperatingState::kEmergencyStop &&
                               state_ != VisionOperatingState::kError) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = "vision recovery requires ERROR or EMERGENCY_STOP";
                    } else {
                        state_ = VisionOperatingState::kRecovering;
                        camera_available_ = false;
                        camera_reset_requested_ = true;
                        clear_work = true;
                        response_text = "vision recovery started";
                    }
                    break;
                case mqtt::ControlCommand::kInitialize:
                    if (state_ == VisionOperatingState::kStopped) {
                        response_text = "vision processing is already initialized";
                    } else if (state_ != VisionOperatingState::kRecovering) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-INVALID-STATE";
                        response_text = "vision initialization requires RECOVERY";
                    } else if (!camera_available_) {
                        result = mqtt::CommandResult::kRejected;
                        error_code = "ERR-CAMERA-UNAVAILABLE";
                        response_text = "vision camera has not recovered";
                    } else {
                        state_ = VisionOperatingState::kStopped;
                        clear_work = true;
                        response_text = "vision processing initialized";
                    }
                    break;
                case mqtt::ControlCommand::kStatusRequest:
                    response_text = "vision state is " + std::string(ToString(state_));
                    break;
                case mqtt::ControlCommand::kDestinationSet:
                case mqtt::ControlCommand::kUnknown:
                    result = mqtt::CommandResult::kRejected;
                    error_code = "ERR-UNSUPPORTED-COMMAND";
                    response_text = "vision command is not supported";
                    break;
            }
        }
    }

    return VisionControlDecision{
        .response =
            mqtt::MqttMessage{
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = std::move(response_message_id),
                .message_type = mqtt::MessageType::kCommandResponse,
                .source_id = device_id_,
                .timestamp = std::move(timestamp),
                .data =
                    mqtt::CommandResponsePayload{
                        .request_id = std::string(request->request_id),
                        .command = request->command,
                        .result = result,
                        .error_code = std::move(error_code),
                        .message = std::move(response_text),
                    },
            },
        .clear_work = clear_work,
    };
}

void VisionControlState::SetCameraAvailable(const bool available) {
    std::lock_guard lock(mutex_);
    camera_available_ = available;
    if (!available && state_ == VisionOperatingState::kRunning) {
        state_ = VisionOperatingState::kError;
    }
}

bool VisionControlState::IsProcessingEnabled() const {
    std::lock_guard lock(mutex_);
    return state_ == VisionOperatingState::kRunning && camera_available_;
}

bool VisionControlState::ConsumeCameraResetRequest() {
    std::lock_guard lock(mutex_);
    const bool requested = camera_reset_requested_;
    camera_reset_requested_ = false;
    return requested;
}

VisionOperatingState VisionControlState::State() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::string VisionControlState::CurrentState() const {
    std::lock_guard lock(mutex_);
    return std::string(ToString(state_));
}

bool VisionControlState::IsTargetedToThisNode(const std::string_view target_device_id) const noexcept {
    return target_device_id == device_id_ || target_device_id == "ALL" || target_device_id == "SYSTEM";
}

}  // namespace logistics::vision
