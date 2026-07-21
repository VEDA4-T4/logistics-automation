#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

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
    std::optional<std::string> barcode;
};

struct AssignedVisionWork final {
    std::string work_id;
    VisionObservation observation;
};

class VisionMqttWorkflow final {
public:
    explicit VisionMqttWorkflow(std::string device_id, std::size_t detection_confirm_frames = 3,
                                std::size_t clear_confirm_frames = 5);

    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> Observe(std::optional<VisionObservation> observation,
                                                                      std::string message_id, std::string timestamp);
    [[nodiscard]] bool AssignWork(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] std::optional<AssignedVisionWork> TakeAssignedWork();
    void CompleteWork();

private:
    enum class Phase { kIdle, kAwaitingWork, kAssigned, kProcessing, kAwaitingClear };

    std::string device_id_;
    std::size_t detection_confirm_frames_;
    std::size_t clear_confirm_frames_;
    std::mutex mutex_;
    Phase phase_{ Phase::kIdle };
    std::size_t detected_frames_{};
    std::size_t clear_frames_{};
    std::optional<VisionObservation> observation_;
    std::optional<std::string> work_id_;
};

[[nodiscard]] contracts::mqtt::MqttMessage MakePositionDetectedMessage(std::string_view device_id,
                                                                       const AssignedVisionWork& work,
                                                                       std::string message_id, std::string timestamp);
[[nodiscard]] contracts::mqtt::MqttMessage MakeBarcodeDetectedMessage(std::string_view device_id,
                                                                      const AssignedVisionWork& work,
                                                                      std::string message_id, std::string timestamp);

}  // namespace logistics::vision
