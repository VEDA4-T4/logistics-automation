#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::vision {

struct VisionObservation final {
    std::string image_name;
    std::int32_t box_x{};
    std::int32_t box_y{};
    std::int32_t box_width{};
    std::int32_t box_height{};
    std::int32_t frame_width{};
    std::int32_t frame_height{};
    std::array<contracts::mqtt::PixelPoint, 4> box_corners{};
    std::optional<std::string> barcode;
    bool barcode_region_detected{};
};

struct AssignedVisionWork final {
    std::string work_id;
    std::optional<VisionObservation> observation;
};

class VisionMqttWorkflow final {
public:
    explicit VisionMqttWorkflow(std::string device_id, std::size_t detection_confirm_frames = 3,
                                std::size_t clear_confirm_frames = 5, std::size_t barcode_wait_frames = 90,
                                std::size_t preassignment_wait_frames = 90);

    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> Observe(std::optional<VisionObservation> observation,
                                                                      std::string message_id, std::string timestamp);
    [[nodiscard]] bool AssignWork(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] bool HasPendingBarcode() const;
    [[nodiscard]] bool NeedsBarcodeFallback() const;
    [[nodiscard]] std::optional<AssignedVisionWork> TakeAssignedWork();
    void CancelPendingWork();
    void CompleteWork();
    void Reset();

private:
    enum class Phase { kIdle, kPreassigned, kAwaitingWork, kAssigned, kProcessing, kAwaitingClear };

    std::string device_id_;
    std::size_t detection_confirm_frames_;
    std::size_t clear_confirm_frames_;
    std::size_t barcode_wait_frames_;
    std::size_t preassignment_wait_frames_;
    mutable std::mutex mutex_;
    Phase phase_{ Phase::kIdle };
    std::size_t detected_frames_{};
    std::size_t clear_frames_{};
    std::size_t assigned_frames_{};
    std::size_t preassignment_frames_{};
    std::optional<VisionObservation> observation_;
    std::optional<std::string> work_id_;
    std::optional<std::string> last_completed_work_id_;
};

enum class VisionPublicationChannel { kEvent, kError };

struct VisionPublication final {
    VisionPublicationChannel channel{ VisionPublicationChannel::kEvent };
    contracts::mqtt::MqttMessage message;
};

class VisionResultOutbox final {
public:
    using Publisher = std::function<bool(const contracts::mqtt::MqttMessage&)>;

    [[nodiscard]] bool Enqueue(std::string work_id, std::vector<VisionPublication> publications);
    [[nodiscard]] bool Flush(const Publisher& event_publisher, const Publisher& error_publisher);
    [[nodiscard]] std::optional<std::string> PendingWorkId() const;
    void Reset();

private:
    mutable std::mutex mutex_;
    std::string work_id_;
    std::vector<VisionPublication> publications_;
    std::size_t next_publication_{};
};

[[nodiscard]] contracts::mqtt::MqttMessage MakePositionDetectedMessage(std::string_view device_id,
                                                                       const AssignedVisionWork& work,
                                                                       std::string message_id, std::string timestamp);
[[nodiscard]] contracts::mqtt::MqttMessage MakeBarcodeDetectedMessage(std::string_view device_id,
                                                                      const AssignedVisionWork& work,
                                                                      std::string message_id, std::string timestamp);
[[nodiscard]] contracts::mqtt::MqttMessage MakeProductImageMessage(std::string_view device_id, std::string_view work_id,
                                                                   std::string_view image_id,
                                                                   std::string_view image_path,
                                                                   std::string_view checksum, std::string message_id,
                                                                   std::string timestamp);

}  // namespace logistics::vision
