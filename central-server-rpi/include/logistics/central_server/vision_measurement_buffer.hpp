#pragma once

#include <optional>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

// Vision is free-running. Keep the newest complete measurement received
// before the ultrasonic station creates a work item and consume it once.
class VisionMeasurementBuffer final {
public:
    void Store(contracts::mqtt::MqttMessage message) {
        const auto* measurement = contracts::mqtt::GetPayload<contracts::mqtt::VisionMeasurementPayload>(message);
        if (measurement != nullptr && measurement->HasBox()) {
            latest_complete_ = std::move(message);
        }
    }

    [[nodiscard]] bool Empty() const noexcept {
        return !latest_complete_.has_value();
    }

    template <typename Handler>
    [[nodiscard]] bool ReplayWhen(const bool ready, Handler&& handler) {
        if (!ready || !latest_complete_.has_value()) {
            return true;
        }
        auto pending = std::move(latest_complete_);
        latest_complete_.reset();
        if (handler(*pending)) {
            return true;
        }
        latest_complete_ = std::move(*pending);
        return false;
    }

private:
    std::optional<contracts::mqtt::MqttMessage> latest_complete_;
};

}  // namespace logistics::central_server
