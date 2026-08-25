#include "logistics/central_server/sensor_detection.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <utility>

#include "logistics/contracts/device.hpp"
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
    std::clog << "[server][SENSOR][INFO] input detection consumed; messageId=" << message.message_id
              << "; source=" << message.source_id << "; sensorId=" << sensor->sensor_id
              << "; distanceCm=" << sensor->distance_cm << "; detectionStatus=" << *sensor->detection_status
              << "; decision=STOP_CONVEYOR\n";
    return true;
}

bool InputDetectionGate::ShouldAwaitVision(const contracts::mqtt::MqttMessage& message, const bool process_accepts_work,
                                           const bool has_active_work) {
    const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
    if (sensor == nullptr || message.source_id != input_device_id_ || sensor->sensor_id != sensor_id_ ||
        !sensor->detection_status.has_value()) {
        return false;
    }
    if (*sensor->detection_status == kDetectionClear) {
        if (phase_ == Phase::kWaitingForClear) {
            phase_ = Phase::kWaitingForDetection;
        }
        return false;
    }
    if (*sensor->detection_status != kDetectionDetected) {
        return false;
    }
    if (phase_ != Phase::kWaitingForDetection || !process_accepts_work || has_active_work) {
        return false;
    }
    phase_ = Phase::kWaitingForVision;
    return true;
}

bool InputDetectionGate::WaitingForVision() const noexcept {
    return phase_ == Phase::kWaitingForVision;
}

void InputDetectionGate::MarkWorkCreated() noexcept {
    if (phase_ == Phase::kWaitingForVision) {
        phase_ = Phase::kWaitingForClear;
    }
}

void InputDetectionGate::Reset() noexcept {
    phase_ = Phase::kWaitingForDetection;
    stop_consumed_ = false;
}

void InputDetectionGate::RetryStop() noexcept {
    stop_consumed_ = false;
}

LineTracerLoadGate::LineTracerLoadGate(std::string line_tracer_device_id)
    : line_tracer_device_id_(std::move(line_tracer_device_id)) {}

std::optional<std::string> LineTracerLoadGate::ShouldStop(const contracts::mqtt::MqttMessage& message,
                                                          const bool process_running,
                                                          const std::vector<WorkProcessSnapshot>& active_works) {
    std::erase_if(consumed_work_ids_, [&active_works](const std::string& work_id) {
        return std::ranges::none_of(active_works, [&work_id](const WorkProcessSnapshot& work) {
            return work.work_id == work_id && work.stage == WorkStage::kSorting;
        });
    });
    const auto* status = contracts::mqtt::GetPayload<contracts::mqtt::DeviceStatusPayload>(message);
    std::string current_state = status == nullptr ? std::string{} : status->current_state;
    std::ranges::transform(current_state, current_state.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    if (status == nullptr || message.source_id != line_tracer_device_id_ ||
        status->status != contracts::mqtt::ConnectionState::kOnline || status->error_code.has_value() ||
        !status->job_id.has_value() || !contracts::HasStateSuffix(current_state, "LOAD_ON_", 'A', 'C') ||
        !process_running || consumed_work_ids_.contains(*status->job_id)) {
        return std::nullopt;
    }
    const auto work = std::ranges::find_if(active_works, [&status](const WorkProcessSnapshot& candidate) {
        return candidate.stage == WorkStage::kSorting && candidate.work_id == *status->job_id;
    });
    if (work == active_works.end()) {
        return std::nullopt;
    }

    consumed_work_ids_.insert(work->work_id);
    return work->work_id;
}

void LineTracerLoadGate::Retry(const std::string_view work_id) {
    consumed_work_ids_.erase(std::string(work_id));
}

}  // namespace logistics::central_server
