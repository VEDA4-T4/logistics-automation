#pragma once

namespace logistics::control_center {

enum class ProcessControlPhase {
    Unknown,
    Idle,
    Running,
    Stopped,
    Error,
    EmergencyStop,
    Recovering,
    RecoveryReady,
};

class ProcessControlState final {
public:
    void setMqttConnected(bool connected) noexcept {
        mqtt_connected_ = connected;
        if (!connected) {
            command_pending_ = false;
            phase_ = ProcessControlPhase::Unknown;
        }
    }

    void setPhase(ProcessControlPhase phase) noexcept {
        phase_ = phase;
    }

    [[nodiscard]] ProcessControlPhase phase() const noexcept {
        return phase_;
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
    [[nodiscard]] bool startEnabled() const noexcept {
        return normalCommandsEnabled() &&
               (phase_ == ProcessControlPhase::Idle || phase_ == ProcessControlPhase::Stopped);
    }
    [[nodiscard]] bool stopEnabled() const noexcept {
        return normalCommandsEnabled() && phase_ == ProcessControlPhase::Running;
    }
    [[nodiscard]] bool restartEnabled() const noexcept {
        return normalCommandsEnabled() && phase_ == ProcessControlPhase::Stopped;
    }
    [[nodiscard]] bool recoveryEnabled() const noexcept {
        return normalCommandsEnabled() &&
               (phase_ == ProcessControlPhase::Error || phase_ == ProcessControlPhase::EmergencyStop ||
                phase_ == ProcessControlPhase::Recovering);
    }
    [[nodiscard]] bool initializeEnabled() const noexcept {
        return normalCommandsEnabled() && phase_ == ProcessControlPhase::RecoveryReady;
    }
    [[nodiscard]] bool emergencyStopEnabled() const noexcept {
        return mqtt_connected_;
    }

private:
    bool mqtt_connected_{ false };
    bool command_pending_{ false };
    ProcessControlPhase phase_{ ProcessControlPhase::Unknown };
};

}  // namespace logistics::control_center
