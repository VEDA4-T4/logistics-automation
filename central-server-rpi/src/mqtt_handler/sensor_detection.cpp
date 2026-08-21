#include "logistics/central_server/sensor_detection.hpp"

#include <algorithm>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"

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

InputDetectionGate::InputDetectionGate(std::string input_device_id, const std::int32_t sensor_id)
    : input_device_id_(std::move(input_device_id)), sensor_id_(sensor_id) {}

bool InputDetectionGate::ShouldStopConveyor(const contracts::mqtt::MqttMessage& message) {
    const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
    if (sensor == nullptr || message.source_id != input_device_id_ || sensor->sensor_id != sensor_id_ ||
        !sensor->detection_status.has_value()) {
        return false;
    }
    if (*sensor->detection_status != kDetectionDetected) {
        stop_consumed_ = false;
        return false;
    }
    if (stop_consumed_) {
        return false;
    }
    stop_consumed_ = true;
    return true;
}

bool InputDetectionGate::ShouldCreateWork(const contracts::mqtt::MqttMessage& message, const bool process_accepts_work,
                                          const bool input_station_occupied) {
    const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
    if (sensor == nullptr || message.source_id != input_device_id_ || sensor->sensor_id != sensor_id_ ||
        !sensor->detection_status.has_value()) {
        return false;
    }
    if (*sensor->detection_status != kDetectionDetected) {
        consumed_ = false;
        return false;
    }
    if (consumed_) {
        return false;
    }
    if (input_station_occupied) {
        consumed_ = true;
        return false;
    }
    if (!process_accepts_work) {
        return false;
    }
    consumed_ = true;
    return true;
}

void InputDetectionGate::Retry() noexcept {
    consumed_ = false;
}

void InputDetectionGate::RetryStop() noexcept {
    stop_consumed_ = false;
}

SortingDetectionGate::SortingDetectionGate(std::string sorting_device_id)
    : sorting_device_id_(std::move(sorting_device_id)) {}

std::optional<std::string> SortingDetectionGate::ShouldStop(const contracts::mqtt::MqttMessage& message,
                                                            const bool process_running,
                                                            const std::vector<WorkProcessSnapshot>& active_works) {
    const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
    if (sensor == nullptr || message.source_id != sorting_device_id_ || !sensor->detection_status.has_value()) {
        return std::nullopt;
    }
    if (*sensor->detection_status == kDetectionClear) {
        if (consumed_sensor_id_ == sensor->sensor_id) {
            consumed_sensor_id_.reset();
        }
        return std::nullopt;
    }
    if (*sensor->detection_status != kDetectionDetected || consumed_sensor_id_.has_value() || !process_running) {
        return std::nullopt;
    }

    const std::string destination = std::to_string(sensor->sensor_id);
    const auto work = std::ranges::find_if(active_works, [&destination](const WorkProcessSnapshot& candidate) {
        return candidate.stage == WorkStage::kSorting && candidate.destination == destination;
    });
    if (work == active_works.end()) {
        return std::nullopt;
    }

    consumed_sensor_id_ = sensor->sensor_id;
    return work->work_id;
}

void SortingDetectionGate::Retry() noexcept {
    consumed_sensor_id_.reset();
}

}  // namespace logistics::central_server
