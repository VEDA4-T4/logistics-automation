#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace logistics::contracts {

enum class DeviceRole : std::uint8_t {
    kInput,
    kVision,
    kGripper,
    kSorting,
    kLineTracer,
};

enum class DeviceStateMeaning : std::uint8_t {
    kUnknown,
    kIdle,
    kWorking,
    kStopped,
    kError,
    kEmergencyStop,
    kRecovery,
    kCompleted,
};

[[nodiscard]] constexpr std::string_view ToString(DeviceRole role) {
    switch (role) {
        case DeviceRole::kInput:
            return "input";
        case DeviceRole::kVision:
            return "vision";
        case DeviceRole::kGripper:
            return "gripper";
        case DeviceRole::kSorting:
            return "sorting";
        case DeviceRole::kLineTracer:
            return "linetracer";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<DeviceRole> DeviceRoleFromString(std::string_view value) noexcept {
    if (value == "input") {
        return DeviceRole::kInput;
    }
    if (value == "vision") {
        return DeviceRole::kVision;
    }
    if (value == "gripper") {
        return DeviceRole::kGripper;
    }
    if (value == "sorting") {
        return DeviceRole::kSorting;
    }
    if (value == "linetracer") {
        return DeviceRole::kLineTracer;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool HasStateSuffix(std::string_view value, std::string_view prefix, char first,
                                            char last) noexcept {
    return value.size() == prefix.size() + 1U && value.starts_with(prefix) && value.back() >= first &&
           value.back() <= last;
}

[[nodiscard]] constexpr bool IsSensorState(std::string_view value, std::string_view suffix) noexcept {
    constexpr std::string_view prefix = "SENSOR_";
    if (!value.starts_with(prefix) || !value.ends_with(suffix) || value.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    for (const char character : value.substr(prefix.size(), value.size() - prefix.size() - suffix.size())) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

// Unknown states remain valid MQTT 1.0 strings for forward compatibility. Consumers must preserve them for display,
// but must not infer work progress or motion from kUnknown.
[[nodiscard]] constexpr DeviceStateMeaning DeviceStateMeaningFor(DeviceRole role, std::string_view state) noexcept {
    if (state == "EMERGENCY_STOP" || state == "ESTOP") {
        return DeviceStateMeaning::kEmergencyStop;
    }
    if (state == "RECOVERY") {
        return DeviceStateMeaning::kRecovery;
    }
    if (state == "STOPPED" || state == "RECOVERY_READY") {
        return DeviceStateMeaning::kStopped;
    }
    if (state == "ERROR" || state == "FAULT" || state == "DISCONNECTED" || state == "MQTT_DISCONNECTED" ||
        state == "UART_HEARTBEAT_TIMEOUT") {
        return DeviceStateMeaning::kError;
    }
    if (state == "COMPLETED") {
        return DeviceStateMeaning::kCompleted;
    }

    switch (role) {
        case DeviceRole::kInput:
            if (state == "IDLE" || state == "READY" || state == "ONLINE") {
                return DeviceStateMeaning::kIdle;
            }
            if (state == "RUNNING" || state == "BUSY" || state == "CONTROLLER_EVENT" ||
                IsSensorState(state, "_DETECTED")) {
                return DeviceStateMeaning::kWorking;
            }
            if (IsSensorState(state, "_FAULT")) {
                return DeviceStateMeaning::kError;
            }
            if (IsSensorState(state, "_CLEAR")) {
                return DeviceStateMeaning::kIdle;
            }
            break;

        case DeviceRole::kVision:
            if (state == "IDLE" || state == "READY" || state == "ONLINE" || state == "RUNNING" ||
                state == "WAITING_FOR_PRODUCT") {
                return DeviceStateMeaning::kIdle;
            }
            if (state == "AWAITING_WORK_ID" || state == "WORK_ASSIGNED" || state == "VISION_PROCESSING" ||
                state == "UPLOAD_PENDING" || state == "RESULT_PENDING") {
                return DeviceStateMeaning::kWorking;
            }
            if (state == "CAMERA_ERROR" || state == "VISION_ERROR" || state == "UPLOAD_ERROR") {
                return DeviceStateMeaning::kError;
            }
            break;

        case DeviceRole::kGripper:
            if (state == "IDLE" || state == "READY" || state == "ONLINE") {
                return DeviceStateMeaning::kIdle;
            }
            if (state == "WORK_ASSIGNED" || state == "PROCESSING" || state == "PICKING" || state == "TRANSFERRING" ||
                state == "PLACING" || state == "RUNNING") {
                return DeviceStateMeaning::kWorking;
            }
            if (state == "PLACED") {
                return DeviceStateMeaning::kCompleted;
            }
            break;

        case DeviceRole::kSorting:
            if (state == "IDLE" || state == "READY" || state == "ONLINE") {
                return DeviceStateMeaning::kIdle;
            }
            if (state == "RUNNING" || state == "BUSY" || state == "SORTING" || state == "ROUTING" ||
                state == "RETURNING_HOME" || HasStateSuffix(state, "GATE_MOVING_DEST_", '1', '3') ||
                HasStateSuffix(state, "WAITING_ITEM_DEST_", '1', '3')) {
                return DeviceStateMeaning::kWorking;
            }
            if (state == "CYCLE_COMPLETE" || state == "HOME") {
                return DeviceStateMeaning::kCompleted;
            }
            break;

        case DeviceRole::kLineTracer:
            if (state == "IDLE" || state == "READY" || state == "ONLINE" ||
                HasStateSuffix(state, "PARKED_", 'A', 'C')) {
                return DeviceStateMeaning::kIdle;
            }
            if (state == "RUNNING" || state == "MOVING" || state == "DELIVERING" || state == "FOLLOWING_LINE" ||
                state == "CORRECTING" || HasStateSuffix(state, "PICKUP_READY_", 'A', 'C') ||
                HasStateSuffix(state, "ARRIVED_", 'A', 'C') || HasStateSuffix(state, "UNLOADING_", 'A', 'C') ||
                HasStateSuffix(state, "LOAD_ON_", 'A', 'C')) {
                return DeviceStateMeaning::kWorking;
            }
            if (HasStateSuffix(state, "LOAD_OFF_", 'A', 'C')) {
                return DeviceStateMeaning::kCompleted;
            }
            if (state == "POSITION_UNKNOWN") {
                return DeviceStateMeaning::kError;
            }
            break;
    }
    return DeviceStateMeaning::kUnknown;
}

[[nodiscard]] constexpr bool IsKnownDeviceState(DeviceRole role, std::string_view state) noexcept {
    return DeviceStateMeaningFor(role, state) != DeviceStateMeaning::kUnknown;
}

}  // namespace logistics::contracts
