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
    bool preserve_work{};
    bool state_changed{};
};

class DeviceControlState final {
public:
    explicit DeviceControlState(DeviceControlConfig config);

    [[nodiscard]] std::optional<DeviceControlDecision> HandleCommand(const contracts::mqtt::MqttMessage& message,
                                                                     std::string response_message_id,
                                                                     std::string timestamp);
    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> CompleteRecovery(std::string response_message_id,
                                                                               std::string timestamp);

    void SetReady(bool ready);
    void SetFault();

    [[nodiscard]] bool IsOperational() const;
    [[nodiscard]] bool ConsumeResetRequest(bool* preserve_work = nullptr);
    [[nodiscard]] DeviceOperatingState State() const;
    [[nodiscard]] std::string CurrentState() const;

private:
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;
    [[nodiscard]] contracts::mqtt::MqttMessage MakeResponse(std::string_view request_id,
                                                            contracts::mqtt::ControlCommand command,
                                                            contracts::mqtt::CommandResult result,
                                                            std::optional<std::string> error_code, std::string message,
                                                            std::string response_message_id,
                                                            std::string timestamp) const;

    DeviceControlConfig config_;
    mutable std::mutex mutex_;
    DeviceOperatingState state_{ DeviceOperatingState::kStopped };
    bool ready_{};
    bool reset_requested_{};
    bool preserve_work_on_reset_{};
    std::optional<std::string> pending_recovery_request_id_;
};

[[nodiscard]] std::string_view ToString(DeviceOperatingState state) noexcept;

}  // namespace logistics::device
