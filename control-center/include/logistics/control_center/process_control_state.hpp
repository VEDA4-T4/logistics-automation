#pragma once

namespace logistics::control_center {

class ProcessControlState final {
public:
    void setMqttConnected(bool connected) noexcept {
        mqtt_connected_ = connected;
        if (!connected) {
            command_pending_ = false;
        }
    }

    void setCommandPending() noexcept {
        command_pending_ = true;
    }
    void setCommandFinished() noexcept {
        command_pending_ = false;
    }

    [[nodiscard]] bool isMqttConnected() const noexcept {
        return mqtt_connected_;
    }
    [[nodiscard]] bool isCommandPending() const noexcept {
        return command_pending_;
    }
    [[nodiscard]] bool normalCommandsEnabled() const noexcept {
        return mqtt_connected_ && !command_pending_;
    }
    [[nodiscard]] bool emergencyStopEnabled() const noexcept {
        return mqtt_connected_;
    }

private:
    bool mqtt_connected_{ false };
    bool command_pending_{ false };
};

}  // namespace logistics::control_center
