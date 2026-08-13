#include "logistics/central_server/sensor_detection.hpp"

#include <utility>

namespace logistics::central_server {

SensorDetector::SensorDetector(SensorDetectionConfig config) noexcept : config_(std::move(config)) {}

void SensorDetector::Reset() noexcept {
    channels_.clear();
}

std::optional<std::string> SensorDetector::Evaluate(std::string_view device_id, std::int32_t sensor_id,
                                                    std::string_view measurement_status, std::int32_t distance_cm) {
    if (!config_.enabled) {
        return std::nullopt;
    }

    // Detection is per physical sensor, so the key has to carry the device too:
    // the input node and the sorting node both number their sensors from 1.
    auto& channel = channels_[std::string(device_id) + "/" + std::to_string(sensor_id)];

    if (measurement_status == "FAULT" || distance_cm == kDistanceUnknown) {
        channel = ChannelState{};
        return std::string(kDetectionUnknown);
    }

    const bool candidate =
        channel.detected ? distance_cm <= config_.exit_threshold_cm : distance_cm <= config_.enter_threshold_cm;

    if (channel.primed && candidate == channel.candidate) {
        if (channel.matches < config_.debounce_count) {
            ++channel.matches;
        }
    } else {
        channel.primed = true;
        channel.candidate = candidate;
        channel.matches = 1;
    }

    if (channel.matches >= config_.debounce_count) {
        channel.detected = candidate;
    }

    return std::string(channel.detected ? kDetectionDetected : kDetectionClear);
}

}  // namespace logistics::central_server
