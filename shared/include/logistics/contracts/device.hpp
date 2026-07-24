#pragma once

#include <cstdint>
#include <string_view>

namespace logistics::contracts {

enum class DeviceRole : std::uint8_t {
    kInput,
    kVision,
    kGripper,
    kSorting,
    kLineTracer,
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

}  // namespace logistics::contracts
