#include "vision_mqtt_workflow.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::vision {

namespace mqtt = contracts::mqtt;

VisionMqttWorkflow::VisionMqttWorkflow(std::string device_id, const std::size_t detection_confirm_frames,
                                       const std::size_t clear_confirm_frames,
                                       const std::chrono::milliseconds preassignment_timeout,
                                       const std::chrono::milliseconds barcode_timeout, MonotonicNow monotonic_now)
    : device_id_(std::move(device_id)),
      detection_confirm_frames_(detection_confirm_frames),
      clear_confirm_frames_(clear_confirm_frames),
      preassignment_timeout_(preassignment_timeout),
      barcode_timeout_(barcode_timeout),
      monotonic_now_(std::move(monotonic_now)) {
    if (!mqtt::IsValidTopicLevel(device_id_) || detection_confirm_frames_ == 0 || clear_confirm_frames_ == 0 ||
        preassignment_timeout_ <= std::chrono::milliseconds::zero() ||
        barcode_timeout_ <= std::chrono::milliseconds::zero() || !monotonic_now_) {
        throw std::invalid_argument("invalid vision MQTT workflow configuration");
    }
}

void VisionMqttWorkflow::Observe(std::optional<VisionObservation> observation) {
    std::lock_guard lock(mutex_);
    const TimePoint now = monotonic_now_();
    ExpirePreassignment(now);

    const bool barcode_deadline_expired =
        phase_ == Phase::kAssigned && barcode_deadline_.has_value() && now >= *barcode_deadline_;
    const bool confirming_box = observation.has_value() && observation->box_detected &&
                                (phase_ == Phase::kIdle || phase_ == Phase::kPreassigned);
    const bool awaiting_barcode_for_confirmed_box =
        confirmed_box_observation_.has_value() && (phase_ == Phase::kAwaitingWork || phase_ == Phase::kAssigned);
    if (observation.has_value() && !barcode_deadline_expired &&
        (confirming_box || awaiting_barcode_for_confirmed_box)) {
        if (observation->barcode.has_value()) {
            barcode_ = std::move(observation->barcode);
        }
        barcode_region_detected_ = barcode_region_detected_ || observation->barcode_region_detected;
    }

    if (!observation.has_value() || !observation->box_detected) {
        detected_frames_ = 0;
        box_candidate_.reset();
        if (phase_ == Phase::kIdle) {
            confirmed_box_observation_.reset();
            barcode_.reset();
            barcode_region_detected_ = false;
        }
        if (phase_ == Phase::kAwaitingClear && ++clear_frames_ >= clear_confirm_frames_) {
            phase_ = Phase::kIdle;
            clear_frames_ = 0;
            confirmed_box_observation_.reset();
            barcode_.reset();
            barcode_region_detected_ = false;
            work_id_.reset();
        }
        return;
    }

    clear_frames_ = 0;
    if (phase_ != Phase::kIdle && phase_ != Phase::kPreassigned) {
        return;
    }
    box_candidate_ = std::move(observation);
    box_candidate_->barcode.reset();
    box_candidate_->barcode_region_detected = false;
    if (++detected_frames_ < detection_confirm_frames_) {
        return;
    }

    detected_frames_ = 0;
    confirmed_box_observation_ = std::move(box_candidate_);
    if (work_id_.has_value()) {
        preassignment_deadline_.reset();
        barcode_deadline_ = now + barcode_timeout_;
        phase_ = Phase::kAssigned;
        return;
    }
    phase_ = Phase::kAwaitingWork;
}

bool VisionMqttWorkflow::AssignWork(const mqtt::MqttMessage& message) {
    const auto* work = mqtt::GetPayload<mqtt::WorkCreatedPayload>(message);
    std::lock_guard lock(mutex_);
    if (work == nullptr || !work->IsValid() ||
        (last_completed_work_id_.has_value() && work->work_id == *last_completed_work_id_)) {
        return false;
    }
    if (work_id_.has_value() && *work_id_ == work->work_id) {
        return phase_ != Phase::kIdle && phase_ != Phase::kAwaitingClear;
    }
    if ((phase_ != Phase::kIdle && phase_ != Phase::kAwaitingWork) || work_id_.has_value()) {
        return false;
    }
    work_id_ = work->work_id;
    if (phase_ == Phase::kAwaitingWork) {
        barcode_deadline_ = monotonic_now_() + barcode_timeout_;
        phase_ = Phase::kAssigned;
    } else {
        preassignment_deadline_ = monotonic_now_() + preassignment_timeout_;
        phase_ = Phase::kPreassigned;
    }
    return true;
}

bool VisionMqttWorkflow::HasPendingBarcode() const {
    std::lock_guard lock(mutex_);
    return (phase_ == Phase::kAwaitingWork || phase_ == Phase::kAssigned) && confirmed_box_observation_.has_value() &&
           barcode_.has_value();
}

bool VisionMqttWorkflow::NeedsBarcodeFallback() const {
    std::lock_guard lock(mutex_);
    return (phase_ == Phase::kAwaitingWork || phase_ == Phase::kAssigned) && confirmed_box_observation_.has_value() &&
           !barcode_.has_value();
}

std::optional<AssignedVisionWork> VisionMqttWorkflow::TakeAssignedWork() {
    std::lock_guard lock(mutex_);
    ExpirePreassignment(monotonic_now_());
    if (phase_ != Phase::kAssigned || !work_id_.has_value()) {
        return std::nullopt;
    }
    if (confirmed_box_observation_.has_value() && !barcode_.has_value() && barcode_deadline_.has_value() &&
        monotonic_now_() < *barcode_deadline_) {
        return std::nullopt;
    }
    phase_ = Phase::kProcessing;
    auto observation = confirmed_box_observation_;
    if (observation.has_value()) {
        observation->barcode = barcode_;
        observation->barcode_region_detected = barcode_region_detected_;
    }
    return AssignedVisionWork{ .work_id = *work_id_, .observation = std::move(observation) };
}

void VisionMqttWorkflow::ExpirePreassignment(const TimePoint now) {
    if (phase_ != Phase::kPreassigned || !preassignment_deadline_.has_value() || now < *preassignment_deadline_) {
        return;
    }
    detected_frames_ = 0;
    box_candidate_.reset();
    confirmed_box_observation_.reset();
    preassignment_deadline_.reset();
    phase_ = Phase::kAssigned;
}

void VisionMqttWorkflow::CancelPendingWork() {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kAwaitingWork) {
        phase_ = Phase::kIdle;
        detected_frames_ = 0;
        preassignment_deadline_.reset();
        barcode_deadline_.reset();
        box_candidate_.reset();
        confirmed_box_observation_.reset();
        barcode_.reset();
        barcode_region_detected_ = false;
        work_id_.reset();
    }
}

void VisionMqttWorkflow::CompleteWork() {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::kProcessing || phase_ == Phase::kAssigned) {
        last_completed_work_id_ = work_id_;
        phase_ = Phase::kAwaitingClear;
        clear_frames_ = 0;
        preassignment_deadline_.reset();
        barcode_deadline_.reset();
    }
}

bool VisionResultOutbox::Enqueue(std::string work_id, std::vector<VisionPublication> publications) {
    std::lock_guard lock(mutex_);
    if (!contracts::IsValidUuid(work_id) || publications.empty() || !publications_.empty()) {
        return false;
    }
    work_id_ = std::move(work_id);
    publications_ = std::move(publications);
    next_publication_ = 0;
    return true;
}

bool VisionResultOutbox::Flush(const Publisher& event_publisher, const Publisher& error_publisher) {
    while (true) {
        VisionPublication publication;
        std::string work_id;
        std::size_t publication_index{};
        {
            std::lock_guard lock(mutex_);
            if (publications_.empty()) {
                return false;
            }
            publication = publications_[next_publication_];
            work_id = work_id_;
            publication_index = next_publication_;
        }
        const bool published = publication.channel == VisionPublicationChannel::kEvent
                                   ? event_publisher(publication.message)
                                   : error_publisher(publication.message);
        if (!published) {
            return false;
        }
        std::lock_guard lock(mutex_);
        if (publications_.empty() || work_id_ != work_id || next_publication_ != publication_index) {
            return false;
        }
        if (++next_publication_ < publications_.size()) {
            continue;
        }
        work_id_.clear();
        publications_.clear();
        next_publication_ = 0;
        return true;
    }
}

std::optional<std::string> VisionResultOutbox::PendingWorkId() const {
    std::lock_guard lock(mutex_);
    return work_id_.empty() ? std::nullopt : std::optional<std::string>(work_id_);
}

void VisionResultOutbox::Reset() {
    std::lock_guard lock(mutex_);
    work_id_.clear();
    publications_.clear();
    next_publication_ = 0;
}

void VisionMqttWorkflow::Reset() {
    std::lock_guard lock(mutex_);
    phase_ = Phase::kIdle;
    detected_frames_ = 0;
    clear_frames_ = 0;
    preassignment_deadline_.reset();
    barcode_deadline_.reset();
    box_candidate_.reset();
    confirmed_box_observation_.reset();
    barcode_.reset();
    barcode_region_detected_ = false;
    work_id_.reset();
}

mqtt::MqttMessage MakePositionDetectedMessage(std::string_view device_id, const AssignedVisionWork& work,
                                              std::string message_id, std::string timestamp) {
    const auto& observation = work.observation.value();
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
                .box_corners = observation.box_corners,
            },
    };
}

mqtt::MqttMessage MakeBarcodeDetectedMessage(std::string_view device_id, const AssignedVisionWork& work,
                                             std::string message_id, std::string timestamp) {
    const bool box_detected = work.observation.has_value();
    const bool detected = box_detected && work.observation->barcode.has_value();
    const bool region_detected = box_detected && work.observation->barcode_region_detected;
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
                .barcode = detected ? *work.observation->barcode : std::string{},
                .confidence = std::nullopt,
                .message = detected ? std::nullopt
                                    : std::optional<std::string>(
                                          !box_detected     ? "box was not detected within the preassignment timeout"
                                          : region_detected ? "barcode region detected but EAN-13 decode failed"
                                                            : "barcode region was not detected"),
                .error_code =
                    detected ? std::nullopt
                             : std::optional<std::string>(!box_detected     ? "ERR-VISION-BOX-NOT-DETECTED"
                                                          : region_detected ? "ERR-VISION-BARCODE-DECODE-FAILED"
                                                                            : "ERR-VISION-BARCODE-REGION-NOT-DETECTED"),
                .failure_stage = detected ? std::nullopt
                                          : std::optional<std::string>(!box_detected     ? "BOX_DETECTION"
                                                                       : region_detected ? "BARCODE_DECODE"
                                                                                         : "BARCODE_DETECTION"),
            },
    };
}

mqtt::MqttMessage MakeVisionMeasurementMessage(std::string_view device_id, const VisionObservation& observation,
                                               std::string message_id, std::string timestamp) {
    if (!observation.box_detected || !observation.barcode.has_value()) {
        throw std::invalid_argument("vision measurement requires a detected box and barcode");
    }
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kVisionMeasurement,
        .source_id = std::string(device_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::VisionMeasurementPayload{
                .barcode = *observation.barcode,
                .box_x = observation.box_x,
                .box_y = observation.box_y,
                .box_width = observation.box_width,
                .box_height = observation.box_height,
                .frame_width = observation.frame_width,
                .frame_height = observation.frame_height,
                .box_corners = observation.box_corners,
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
