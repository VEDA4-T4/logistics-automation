#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::device {

enum class DeviceOperatingState {
    kStopped,
    kRunning,
    kEmergencyStop,
    kRecovering,
    kError,
};

struct DeviceControlConfig final {
    std::string device_id;
    std::string component_name{ "device" };
    std::string not_ready_error_code{ "ERR-DEVICE-NOT-READY" };
};

struct DeviceControlDecision final {
    contracts::mqtt::MqttMessage response;
    bool clear_work{};
};

class DeviceControlState final {
public:
    explicit DeviceControlState(DeviceControlConfig config);

    [[nodiscard]] std::optional<DeviceControlDecision> HandleCommand(const contracts::mqtt::MqttMessage& message,
                                                                     std::string response_message_id,
                                                                     std::string timestamp);

    void SetReady(bool ready);

    [[nodiscard]] bool IsOperational() const;
    [[nodiscard]] bool ConsumeResetRequest();
    [[nodiscard]] DeviceOperatingState State() const;
    [[nodiscard]] std::string CurrentState() const;

private:
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;

    DeviceControlConfig config_;
    mutable std::mutex mutex_;
    DeviceOperatingState state_{ DeviceOperatingState::kStopped };
    bool ready_{};
    bool reset_requested_{};
};

[[nodiscard]] std::string_view ToString(DeviceOperatingState state) noexcept;

}  // namespace logistics::device
