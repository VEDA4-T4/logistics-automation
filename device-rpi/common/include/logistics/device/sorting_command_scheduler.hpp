#pragma once

namespace logistics::device {

enum class SortingUartWork {
    kNone,
    kCommand,
    kResync,
    kKeepalive,
};

enum class SortingCommandBlocker {
    kNone,
    kEmergency,
    kSafety,
    kFailure,
    kResync,
    kPendingUart,
};

struct SortingUartWorkState {
    bool emergency_processed{};
    bool safety_pending{};
    bool uart_failure_pending{};
    bool uart_open{};
    bool uart_command_pending{};
    bool resync_pending{};
    bool command_queued{};
    bool keepalive_due{};
};

struct SortingUartWorkDecision {
    SortingUartWork work{ SortingUartWork::kNone };
    SortingCommandBlocker blocker{ SortingCommandBlocker::kNone };
};

[[nodiscard]] constexpr SortingUartWorkDecision ChooseSortingUartWork(const SortingUartWorkState& state) noexcept {
    if (state.emergency_processed) {
        return { .blocker = SortingCommandBlocker::kEmergency };
    }
    if (state.safety_pending) {
        return { .blocker = SortingCommandBlocker::kSafety };
    }
    if (state.uart_failure_pending || !state.uart_open) {
        return { .blocker = SortingCommandBlocker::kFailure };
    }
    if (state.uart_command_pending) {
        return { .blocker =
                     state.resync_pending ? SortingCommandBlocker::kResync : SortingCommandBlocker::kPendingUart };
    }
    if (state.resync_pending) {
        return { .work = SortingUartWork::kResync,
                 .blocker = state.command_queued ? SortingCommandBlocker::kResync : SortingCommandBlocker::kNone };
    }
    if (state.command_queued) {
        return { .work = SortingUartWork::kCommand };
    }
    if (state.keepalive_due) {
        return { .work = SortingUartWork::kKeepalive };
    }
    return {};
}

}  // namespace logistics::device
