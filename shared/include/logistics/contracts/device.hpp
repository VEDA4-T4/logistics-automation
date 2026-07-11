#pragma once

#include <cstdint>
#include <string_view>

namespace logistics::contracts {

enum class DeviceRole : std::uint8_t {
    kInput,
    kCamera,
    kRecognition,
    kSorting,
    kLineTracer,
};

[[nodiscard]] constexpr std::string_view ToString(DeviceRole role) {
    switch (role) {
        case DeviceRole::kInput:
            return "input";
        case DeviceRole::kCamera:
            return "camera";
        case DeviceRole::kRecognition:
            return "recognition";
        case DeviceRole::kSorting:
            return "sorting";
        case DeviceRole::kLineTracer:
            return "linetracer";
    }
    return "unknown";
}

}  // namespace logistics::contracts
