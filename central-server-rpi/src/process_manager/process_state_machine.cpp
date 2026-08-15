#include "logistics/central_server/process_state_machine.hpp"

#include <algorithm>
#include <utility>

#include "logistics/contracts/identifier.hpp"

namespace logistics::central_server {
namespace {

[[nodiscard]] bool IsTerminal(WorkStage stage) noexcept {
    return stage == WorkStage::kCompleted;
}

[[nodiscard]] bool IsSuspended(WorkStage stage) noexcept {
    return stage == WorkStage::kStopped || stage == WorkStage::kFailed || stage == WorkStage::kEmergencyStopped ||
           stage == WorkStage::kRecovering;
}

[[nodiscard]] bool IsOneOf(WorkStage stage, std::initializer_list<WorkStage> expected) noexcept {
    return std::ranges::find(expected, stage) != expected.end();
}

}  // namespace

ProcessSystemState ProcessStateMachine::SystemState() const noexcept {
    return system_state_;
}

ProcessTransition ProcessStateMachine::Apply(const ProcessEvent& event) {
    if (!contracts::IsValidUuid(event.work_id)) {
        return Reject("process event workId must be a UUID");
    }
    if (!event.message_id.empty() && processed_message_ids_.contains(event.message_id)) {
        return {
            .disposition = TransitionDisposition::kDuplicate,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = "messageId was already processed",
        };
    }

    ProcessTransition transition;
    if (event.type == ProcessEventType::kWorkCreated) {
        if (system_state_ != ProcessSystemState::kIdle && system_state_ != ProcessSystemState::kRunning) {
            transition = Reject("new work is not allowed while the system is stopped, failed, or recovering");
        } else if (works_.contains(event.work_id)) {
            transition = Reject("workId already exists");
        } else {
            WorkProcessSnapshot work{
                .work_id = event.work_id,
                .stage = WorkStage::kInputDetected,
                .suspended_stage = std::nullopt,
                .destination = {},
                .last_source_id = event.source_id,
                .failure_reason = {},
            };
            works_.emplace(work.work_id, work);
            system_state_ = ProcessSystemState::kRunning;
            transition = {
                .disposition = TransitionDisposition::kApplied,
                .previous_stage = std::nullopt,
                .current_stage = WorkStage::kInputDetected,
                .reason = {},
            };
        }
    } else {
        const auto iterator = works_.find(event.work_id);
        transition =
            iterator == works_.end() ? Reject("workId is not active") : ApplyToExisting(event, iterator->second);
    }

    if (!event.message_id.empty()) {
        RememberMessage(event.message_id);
    }
    return transition;
}

ProcessTransition ProcessStateMachine::ApplySystemFailure(std::string reason) {
    SuspendActiveWorks(WorkStage::kFailed);
    for (auto& [work_id, work] : works_) {
        static_cast<void>(work_id);
        if (work.stage == WorkStage::kFailed) {
            work.failure_reason = reason;
        }
    }
    system_state_ = ProcessSystemState::kError;
    return {
        .disposition = TransitionDisposition::kApplied,
        .previous_stage = std::nullopt,
        .current_stage = std::nullopt,
        .reason = {},
    };
}

ProcessTransition ProcessStateMachine::ClearSystemFailureIfIdle() {
    if (system_state_ != ProcessSystemState::kError && system_state_ != ProcessSystemState::kEmergencyStop) {
        return Reject("automatic recovery requires an error or emergency-stop state");
    }
    if (!ActiveWorks().empty()) {
        return Reject("automatic recovery is not allowed while work is active");
    }
    system_state_ =
        system_state_ == ProcessSystemState::kEmergencyStop ? ProcessSystemState::kStopped : ProcessSystemState::kIdle;
    return {
        .disposition = TransitionDisposition::kApplied,
        .previous_stage = std::nullopt,
        .current_stage = std::nullopt,
        .reason = {},
    };
}

ProcessTransition ProcessStateMachine::ApplySystemCommand(contracts::mqtt::ControlCommand command) {
    using contracts::mqtt::ControlCommand;
    switch (command) {
        case ControlCommand::kStart:
        case ControlCommand::kRestart:
            if (system_state_ == ProcessSystemState::kRunning) {
                return {
                    .disposition = TransitionDisposition::kDuplicate,
                    .previous_stage = std::nullopt,
                    .current_stage = std::nullopt,
                    .reason = "system is already running; command may be retried for stopped devices",
                };
            }
            if (system_state_ != ProcessSystemState::kIdle && system_state_ != ProcessSystemState::kStopped) {
                return Reject("START or RESTART is only allowed from IDLE or STOPPED");
            }
            RestoreSuspendedWorks();
            system_state_ = ProcessSystemState::kRunning;
            return { .disposition = TransitionDisposition::kApplied,
                     .previous_stage = std::nullopt,
                     .current_stage = std::nullopt,
                     .reason = {} };

        case ControlCommand::kStop:
            if (system_state_ != ProcessSystemState::kRunning && system_state_ != ProcessSystemState::kIdle) {
                return Reject("STOP is only allowed from IDLE or RUNNING");
            }
            SuspendActiveWorks(WorkStage::kStopped);
            system_state_ = ProcessSystemState::kStopped;
            return { .disposition = TransitionDisposition::kApplied,
                     .previous_stage = std::nullopt,
                     .current_stage = std::nullopt,
                     .reason = {} };

        case ControlCommand::kEmergencyStop:
            if (system_state_ == ProcessSystemState::kEmergencyStop) {
                return {
                    .disposition = TransitionDisposition::kDuplicate,
                    .previous_stage = std::nullopt,
                    .current_stage = std::nullopt,
                    .reason = "system is already emergency-stopped",
                };
            }
            SuspendActiveWorks(WorkStage::kEmergencyStopped);
            system_state_ = ProcessSystemState::kEmergencyStop;
            return { .disposition = TransitionDisposition::kApplied,
                     .previous_stage = std::nullopt,
                     .current_stage = std::nullopt,
                     .reason = {} };

        case ControlCommand::kRecovery:
            if (system_state_ == ProcessSystemState::kRecovery) {
                return {
                    .disposition = TransitionDisposition::kDuplicate,
                    .previous_stage = std::nullopt,
                    .current_stage = std::nullopt,
                    .reason = "system recovery is already in progress; command may be retried",
                };
            }
            if (system_state_ != ProcessSystemState::kError && system_state_ != ProcessSystemState::kEmergencyStop) {
                return Reject("RECOVERY is only allowed from ERROR or EMERGENCY_STOP");
            }
            SuspendActiveWorks(WorkStage::kRecovering);
            system_state_ = ProcessSystemState::kRecovery;
            return { .disposition = TransitionDisposition::kApplied,
                     .previous_stage = std::nullopt,
                     .current_stage = std::nullopt,
                     .reason = {} };

        case ControlCommand::kInitialize:
            if (system_state_ == ProcessSystemState::kStopped) {
                return {
                    .disposition = TransitionDisposition::kDuplicate,
                    .previous_stage = std::nullopt,
                    .current_stage = std::nullopt,
                    .reason = "system recovery already completed in STOPPED",
                };
            }
            return Reject("INITIALIZE is retained for compatibility but is not required");

        case ControlCommand::kStatusRequest:
            return { .disposition = TransitionDisposition::kApplied,
                     .previous_stage = std::nullopt,
                     .current_stage = std::nullopt,
                     .reason = {} };

        case ControlCommand::kDestinationSet:
        case ControlCommand::kUnknown:
            return Reject("command does not change the system process state");
    }
    return Reject("unsupported system command");
}

ProcessTransition ProcessStateMachine::CompleteSystemRecovery() {
    if (system_state_ != ProcessSystemState::kRecovery) {
        return Reject("system recovery is not in progress");
    }
    works_.clear();
    processed_message_ids_.clear();
    processed_message_order_.clear();
    system_state_ = ProcessSystemState::kStopped;
    return {
        .disposition = TransitionDisposition::kApplied,
        .previous_stage = std::nullopt,
        .current_stage = std::nullopt,
        .reason = {},
    };
}

bool ProcessStateMachine::RestoreAfterServerRestart(ProcessSystemState stored_state,
                                                    std::vector<WorkProcessSnapshot> works,
                                                    std::vector<std::string> processed_message_ids) {
    std::unordered_map<std::string, WorkProcessSnapshot> restored;
    for (auto& work : works) {
        if (!contracts::IsValidUuid(work.work_id) || IsTerminal(work.stage) || restored.contains(work.work_id)) {
            return false;
        }

        const WorkStage resumable_stage = work.suspended_stage.value_or(work.stage);
        if (IsTerminal(resumable_stage) || IsSuspended(resumable_stage)) {
            return false;
        }
        work.suspended_stage = resumable_stage;
        if (stored_state == ProcessSystemState::kEmergencyStop) {
            work.stage = WorkStage::kEmergencyStopped;
        } else if (stored_state == ProcessSystemState::kError) {
            work.stage = WorkStage::kFailed;
        } else if (stored_state == ProcessSystemState::kRecovery) {
            work.stage = WorkStage::kRecovering;
        } else {
            work.stage = WorkStage::kStopped;
        }
        restored.emplace(work.work_id, std::move(work));
    }

    works_ = std::move(restored);
    processed_message_ids_.clear();
    processed_message_order_.clear();
    for (auto& message_id : processed_message_ids) {
        if (message_id.empty()) {
            return false;
        }
        RememberMessage(std::move(message_id));
    }
    if (stored_state == ProcessSystemState::kEmergencyStop) {
        system_state_ = ProcessSystemState::kEmergencyStop;
    } else if (stored_state == ProcessSystemState::kError) {
        system_state_ = ProcessSystemState::kError;
    } else if (stored_state == ProcessSystemState::kRecovery) {
        system_state_ = ProcessSystemState::kRecovery;
    } else if (stored_state == ProcessSystemState::kIdle && works_.empty()) {
        system_state_ = ProcessSystemState::kIdle;
    } else {
        system_state_ = ProcessSystemState::kStopped;
    }
    return true;
}

std::optional<WorkProcessSnapshot> ProcessStateMachine::FindWork(std::string_view work_id) const {
    const auto iterator = works_.find(std::string(work_id));
    return iterator == works_.end() ? std::nullopt : std::optional<WorkProcessSnapshot>{ iterator->second };
}

std::vector<WorkProcessSnapshot> ProcessStateMachine::ActiveWorks() const {
    std::vector<WorkProcessSnapshot> result;
    for (const auto& [work_id, work] : works_) {
        static_cast<void>(work_id);
        if (!IsTerminal(work.stage)) {
            result.push_back(work);
        }
    }
    std::ranges::sort(result, {}, &WorkProcessSnapshot::work_id);
    return result;
}

std::vector<std::string> ProcessStateMachine::ProcessedMessageIds() const {
    return { processed_message_order_.begin(), processed_message_order_.end() };
}

ProcessTransition ProcessStateMachine::ApplyToExisting(const ProcessEvent& event, WorkProcessSnapshot& work) {
    if (IsTerminal(work.stage)) {
        return Reject("completed work cannot transition");
    }
    if (IsSuspended(work.stage) && event.type != ProcessEventType::kWorkFailed) {
        return Reject("work is suspended until the system is restarted");
    }

    switch (event.type) {
        case ProcessEventType::kVisionCommandDispatched:
            if (work.stage != WorkStage::kInputDetected) {
                return Reject("vision dispatch is only allowed after input detection");
            }
            return Move(work, WorkStage::kVisionAssigned, event.source_id);

        case ProcessEventType::kPositionDetected:
            if (!IsOneOf(work.stage, { WorkStage::kVisionAssigned, WorkStage::kVisionProcessing })) {
                return Reject("POSITION_DETECTED is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kVisionProcessing, event.source_id);

        case ProcessEventType::kBarcodeSucceeded:
            if (!IsOneOf(work.stage, { WorkStage::kVisionAssigned, WorkStage::kVisionProcessing })) {
                return Reject("successful barcode recognition is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kBarcodeRecognized, event.source_id);

        case ProcessEventType::kBarcodeFailed:
        case ProcessEventType::kProductInfoFailed:
        case ProcessEventType::kWorkFailed: {
            if (!work.suspended_stage.has_value()) {
                work.suspended_stage = work.stage;
            }
            const auto previous = work.stage;
            work.stage = WorkStage::kFailed;
            work.last_source_id = event.source_id;
            work.failure_reason = event.reason.empty() ? std::string(ToString(event.type)) : event.reason;
            system_state_ = ProcessSystemState::kError;
            return {
                .disposition = TransitionDisposition::kApplied,
                .previous_stage = previous,
                .current_stage = work.stage,
                .reason = {},
            };
        }

        case ProcessEventType::kProductInfoReady:
            if (work.stage != WorkStage::kBarcodeRecognized) {
                return Reject("PRODUCT_INFO is only allowed after successful barcode recognition");
            }
            if (event.destination.empty()) {
                return Reject("PRODUCT_INFO requires a destination");
            }
            work.destination = event.destination;
            return Move(work, WorkStage::kProductIdentified, event.source_id);

        case ProcessEventType::kGripperCommandDispatched:
            if (work.stage != WorkStage::kProductIdentified) {
                return Reject("gripper dispatch is only allowed after product identification");
            }
            return Move(work, WorkStage::kGripperRequested, event.source_id);

        case ProcessEventType::kGripperStarted:
            if (!IsOneOf(work.stage, { WorkStage::kGripperRequested, WorkStage::kGripperTransferring })) {
                return Reject("gripper status is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kGripperTransferring, event.source_id);

        case ProcessEventType::kGripperCompleted:
            if (work.stage != WorkStage::kGripperTransferring) {
                return Reject("gripper completion is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kSortingRequested, event.source_id);

        case ProcessEventType::kSortingCommandDispatched:
            if (work.stage != WorkStage::kSortingRequested) {
                return Reject("sorting dispatch is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kSorting, event.source_id);

        case ProcessEventType::kSortingStarted:
            if (work.stage != WorkStage::kSorting) {
                return Reject("sorting status is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kSorting, event.source_id);

        case ProcessEventType::kSortingCompleted:
            if (!IsOneOf(work.stage, { WorkStage::kSortingRequested, WorkStage::kSorting })) {
                return Reject("sorting completion is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kTransporting, event.source_id);

        case ProcessEventType::kTransportCommandDispatched:
            if (work.stage != WorkStage::kTransportRequested) {
                return Reject("transport dispatch is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kTransporting, event.source_id);

        case ProcessEventType::kTransportStarted:
            if (!IsOneOf(work.stage, { WorkStage::kTransportRequested, WorkStage::kTransporting })) {
                return Reject("line-tracer status is not allowed in the current work stage");
            }
            return Move(work, WorkStage::kTransporting, event.source_id);

        case ProcessEventType::kWorkCompleted:
            if (!IsOneOf(work.stage, { WorkStage::kTransportRequested, WorkStage::kTransporting })) {
                return Reject("WORK_COMPLETED is only allowed during transport");
            }
            return Move(work, WorkStage::kCompleted, event.source_id);

        case ProcessEventType::kWorkCreated:
            return Reject("work already exists");
    }
    return Reject("unsupported process event");
}

ProcessTransition ProcessStateMachine::Move(WorkProcessSnapshot& work, WorkStage next_stage,
                                            std::string_view source_id) {
    const auto previous = work.stage;
    work.stage = next_stage;
    if (!source_id.empty()) {
        work.last_source_id = source_id;
    }
    if (next_stage == WorkStage::kCompleted && system_state_ == ProcessSystemState::kRunning && ActiveWorks().empty()) {
        system_state_ = ProcessSystemState::kIdle;
    }
    return {
        .disposition = TransitionDisposition::kApplied,
        .previous_stage = previous,
        .current_stage = next_stage,
        .reason = {},
    };
}

ProcessTransition ProcessStateMachine::Reject(std::string reason) const {
    return {
        .disposition = TransitionDisposition::kRejected,
        .previous_stage = std::nullopt,
        .current_stage = std::nullopt,
        .reason = std::move(reason),
    };
}

void ProcessStateMachine::SuspendActiveWorks(WorkStage suspended_stage) {
    for (auto& [work_id, work] : works_) {
        static_cast<void>(work_id);
        if (IsTerminal(work.stage)) {
            continue;
        }
        if (!IsSuspended(work.stage)) {
            work.suspended_stage = work.stage;
        }
        work.stage = suspended_stage;
    }
}

void ProcessStateMachine::RestoreSuspendedWorks() {
    for (auto& [work_id, work] : works_) {
        static_cast<void>(work_id);
        if (work.stage == WorkStage::kStopped && work.suspended_stage.has_value()) {
            work.stage = *work.suspended_stage;
            work.suspended_stage.reset();
            work.failure_reason.clear();
        }
    }
}

void ProcessStateMachine::RememberMessage(std::string message_id) {
    processed_message_ids_.insert(message_id);
    processed_message_order_.push_back(std::move(message_id));
    while (processed_message_order_.size() > kRememberedMessageLimit) {
        processed_message_ids_.erase(processed_message_order_.front());
        processed_message_order_.pop_front();
    }
}

std::string_view ToString(ProcessSystemState state) noexcept {
    switch (state) {
        case ProcessSystemState::kIdle:
            return "IDLE";
        case ProcessSystemState::kRunning:
            return "RUNNING";
        case ProcessSystemState::kStopped:
            return "STOPPED";
        case ProcessSystemState::kError:
            return "ERROR";
        case ProcessSystemState::kEmergencyStop:
            return "ESTOP";
        case ProcessSystemState::kRecovery:
            return "RECOVERY";
    }
    return "UNKNOWN";
}

std::string_view ToString(WorkStage stage) noexcept {
    switch (stage) {
        case WorkStage::kInputDetected:
            return "INPUT_DETECTED";
        case WorkStage::kVisionAssigned:
            return "VISION_ASSIGNED";
        case WorkStage::kVisionProcessing:
            return "VISION_PROCESSING";
        case WorkStage::kBarcodeRecognized:
            return "BARCODE_RECOGNIZED";
        case WorkStage::kProductIdentified:
            return "PRODUCT_IDENTIFIED";
        case WorkStage::kGripperRequested:
            return "GRIPPER_REQUESTED";
        case WorkStage::kGripperTransferring:
            return "GRIPPER_TRANSFERRING";
        case WorkStage::kSortingRequested:
            return "SORTING_REQUESTED";
        case WorkStage::kSorting:
            return "SORTING";
        case WorkStage::kTransportRequested:
            return "TRANSPORT_REQUESTED";
        case WorkStage::kTransporting:
            return "TRANSPORTING";
        case WorkStage::kCompleted:
            return "COMPLETED";
        case WorkStage::kStopped:
            return "STOPPED";
        case WorkStage::kFailed:
            return "FAILED";
        case WorkStage::kEmergencyStopped:
            return "ESTOP";
        case WorkStage::kRecovering:
            return "RECOVERY";
    }
    return "UNKNOWN";
}

std::string_view ToString(ProcessEventType type) noexcept {
    switch (type) {
        case ProcessEventType::kWorkCreated:
            return "WORK_CREATED";
        case ProcessEventType::kVisionCommandDispatched:
            return "VISION_COMMAND_DISPATCHED";
        case ProcessEventType::kPositionDetected:
            return "POSITION_DETECTED";
        case ProcessEventType::kBarcodeSucceeded:
            return "BARCODE_SUCCEEDED";
        case ProcessEventType::kBarcodeFailed:
            return "BARCODE_FAILED";
        case ProcessEventType::kProductInfoReady:
            return "PRODUCT_INFO_READY";
        case ProcessEventType::kProductInfoFailed:
            return "PRODUCT_INFO_FAILED";
        case ProcessEventType::kGripperCommandDispatched:
            return "GRIPPER_COMMAND_DISPATCHED";
        case ProcessEventType::kGripperStarted:
            return "GRIPPER_STARTED";
        case ProcessEventType::kGripperCompleted:
            return "GRIPPER_COMPLETED";
        case ProcessEventType::kSortingCommandDispatched:
            return "SORTING_COMMAND_DISPATCHED";
        case ProcessEventType::kSortingStarted:
            return "SORTING_STARTED";
        case ProcessEventType::kSortingCompleted:
            return "SORTING_COMPLETED";
        case ProcessEventType::kTransportCommandDispatched:
            return "TRANSPORT_COMMAND_DISPATCHED";
        case ProcessEventType::kTransportStarted:
            return "TRANSPORT_STARTED";
        case ProcessEventType::kWorkCompleted:
            return "WORK_COMPLETED";
        case ProcessEventType::kWorkFailed:
            return "WORK_FAILED";
    }
    return "UNKNOWN";
}

std::optional<ProcessSystemState> ParseProcessSystemState(std::string_view value) noexcept {
    if (value == "IDLE") {
        return ProcessSystemState::kIdle;
    }
    if (value == "RUNNING") {
        return ProcessSystemState::kRunning;
    }
    if (value == "STOPPED") {
        return ProcessSystemState::kStopped;
    }
    if (value == "ERROR") {
        return ProcessSystemState::kError;
    }
    if (value == "ESTOP") {
        return ProcessSystemState::kEmergencyStop;
    }
    if (value == "RECOVERY") {
        return ProcessSystemState::kRecovery;
    }
    return std::nullopt;
}

std::optional<WorkStage> ParseWorkStage(std::string_view value) noexcept {
    constexpr WorkStage stages[] = {
        WorkStage::kInputDetected,
        WorkStage::kVisionAssigned,
        WorkStage::kVisionProcessing,
        WorkStage::kBarcodeRecognized,
        WorkStage::kProductIdentified,
        WorkStage::kGripperRequested,
        WorkStage::kGripperTransferring,
        WorkStage::kSortingRequested,
        WorkStage::kSorting,
        WorkStage::kTransportRequested,
        WorkStage::kTransporting,
        WorkStage::kCompleted,
        WorkStage::kStopped,
        WorkStage::kFailed,
        WorkStage::kEmergencyStopped,
        WorkStage::kRecovering,
    };
    for (const WorkStage stage : stages) {
        if (ToString(stage) == value) {
            return stage;
        }
    }
    return std::nullopt;
}

std::optional<ProcessEventType> ParseProcessEventType(std::string_view value) noexcept {
    constexpr std::array values{
        ProcessEventType::kWorkCreated,
        ProcessEventType::kVisionCommandDispatched,
        ProcessEventType::kPositionDetected,
        ProcessEventType::kBarcodeSucceeded,
        ProcessEventType::kBarcodeFailed,
        ProcessEventType::kProductInfoReady,
        ProcessEventType::kProductInfoFailed,
        ProcessEventType::kGripperCommandDispatched,
        ProcessEventType::kGripperStarted,
        ProcessEventType::kGripperCompleted,
        ProcessEventType::kSortingCommandDispatched,
        ProcessEventType::kSortingStarted,
        ProcessEventType::kSortingCompleted,
        ProcessEventType::kTransportCommandDispatched,
        ProcessEventType::kTransportStarted,
        ProcessEventType::kWorkCompleted,
        ProcessEventType::kWorkFailed,
    };
    const auto found = std::ranges::find(values, value, [](ProcessEventType type) { return ToString(type); });
    return found == values.end() ? std::nullopt : std::optional<ProcessEventType>{ *found };
}

}  // namespace logistics::central_server
