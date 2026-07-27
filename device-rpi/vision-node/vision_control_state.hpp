#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::vision {

enum class VisionOperatingState {
    kStopped,
    kRunning,
    kEmergencyStop,
    kRecovering,
    kError,
};

struct VisionControlDecision final {
    contracts::mqtt::MqttMessage response;
    bool clear_work{};
};

class VisionControlState final {
public:
    explicit VisionControlState(std::string device_id);

    [[nodiscard]] std::optional<VisionControlDecision> HandleCommand(const contracts::mqtt::MqttMessage& message,
                                                                     std::string response_message_id,
                                                                     std::string timestamp);

    void SetCameraAvailable(bool available);

    [[nodiscard]] bool IsProcessingEnabled() const;
    [[nodiscard]] bool ConsumeCameraResetRequest();
    [[nodiscard]] VisionOperatingState State() const;
    [[nodiscard]] std::string CurrentState() const;

private:
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;

    std::string device_id_;
    mutable std::mutex mutex_;
    VisionOperatingState state_{ VisionOperatingState::kStopped };
    bool camera_available_{};
    bool camera_reset_requested_{};
};

[[nodiscard]] std::string_view ToString(VisionOperatingState state) noexcept;

}  // namespace logistics::vision
