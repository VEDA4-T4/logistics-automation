#include "vision_mqtt_workflow.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::vision {

namespace mqtt = contracts::mqtt;

VisionMqttWorkflow::VisionMqttWorkflow(std::string device_id, const std::size_t detection_confirm_frames,
                                       const std::size_t clear_confirm_frames, const std::size_t barcode_wait_frames)
    : device_id_(std::move(device_id)),
      detection_confirm_frames_(detection_confirm_frames),
      clear_confirm_frames_(clear_confirm_frames),
      barcode_wait_frames_(barcode_wait_frames) {
    if (!mqtt::IsValidTopicLevel(device_id_) || detection_confirm_frames_ == 0 || clear_confirm_frames_ == 0 ||
        barcode_wait_frames_ == 0) {
        throw std::invalid_argument("invalid vision MQTT workflow configuration");
    }
}

std::optional<mqtt::MqttMessage> VisionMqttWorkflow::Observe(std::optional<VisionObservation> observation,
                                                             std::string message_id, std::string timestamp) {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kAssigned && assigned_frames_ < barcode_wait_frames_) {
        ++assigned_frames_;
    }
    if (!observation.has_value()) {
        detected_frames_ = 0;
        if (phase_ == Phase::kAwaitingClear && ++clear_frames_ >= clear_confirm_frames_) {
            phase_ = Phase::kIdle;
            clear_frames_ = 0;
            observation_.reset();
            work_id_.reset();
        }
        return std::nullopt;
    }

    clear_frames_ = 0;
    if ((phase_ == Phase::kAwaitingWork || phase_ == Phase::kAssigned) && observation->barcode.has_value()) {
        observation_->barcode = std::move(observation->barcode);
    }
    if (phase_ != Phase::kIdle) {
        return std::nullopt;
    }
    if (++detected_frames_ < detection_confirm_frames_) {
        return std::nullopt;
    }

    detected_frames_ = 0;
    observation_ = std::move(observation);
    phase_ = Phase::kAwaitingWork;
    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kBoxDetected,
        .source_id = device_id_,
        .timestamp = std::move(timestamp),
        .data = mqtt::BoxDetectedPayload{ .detected = true, .image_name = observation_->image_name },
    };
}

bool VisionMqttWorkflow::AssignWork(const mqtt::MqttMessage& message) {
    const auto* work = mqtt::GetPayload<mqtt::WorkCreatedPayload>(message);
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kAwaitingWork || work == nullptr || !work->IsValid()) {
        return false;
    }
    work_id_ = work->work_id;
    assigned_frames_ = 0;
    phase_ = Phase::kAssigned;
    return true;
}

bool VisionMqttWorkflow::HasPendingBarcode() const {
    std::lock_guard lock(mutex_);
    return (phase_ == Phase::kAwaitingWork || phase_ == Phase::kAssigned) && observation_.has_value() &&
           observation_->barcode.has_value();
}

std::optional<AssignedVisionWork> VisionMqttWorkflow::TakeAssignedWork() {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kAssigned || !work_id_.has_value() || !observation_.has_value()) {
        return std::nullopt;
    }
    if (!observation_->barcode.has_value() && assigned_frames_ < barcode_wait_frames_) {
        return std::nullopt;
    }
    phase_ = Phase::kProcessing;
    return AssignedVisionWork{ .work_id = *work_id_, .observation = *observation_ };
}

void VisionMqttWorkflow::CancelPendingWork() {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kAwaitingWork) {
        phase_ = Phase::kIdle;
        detected_frames_ = 0;
        assigned_frames_ = 0;
        observation_.reset();
        work_id_.reset();
    }
}

void VisionMqttWorkflow::CompleteWork() {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kProcessing || phase_ == Phase::kAssigned) {
        phase_ = Phase::kAwaitingClear;
        clear_frames_ = 0;
        assigned_frames_ = 0;
    }
}

mqtt::MqttMessage MakePositionDetectedMessage(std::string_view device_id, const AssignedVisionWork& work,
                                              std::string message_id, std::string timestamp) {
    const auto& observation = work.observation;
    const std::int32_t center_x = observation.box_x + observation.box_width / 2;
    const std::int32_t center_y = observation.box_y + observation.box_height / 2;
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kPositionDetected,
        .source_id = std::string(device_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::PositionDetectedPayload{
                .work_id = work.work_id,
                .box_x = observation.box_x,
                .box_y = observation.box_y,
                .box_width = observation.box_width,
                .box_height = observation.box_height,
                .center_x = center_x,
                .center_y = center_y,
                .offset_x = center_x - observation.frame_width / 2,
                .offset_y = center_y - observation.frame_height / 2,
                .position_status = "DETECTED",
            },
    };
}

mqtt::MqttMessage MakeBarcodeDetectedMessage(std::string_view device_id, const AssignedVisionWork& work,
                                             std::string message_id, std::string timestamp) {
    const bool detected = work.observation.barcode.has_value();
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kBarcodeDetected,
        .source_id = std::string(device_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::BarcodeDetectedPayload{
                .work_id = work.work_id,
                .recognition_status = detected ? "SUCCESS" : "FAILED",
                .barcode = detected ? *work.observation.barcode : std::string{},
                .confidence = std::nullopt,
                .message = detected ? std::nullopt : std::optional<std::string>("EAN-13 barcode was not detected"),
            },
    };
}

mqtt::MqttMessage MakeProductImageMessage(std::string_view device_id, std::string_view work_id,
                                          std::string_view image_id, std::string_view image_path,
                                          std::string_view checksum, std::string message_id, std::string timestamp) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kProductImage,
        .source_id = std::string(device_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::ProductImagePayload{
                .work_id = std::string(work_id),
                .image_id = std::string(image_id),
                .image_url = {},
                .image_path = std::string(image_path),
                .checksum = std::string(checksum),
                .upload_status = "UPLOADED",
            },
    };
}

}  // namespace logistics::vision
