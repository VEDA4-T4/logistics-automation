#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::central_server {

enum class ProcessSystemState : std::uint8_t {
    kIdle,
    kRunning,
    kStopped,
    kError,
    kEmergencyStop,
    kRecovery,
};

enum class WorkStage : std::uint8_t {
    kInputDetected,
    kVisionAssigned,
    kVisionProcessing,
    kBarcodeRecognized,
    kProductIdentified,
    kGripperRequested,
    kGripperTransferring,
    kSortingRequested,
    kSorting,
    kTransportRequested,
    kTransporting,
    kCompleted,
    kStopped,
    kFailed,
    kEmergencyStopped,
    kRecovering,
};

enum class ProcessEventType : std::uint8_t {
    kWorkCreated,
    kVisionCommandDispatched,
    kPositionDetected,
    kBarcodeSucceeded,
    kBarcodeFailed,
    kProductInfoReady,
    kProductInfoFailed,
    kGripperCommandDispatched,
    kGripperStarted,
    kGripperCompleted,
    kSortingCommandDispatched,
    kSortingStarted,
    kSortingCompleted,
    kTransportCommandDispatched,
    kTransportStarted,
    kWorkCompleted,
    kWorkFailed,
};

struct ProcessEvent final {
    ProcessEventType type{ ProcessEventType::kWorkCreated };
    std::string message_id;
    std::string work_id;
    std::string source_id;
    std::string destination;
    std::string reason;
};

struct WorkProcessSnapshot final {
    std::string work_id;
    WorkStage stage{ WorkStage::kInputDetected };
    std::optional<WorkStage> suspended_stage;
    std::string destination;
    std::string last_source_id;
    std::string failure_reason;
};

enum class TransitionDisposition : std::uint8_t {
    kApplied,
    kDuplicate,
    kRejected,
};

struct ProcessTransition final {
    TransitionDisposition disposition{ TransitionDisposition::kRejected };
    std::optional<WorkStage> previous_stage;
    std::optional<WorkStage> current_stage;
    std::string reason;

    [[nodiscard]] bool Applied() const noexcept {
        return disposition == TransitionDisposition::kApplied;
    }
};

class ProcessStateMachine final {
public:
    [[nodiscard]] ProcessSystemState SystemState() const noexcept;
    [[nodiscard]] bool AcceptsNewWork() const noexcept;
    [[nodiscard]] ProcessTransition Apply(const ProcessEvent& event);
    [[nodiscard]] ProcessTransition ApplySystemFailure(std::string reason);
    [[nodiscard]] ProcessTransition ClearSystemFailureIfIdle();
    [[nodiscard]] ProcessTransition ApplySystemCommand(contracts::mqtt::ControlCommand command);
    [[nodiscard]] ProcessTransition CompleteSystemRecovery();
    [[nodiscard]] bool RestoreAfterServerRestart(ProcessSystemState stored_state,
                                                 std::vector<WorkProcessSnapshot> works,
                                                 std::vector<std::string> processed_message_ids = {});
    [[nodiscard]] std::optional<WorkProcessSnapshot> FindWork(std::string_view work_id) const;
    [[nodiscard]] std::vector<WorkProcessSnapshot> ActiveWorks() const;
    [[nodiscard]] std::vector<std::string> ProcessedMessageIds() const;

private:
    static constexpr std::size_t kRememberedMessageLimit = 2048;

    [[nodiscard]] ProcessTransition ApplyToExisting(const ProcessEvent& event, WorkProcessSnapshot& work);
    [[nodiscard]] ProcessTransition Move(WorkProcessSnapshot& work, WorkStage next_stage,
                                         std::string_view source_id = {});
    [[nodiscard]] ProcessTransition Reject(std::string reason) const;
    void SuspendActiveWorks(WorkStage suspended_stage);
    void RestoreSuspendedWorks();
    void RememberMessage(std::string message_id);

    ProcessSystemState system_state_{ ProcessSystemState::kIdle };
    std::unordered_map<std::string, WorkProcessSnapshot> works_;
    std::unordered_set<std::string> processed_message_ids_;
    std::deque<std::string> processed_message_order_;
};

[[nodiscard]] std::string_view ToString(ProcessSystemState state) noexcept;
[[nodiscard]] std::string_view ToString(WorkStage stage) noexcept;
[[nodiscard]] std::string_view ToString(ProcessEventType type) noexcept;
[[nodiscard]] std::optional<ProcessSystemState> ParseProcessSystemState(std::string_view value) noexcept;
[[nodiscard]] std::optional<WorkStage> ParseWorkStage(std::string_view value) noexcept;
[[nodiscard]] std::optional<ProcessEventType> ParseProcessEventType(std::string_view value) noexcept;

}  // namespace logistics::central_server
