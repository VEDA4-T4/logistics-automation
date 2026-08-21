#pragma once

#include <optional>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

// Vision is free-running. Keep the newest valid measurement received before
// the ultrasonic station creates a work item and consume it once.
class VisionMeasurementBuffer final {
public:
    void Store(contracts::mqtt::MqttMessage message) {
        latest_ = std::move(message);
    }

    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> Take() {
        auto result = std::move(latest_);
        latest_.reset();
        return result;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return !latest_.has_value();
    }

private:
    std::optional<contracts::mqtt::MqttMessage> latest_;
};

}  // namespace logistics::central_server
