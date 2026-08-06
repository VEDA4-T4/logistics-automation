#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::device {

enum class DeviceControlAction {
    kStart,
    kStop,
    kStatusRequest,
    kInitialize,
    kSafetyRecovery,
    kComponentRecovery,
    kEmergencyStop,
    kUnsupported,
};

struct DeviceControlRequestView final {
    contracts::mqtt::ControlCommand command{ contracts::mqtt::ControlCommand::kUnknown };
    std::string_view request_id;
    std::string_view target_device_id;
    std::string_view component_id;
};

[[nodiscard]] inline std::string NormalizeControlComponent(std::string_view component_id) {
    std::string normalized;
    normalized.reserve(component_id.size());
    for (const unsigned char character : component_id) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return normalized;
}

[[nodiscard]] inline bool IsControlTargetForDevice(std::string_view target_device_id,
                                                   std::string_view device_id) noexcept {
    return target_device_id == device_id || target_device_id == "ALL" || target_device_id == "SYSTEM";
}

[[nodiscard]] inline DeviceControlAction ResolveDeviceControlAction(const contracts::mqtt::ControlCommand command,
                                                                    std::string_view component_id = {}) {
    namespace mqtt = contracts::mqtt;

    switch (command) {
        case mqtt::ControlCommand::kStart:
        case mqtt::ControlCommand::kRestart:
            return DeviceControlAction::kStart;
        case mqtt::ControlCommand::kStop:
            return DeviceControlAction::kStop;
        case mqtt::ControlCommand::kStatusRequest:
            return DeviceControlAction::kStatusRequest;
        case mqtt::ControlCommand::kInitialize:
            return DeviceControlAction::kInitialize;
        case mqtt::ControlCommand::kEmergencyStop:
            return DeviceControlAction::kEmergencyStop;
        case mqtt::ControlCommand::kRecovery:
            return component_id.empty() || NormalizeControlComponent(component_id) == "SAFETY"
                       ? DeviceControlAction::kSafetyRecovery
                       : DeviceControlAction::kComponentRecovery;
        case mqtt::ControlCommand::kDestinationSet:
        case mqtt::ControlCommand::kUnknown:
            return DeviceControlAction::kUnsupported;
    }
    return DeviceControlAction::kUnsupported;
}

[[nodiscard]] inline std::optional<DeviceControlRequestView> ReadDeviceControlRequest(
    const contracts::mqtt::MqttMessage& message) {
    namespace mqtt = contracts::mqtt;

    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message); command != nullptr) {
        return DeviceControlRequestView{
            .command = command->command,
            .request_id = command->request_id,
            .target_device_id = command->target_device_id,
            .component_id = command->component_id,
        };
    }
    if (const auto* emergency = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message); emergency != nullptr) {
        return DeviceControlRequestView{
            .command = emergency->command,
            .request_id = emergency->request_id,
            .target_device_id = emergency->target_device_id,
            .component_id = {},
        };
    }
    return std::nullopt;
}

}  // namespace logistics::device
