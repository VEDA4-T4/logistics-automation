#pragma once

#include <cstdint>
#include <string_view>

namespace logistics::contracts {

    enum class ProcessState : std::uint8_t {
        kIdle,
        kRunning,
        kStopped,
        kError,
        kEmergencyStop,
        kRecovery,
    };

    [[nodiscard]] constexpr std::string_view ToString(ProcessState state) {
        switch (state) {
        case ProcessState::kIdle:
            return "IDLE";
        case ProcessState::kRunning:
            return "RUNNING";
        case ProcessState::kStopped:
            return "STOPPED";
        case ProcessState::kError:
            return "ERROR";
        case ProcessState::kEmergencyStop:
            return "ESTOP";
        case ProcessState::kRecovery:
            return "RECOVERY";
        }
        return "UNKNOWN";
    }

}  // namespace logistics::contracts
