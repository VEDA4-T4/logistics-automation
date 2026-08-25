#include "logistics/central_server/process_orchestrator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <utility>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] std::string Uppercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

[[nodiscard]] bool IsOneOf(std::string_view value, std::initializer_list<std::string_view> expected) noexcept {
    return std::ranges::find(expected, value) != expected.end();
}

[[nodiscard]] std::optional<contracts::DeviceRole> DeviceRoleForSource(const ProcessOrchestratorConfig& config,
                                                                       std::string_view source_id) noexcept {
    if (source_id == config.input_device_id) {
        return contracts::DeviceRole::kInput;
    }
    if (source_id == config.vision_device_id) {
        return contracts::DeviceRole::kVision;
    }
    if (source_id == config.gripper_device_id) {
        return contracts::DeviceRole::kGripper;
    }
    if (source_id == config.sorting_device_id) {
        return contracts::DeviceRole::kSorting;
    }
    if (config.line_tracer_enabled && source_id == config.line_tracer_device_id) {
        return contracts::DeviceRole::kLineTracer;
    }
    return std::nullopt;
}

[[nodiscard]] ProcessOrchestrationResult NotHandled() {
    return {
        .handled = false,
        .transition =
            {
                .disposition = TransitionDisposition::kRejected,
                .previous_stage = std::nullopt,
                .current_stage = std::nullopt,
                .reason = "message does not affect the process state",
            },
        .commands = {},
    };
}

[[nodiscard]] ProcessOrchestrationResult Rejected(std::string reason) {
    return {
        .handled = true,
        .transition =
            {
                .disposition = TransitionDisposition::kRejected,
                .previous_stage = std::nullopt,
                .current_stage = std::nullopt,
                .reason = std::move(reason),
            },
        .commands = {},
    };
}

[[nodiscard]] ProcessEvent Event(ProcessEventType type, const mqtt::MqttMessage& message, std::string work_id,
                                 std::string reason = {}) {
    return {
        .type = type,
        .message_id = message.message_id,
        .work_id = std::move(work_id),
        .source_id = message.source_id,
        .destination = {},
        .reason = std::move(reason),
    };
}

[[nodiscard]] bool OwnsDownstreamDevices(WorkStage stage) noexcept {
    constexpr std::array occupied_stages{
        WorkStage::kGripperRequested, WorkStage::kGripperTransferring, WorkStage::kSortingRequested,
        WorkStage::kSorting,          WorkStage::kTransportRequested,  WorkStage::kTransporting,
    };
    return std::ranges::find(occupied_stages, stage) != occupied_stages.end();
}

[[nodiscard]] bool DownstreamDevicesBusy(const ProcessStateMachine& machine, std::string_view work_id) {
    return std::ranges::any_of(machine.ActiveWorks(), [work_id](const WorkProcessSnapshot& work) {
        return work.work_id != work_id && OwnsDownstreamDevices(work.stage);
    });
}

[[nodiscard]] std::optional<std::string> ProcessCommandRequestId(const mqtt::MqttMessage& message) {
    if (const auto* control = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return control->request_id;
    }
    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return destination->request_id;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> ProcessMessageSequence(std::string_view message_id) noexcept {
    constexpr std::string_view prefix = "PROCESS-";
    if (!message_id.starts_with(prefix)) {
        return std::nullopt;
    }
    std::uint64_t sequence{};
    const auto sequence_separator = message_id.rfind('-');
    const auto sequence_offset = sequence_separator == std::string_view::npos ? prefix.size() : sequence_separator + 1;
    if (sequence_offset >= message_id.size()) {
        return std::nullopt;
    }
    const auto first = message_id.data() + sequence_offset;
    const auto last = message_id.data() + message_id.size();
    const auto parsed = std::from_chars(first, last, sequence);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional{ sequence } : std::nullopt;
}

}  // namespace

bool ProcessOrchestratorConfig::IsValid() const noexcept {
    const bool initial_position_valid =
        line_tracer_initial_position.empty() || IsOneOf(line_tracer_initial_position, { "A", "B", "C" });

    return mqtt::IsValidTopicLevel(server_id) && mqtt::IsValidTopicLevel(input_device_id) &&
           mqtt::IsValidTopicLevel(vision_device_id) && mqtt::IsValidTopicLevel(gripper_device_id) &&
           mqtt::IsValidTopicLevel(sorting_device_id) && mqtt::IsValidTopicLevel(line_tracer_device_id) &&
           mqtt::IsValidTopicLevel(default_destination) && initial_position_valid &&
           (!homography.enabled || homography.IsValid());
}

bool ProcessCommandTracker::Track(const ProcessCommandIntent& intent) {
    const auto request_id = ProcessCommandRequestId(intent.message);
    if (!request_id.has_value() || request_id->empty() || pending_.contains(*request_id)) {
        return false;
    }
    pending_.emplace(*request_id, intent);
    return true;
}

bool ProcessCommandTracker::Restore(std::vector<ProcessCommandIntent> intents) {
    std::unordered_map<std::string, ProcessCommandIntent> restored;
    for (auto& intent : intents) {
        const auto request_id = ProcessCommandRequestId(intent.message);
        if (!request_id.has_value() || request_id->empty() ||
            !restored.emplace(*request_id, std::move(intent)).second) {
            return false;
        }
    }
    pending_ = std::move(restored);
    return true;
}

bool ProcessCommandTracker::Remove(std::string_view request_id) {
    return pending_.erase(std::string(request_id)) != 0;
}

bool ProcessCommandTracker::MarkDispatched(std::string_view request_id) {
    const auto pending = pending_.find(std::string(request_id));
    if (pending == pending_.end()) {
        return false;
    }
    pending->second.dispatch_confirmed = true;
    return true;
}

std::optional<ProcessCommandIntent> ProcessCommandTracker::HandleResponse(const mqtt::MqttMessage& message) {
    const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message);
    if (response == nullptr || !mqtt::IsTerminal(response->result)) {
        return std::nullopt;
    }
    const auto pending = pending_.find(response->request_id);
    if (pending == pending_.end()) {
        return std::nullopt;
    }

    ProcessCommandIntent intent = std::move(pending->second);
    pending_.erase(pending);
    return intent;
}

std::size_t ProcessCommandTracker::PendingCount() const noexcept {
    return pending_.size();
}

void ProcessCommandTracker::Clear() noexcept {
    pending_.clear();
}

ProcessOrchestrator::ProcessOrchestrator(ProcessOrchestratorConfig config)
    : config_(std::move(config)), homography_(config_.homography) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid process orchestrator device identifier");
    }
}

void ProcessOrchestrator::SetProcessEpoch(std::string process_epoch) {
    if (!process_epoch.empty() && !contracts::IsValidUuid(process_epoch)) {
        throw std::invalid_argument("process epoch must be a valid UUID");
    }
    process_epoch_ = std::move(process_epoch);
}

bool ProcessOrchestrator::Enabled() const noexcept {
    return config_.enabled;
}

bool ProcessOrchestrator::AcceptsInputWorkCreation() const noexcept {
    return config_.enabled && state_machine_.AcceptsNewWork();
}

bool ProcessOrchestrator::AcceptsNewWork() const noexcept {
    return AcceptsInputWorkCreation();
}

bool ProcessOrchestrator::IsWorkCreationSource(std::string_view device_id) const noexcept {
    return config_.enabled && device_id == config_.input_device_id;
}

std::string_view ProcessOrchestrator::VisionDeviceId() const noexcept {
    return config_.vision_device_id;
}

const ProcessStateMachine& ProcessOrchestrator::StateMachine() const noexcept {
    return state_machine_;
}

ProcessOrchestrationResult ProcessOrchestrator::Preview(const mqtt::MqttMessage& message) const {
    if (!config_.enabled) {
        return NotHandled();
    }
    auto state_copy = state_machine_;
    auto orchestrator_copy = *this;
    orchestrator_copy.state_machine_ = std::move(state_copy);
    return orchestrator_copy.HandleWith(orchestrator_copy.state_machine_, message, false);
}

ProcessOrchestrationResult ProcessOrchestrator::Handle(const mqtt::MqttMessage& message) {
    if (!config_.enabled) {
        return NotHandled();
    }
    auto result = HandleWith(state_machine_, message, true);
    if (result.transition.Applied()) {
        ++revision_;
    }
    return result;
}

ProcessOrchestrationResult ProcessOrchestrator::BeginWork(std::string_view message_id, std::string_view work_id,
                                                          std::string_view input_device_id,
                                                          std::string_view timestamp) {
    return BeginWorkImpl(message_id, work_id, input_device_id, timestamp, true);
}

ProcessOrchestrationResult ProcessOrchestrator::BeginWorkAfterInputStopped(std::string_view message_id,
                                                                           std::string_view work_id,
                                                                           std::string_view input_device_id,
                                                                           std::string_view timestamp) {
    return BeginWorkImpl(message_id, work_id, input_device_id, timestamp, false);
}

ProcessOrchestrationResult ProcessOrchestrator::BeginWorkImpl(std::string_view message_id, std::string_view work_id,
                                                              std::string_view input_device_id,
                                                              std::string_view timestamp,
                                                              const bool stop_input_conveyor) {
    if (!config_.enabled) {
        return {
            .handled = false,
            .transition =
                {
                    .disposition = TransitionDisposition::kApplied,
                    .previous_stage = std::nullopt,
                    .current_stage = std::nullopt,
                    .reason = {},
                },
            .commands = {},
        };
    }
    auto transition = state_machine_.Apply({
        .type = ProcessEventType::kWorkCreated,
        .message_id = std::string(message_id),
        .work_id = std::string(work_id),
        .source_id = std::string(input_device_id),
        .destination = {},
        .reason = {},
    });
    if (transition.Applied()) {
        ++revision_;
    }
    ProcessOrchestrationResult result{
        .handled = true,
        .transition = std::move(transition),
        .commands = {},
    };
    if (result.transition.Applied() && stop_input_conveyor) {
        result.commands.push_back(MakeInputConveyorCommand(work_id, mqtt::ControlCommand::kStop, timestamp));
    }
    return result;
}

contracts::mqtt::MqttMessage ProcessOrchestrator::MakeInputConveyorSafetyStop(std::string_view trigger_message_id,
                                                                              std::string_view timestamp) const {
    const std::string request_id = "INPUT-DETECTION-STOP-" + std::string(trigger_message_id);
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = request_id,
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = config_.server_id,
        .timestamp = std::string(timestamp),
        .data =
            mqtt::ControlCommandPayload{
                .request_id = request_id,
                .command = mqtt::ControlCommand::kStop,
                .target_device_id = config_.input_device_id,
                .component_id = "input_conveyor",
                .params = mqtt::Json{ { "reason", "BOX_DETECTED" } },
            },
    };
}

std::vector<ProcessCommandIntent> ProcessOrchestrator::SortingDetectionCommands(std::string_view work_id,
                                                                                std::string_view timestamp) {
    const auto work = state_machine_.FindWork(work_id);
    if (!config_.enabled || !work.has_value() || work->stage != WorkStage::kSorting) {
        return {};
    }
    std::vector<ProcessCommandIntent> commands;
    commands.push_back(MakeSortingControlCommand(work_id, mqtt::ControlCommand::kStop, "sorting_conveyor", timestamp));
    return commands;
}

ProcessTransition ProcessOrchestrator::ConfirmVisionAssignment(std::string_view message_id, std::string_view work_id) {
    if (!config_.enabled) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
        };
    }
    auto transition = state_machine_.Apply({
        .type = ProcessEventType::kVisionCommandDispatched,
        .message_id = std::string(message_id) + "-VISION-DISPATCHED",
        .work_id = std::string(work_id),
        .source_id = config_.server_id,
        .destination = {},
        .reason = {},
    });
    if (transition.Applied()) {
        ++revision_;
    }
    return transition;
}

ProcessOrchestrationResult ProcessOrchestrator::HandleCommandCompletion(const ProcessCommandIntent& intent,
                                                                        const mqtt::MqttMessage& response) {
    const auto* result = mqtt::GetPayload<mqtt::CommandResponsePayload>(response);
    if (result == nullptr ||
        (result->result != mqtt::CommandResult::kSuccess && result->result != mqtt::CommandResult::kDuplicated)) {
        return NotHandled();
    }

    ProcessOrchestrationResult completion{
        .handled = true,
        .transition =
            {
                .disposition = TransitionDisposition::kApplied,
                .previous_stage = std::nullopt,
                .current_stage = std::nullopt,
                .reason = {},
            },
        .commands = {},
    };

    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(intent.message)) {
        const auto work = state_machine_.FindWork(intent.work_id);
        if (destination->target_device_id != config_.sorting_device_id ||
            response.source_id != config_.sorting_device_id || result->request_id != destination->request_id ||
            result->command != mqtt::ControlCommand::kDestinationSet || !work.has_value() ||
            (work->stage != WorkStage::kSortingRequested && work->stage != WorkStage::kSorting)) {
            return NotHandled();
        }
        completion.commands.push_back(MakeSortingControlCommand(intent.work_id, mqtt::ControlCommand::kStart,
                                                                "sorting_conveyor", response.timestamp));
        return completion;
    }

    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(intent.message);
    if (command == nullptr || response.source_id != command->target_device_id ||
        result->request_id != command->request_id || result->command != command->command) {
        return NotHandled();
    }

    if (command->target_device_id == config_.gripper_device_id && command->command == mqtt::ControlCommand::kExecute) {
        completion.transition = state_machine_.Apply({
            .type = ProcessEventType::kGripperCompleted,
            .message_id = response.message_id + "-PROCESS-COMPLETED",
            .work_id = intent.work_id,
            .source_id = response.source_id,
            .destination = {},
            .reason = {},
        });
        if (!completion.transition.Applied()) {
            return completion;
        }
        ++revision_;
        const auto work = state_machine_.FindWork(intent.work_id);
        if (work.has_value()) {
            completion.commands.push_back(MakeDestinationCommand(
                intent.work_id, work->destination, config_.sorting_device_id, std::nullopt, response.timestamp));
        }
        return completion;
    }

    if (command->target_device_id != config_.sorting_device_id) {
        return NotHandled();
    }
    const auto work = state_machine_.FindWork(intent.work_id);
    if (!work.has_value()) {
        return NotHandled();
    }
    if (command->command == mqtt::ControlCommand::kStart && command->component_id == "sorting_conveyor" &&
        work->stage == WorkStage::kSortingRequested) {
        completion.transition = state_machine_.Apply({
            .type = ProcessEventType::kSortingCommandDispatched,
            .message_id = response.message_id + "-PROCESS-ACCEPTED",
            .work_id = intent.work_id,
            .source_id = config_.server_id,
            .destination = {},
            .reason = {},
        });
        if (!completion.transition.Applied()) {
            return completion;
        }
        ++revision_;
        if (!config_.line_tracer_enabled) {
            completion.commands.push_back(
                MakeInputConveyorCommand(intent.work_id, mqtt::ControlCommand::kStart, response.timestamp));
        }
        return completion;
    }
    if (command->command == mqtt::ControlCommand::kStop && command->component_id == "sorting_conveyor" &&
        (work->stage == WorkStage::kSorting || work->stage == WorkStage::kTransporting)) {
        completion.commands.push_back(
            MakeSortingControlCommand(intent.work_id, mqtt::ControlCommand::kRecovery, "GATE", response.timestamp));
        return completion;
    }
    return NotHandled();
}

ProcessTransition ProcessOrchestrator::ConfirmDispatch(const ProcessCommandIntent& intent) {
    if (!intent.dispatched_event.has_value()) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
        };
    }
    auto transition = state_machine_.Apply({
        .type = *intent.dispatched_event,
        .message_id = intent.message.message_id + "-DISPATCHED",
        .work_id = intent.work_id,
        .source_id = config_.server_id,
        .destination = {},
        .reason = {},
    });
    if (transition.Applied()) {
        ++revision_;
    }
    return transition;
}

ProcessTransition ProcessOrchestrator::FailCommandResponse(const ProcessCommandIntent& intent,
                                                           const mqtt::CommandResult /*result*/, std::string reason) {
    return FailDispatch(intent, std::move(reason));
}

ProcessTransition ProcessOrchestrator::FailDispatch(const ProcessCommandIntent& intent, std::string reason) {
    std::string target_device_id;
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(intent.message)) {
        target_device_id = command->target_device_id;
    } else if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(intent.message)) {
        target_device_id = destination->target_device_id;
    }
    if (!target_device_id.empty() && target_device_id != "SYSTEM") {
        const std::array process_devices{
            config_.input_device_id,   config_.vision_device_id,      config_.gripper_device_id,
            config_.sorting_device_id, config_.line_tracer_device_id,
        };
        if (std::ranges::find(process_devices, target_device_id) != process_devices.end()) {
            device_health_.insert_or_assign(std::move(target_device_id), false);
        }
    }
    auto transition = state_machine_.Apply({
        .type = ProcessEventType::kWorkFailed,
        .message_id = intent.message.message_id + "-FAILED",
        .work_id = intent.work_id,
        .source_id = config_.server_id,
        .destination = {},
        .reason = std::move(reason),
    });
    if (transition.Applied()) {
        ++revision_;
    }
    return transition;
}

ProcessTransition ProcessOrchestrator::PreviewSystemCommand(mqtt::ControlCommand command) const {
    auto state_copy = state_machine_;
    return state_copy.ApplySystemCommand(command);
}

ProcessTransition ProcessOrchestrator::ApplySystemCommand(mqtt::ControlCommand command) {
    if (!config_.enabled) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
        };
    }
    auto transition = state_machine_.ApplySystemCommand(command);
    if (transition.Applied()) {
        ++revision_;
    }
    return transition;
}

std::vector<ProcessCommandIntent> ProcessCommandTracker::PendingCommands() const {
    std::vector<ProcessCommandIntent> commands;
    commands.reserve(pending_.size());
    for (const auto& [request_id, intent] : pending_) {
        static_cast<void>(request_id);
        commands.push_back(intent);
    }
    std::ranges::sort(commands, [](const ProcessCommandIntent& left, const ProcessCommandIntent& right) {
        const auto left_sequence = ProcessMessageSequence(left.message.message_id);
        const auto right_sequence = ProcessMessageSequence(right.message.message_id);
        if (left_sequence.has_value() && right_sequence.has_value() && left_sequence != right_sequence) {
            return *left_sequence < *right_sequence;
        }
        return left.message.message_id < right.message.message_id;
    });
    return commands;
}

ProcessTransition ProcessOrchestrator::FailSystemCommand(mqtt::ControlCommand command, mqtt::CommandResult result,
                                                         std::string reason) {
    if (!config_.enabled) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
        };
    }
    if (command == mqtt::ControlCommand::kEmergencyStop) {
        auto transition = state_machine_.ApplySystemCommand(command);
        if (transition.Applied()) {
            ++revision_;
        }
        return transition;
    }
    static_cast<void>(result);
    return {
        .disposition = TransitionDisposition::kDuplicate,
        .previous_stage = std::nullopt,
        .current_stage = std::nullopt,
        .reason = std::move(reason),
    };
}

ProcessTransition ProcessOrchestrator::CompleteSystemRecovery() {
    if (!config_.enabled) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
        };
    }
    auto transition = state_machine_.CompleteSystemRecovery();
    if (transition.Applied()) {
        std::erase_if(gripper_targets_,
                      [this](const auto& entry) { return !state_machine_.FindWork(entry.first).has_value(); });
        ++revision_;
    }
    return transition;
}

ProcessTransition ProcessOrchestrator::CommitSystemRecovery(
    const std::function<bool(const std::vector<WorkProcessSnapshot>&)>& persist) {
    auto preview = state_machine_;
    auto transition = config_.enabled ? preview.CompleteSystemRecovery()
                                      : ProcessTransition{
                                            .disposition = TransitionDisposition::kApplied,
                                            .previous_stage = std::nullopt,
                                            .current_stage = std::nullopt,
                                            .reason = {},
                                        };
    if (!transition.Applied()) {
        return transition;
    }
    if (!persist(state_machine_.ActiveWorks())) {
        return {
            .disposition = TransitionDisposition::kRejected,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = "recovery completion persistence failed",
        };
    }
    return CompleteSystemRecovery();
}

ProcessRestoreResult ProcessOrchestrator::RestoreAfterServerRestart(
    ProcessSystemState stored_state, std::vector<WorkProcessSnapshot> works,
    std::unordered_map<std::string, GripperTarget> gripper_targets, std::uint64_t message_sequence,
    std::vector<std::string> processed_message_ids) {
    std::vector<InvalidatedRestoredWork> invalidated_works;
    std::erase_if(works, [](const WorkProcessSnapshot& work) { return work.stage == WorkStage::kFailed; });
    if (!homography_.Enabled()) {
        gripper_targets.clear();
    } else {
        for (auto iterator = gripper_targets.begin(); iterator != gripper_targets.end();) {
            const GripperTarget& target = iterator->second;
            const bool current_calibration = target.calibration_version == config_.homography.calibration_version &&
                                             target.coordinate_frame == config_.homography.coordinate_frame;
            const auto work = std::ranges::find(works, iterator->first, &WorkProcessSnapshot::work_id);
            if (work == works.end()) {
                iterator = gripper_targets.erase(iterator);
            } else if (!current_calibration) {
                invalidated_works.push_back({
                    .work_id = iterator->first,
                    .reason = "stored gripper target uses stale homography calibration; detect the product again",
                });
                works.erase(work);
                iterator = gripper_targets.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    if (!state_machine_.RestoreAfterServerRestart(stored_state, std::move(works), std::move(processed_message_ids))) {
        return {};
    }
    gripper_targets_ = std::move(gripper_targets);
    message_sequence_ = message_sequence;
    ++revision_;
    return {
        .restored = true,
        .invalidated_works = std::move(invalidated_works),
    };
}

const std::unordered_map<std::string, GripperTarget>& ProcessOrchestrator::GripperTargets() const noexcept {
    return gripper_targets_;
}

std::uint64_t ProcessOrchestrator::MessageSequence() const noexcept {
    return message_sequence_;
}

std::uint64_t ProcessOrchestrator::Revision() const noexcept {
    return revision_;
}

ProcessOrchestrationResult ProcessOrchestrator::HandleWith(ProcessStateMachine& machine,
                                                           const mqtt::MqttMessage& message, bool create_commands) {
    ProcessEvent event;
    bool mapped = true;
    std::optional<GripperTarget> position_target;

    if (const auto* position = mqtt::GetPayload<mqtt::PositionDetectedPayload>(message)) {
        if (homography_.Enabled()) {
            position_target = homography_.Transform(*position);
            if (!position_target.has_value()) {
                return Rejected("position detection is missing valid rotated box corners for homography");
            }
        }
        event = Event(ProcessEventType::kPositionDetected, message, position->work_id);
    } else if (const auto* barcode = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(message)) {
        const bool succeeded = barcode->recognition_status == "SUCCESS";
        event = Event(succeeded ? ProcessEventType::kBarcodeSucceeded : ProcessEventType::kBarcodeFailed, message,
                      barcode->work_id, barcode->message.value_or("barcode recognition failed"));
    } else if (const auto* product = mqtt::GetPayload<mqtt::ProductInfoPayload>(message)) {
        const bool succeeded = product->recognition_status == "SUCCESS" && !product->destination.empty();
        if (succeeded && homography_.Enabled() && !gripper_targets_.contains(product->work_id)) {
            return Rejected("gripper target is unavailable; a valid position detection is required first");
        }
        event = Event(succeeded ? ProcessEventType::kProductInfoReady : ProcessEventType::kProductInfoFailed, message,
                      product->work_id, product->message.value_or("product information is incomplete"));
        event.destination = product->destination;
    } else if (const auto* heartbeat = mqtt::GetPayload<mqtt::HeartbeatPayload>(message)) {
        const auto role = DeviceRoleForSource(config_, message.source_id);
        if (!role.has_value()) {
            return NotHandled();
        }
        const auto meaning = contracts::DeviceStateMeaningFor(*role, Uppercase(heartbeat->current_state));
        RememberDeviceHealth(message.source_id, meaning, heartbeat->status, heartbeat->error_code);
        return NotHandled();
    } else if (const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message)) {
        const std::string current_state = Uppercase(status->current_state);
        const auto role = DeviceRoleForSource(config_, message.source_id);
        const auto meaning = role.has_value() ? contracts::DeviceStateMeaningFor(*role, current_state)
                                              : contracts::DeviceStateMeaning::kUnknown;
        if (role.has_value()) {
            RememberDeviceHealth(message.source_id, meaning, status->status, status->error_code);
        }
        if (role.has_value() && mqtt::IsConnectionFailure(status->status)) {
            return NotHandled();
        }
        if (!status->job_id.has_value()) {
            return NotHandled();
        }
        if (message.source_id == config_.gripper_device_id) {
            if (meaning != contracts::DeviceStateMeaning::kWorking) {
                return NotHandled();
            }
            event = Event(ProcessEventType::kGripperStarted, message, *status->job_id);
        } else if (message.source_id == config_.sorting_device_id) {
            if (meaning != contracts::DeviceStateMeaning::kWorking &&
                meaning != contracts::DeviceStateMeaning::kCompleted) {
                return NotHandled();
            }
            event = Event(meaning == contracts::DeviceStateMeaning::kCompleted ? ProcessEventType::kSortingCompleted
                                                                               : ProcessEventType::kSortingStarted,
                          message, *status->job_id);
        } else if (config_.line_tracer_enabled && message.source_id == config_.line_tracer_device_id) {
            const bool ignorable_sensor_error =
                status->error_code.has_value() && Uppercase(*status->error_code) == "ERR-SENSOR";
            if (status->status != mqtt::ConnectionState::kOnline ||
                (status->error_code.has_value() && !ignorable_sensor_error)) {
                return NotHandled();
            }
            const auto work = machine.FindWork(*status->job_id);
            if (!work.has_value()) {
                return NotHandled();
            }
            if (contracts::HasStateSuffix(current_state, "PICKUP_READY_", 'A', 'C')) {
                if (work->stage != WorkStage::kProductIdentified || DownstreamDevicesBusy(machine, work->work_id)) {
                    return NotHandled();
                }
                ProcessOrchestrationResult pickup_ready{
                    .handled = true,
                    .transition =
                        {
                            .disposition = TransitionDisposition::kApplied,
                            .previous_stage = work->stage,
                            .current_stage = work->stage,
                            .reason = {},
                        },
                    .commands = {},
                };
                if (create_commands) {
                    const auto target =
                        homography_.Enabled() ? gripper_targets_.find(work->work_id) : gripper_targets_.end();
                    pickup_ready.commands.push_back(MakeGripperCommand(
                        work->work_id, work->destination, target == gripper_targets_.end() ? nullptr : &target->second,
                        message.timestamp));
                }
                return pickup_ready;
            }
            if (!contracts::HasStateSuffix(current_state, "LOAD_ON_", 'A', 'C') ||
                (work->stage != WorkStage::kSorting && work->stage != WorkStage::kTransportRequested &&
                 work->stage != WorkStage::kTransporting)) {
                return NotHandled();
            }
            event = Event(ProcessEventType::kTransportStarted, message, *status->job_id);
        } else {
            mapped = false;
        }
    } else if (const auto* completed = mqtt::GetPayload<mqtt::WorkCompletedPayload>(message)) {
        event = Event(completed->result == "SUCCESS" ? ProcessEventType::kWorkCompleted : ProcessEventType::kWorkFailed,
                      message, completed->work_id, completed->message.value_or("work failed"));
    } else if (const auto* error = mqtt::GetPayload<mqtt::ErrorOccurredPayload>(message)) {
        if (config_.line_tracer_enabled && message.source_id == config_.line_tracer_device_id &&
            Uppercase(error->error_code) == "ERR-SENSOR") {
            return NotHandled();
        }
        const std::string reason = error->error_code + ": " + error->message;
        const std::string error_level = Uppercase(error->error_level);
        if (!IsOneOf(error_level, { "ERROR", "CRITICAL" })) {
            return NotHandled();
        }
        if (!error->job_id.has_value()) {
            if (DeviceRoleForSource(config_, message.source_id).has_value()) {
                device_health_.insert_or_assign(message.source_id, false);
            }
            return NotHandled();
        }
        event = Event(ProcessEventType::kWorkFailed, message, *error->job_id, reason);
    } else {
        mapped = false;
    }

    if (!mapped) {
        return NotHandled();
    }

    ProcessOrchestrationResult result{
        .handled = true,
        .transition = machine.Apply(event),
        .commands = {},
    };
    if (result.transition.Applied() && position_target.has_value()) {
        gripper_targets_.insert_or_assign(event.work_id, *position_target);
    }
    if (!config_.line_tracer_enabled && event.type == ProcessEventType::kSortingCompleted &&
        result.transition.Applied()) {
        auto completion = event;
        completion.type = ProcessEventType::kWorkCompleted;
        completion.message_id += "-WITHOUT-LINETRACER";
        completion.source_id = config_.server_id;
        result.transition = machine.Apply(completion);
        if (result.transition.Applied()) {
            gripper_targets_.erase(event.work_id);
        }
    }
    if (!result.transition.Applied() || !create_commands) {
        return result;
    }

    const auto work = machine.FindWork(event.work_id);
    if (!work.has_value()) {
        return result;
    }
    if (event.type == ProcessEventType::kProductInfoReady && !DownstreamDevicesBusy(machine, work->work_id)) {
        AppendDownstreamCommands(result, *work, message.timestamp);
    }
    if (event.type == ProcessEventType::kWorkCompleted) {
        gripper_targets_.erase(event.work_id);
        result.commands.push_back(
            MakeInputConveyorCommand(event.work_id, mqtt::ControlCommand::kStart, message.timestamp));
        // ponytail: the stopped input conveyor leaves at most one routed work waiting; add a persisted FIFO if
        // multiple camera buffers are introduced.
        const auto active_works = machine.ActiveWorks();
        const auto waiting =
            std::ranges::find(active_works, WorkStage::kProductIdentified, &WorkProcessSnapshot::stage);
        if (waiting != active_works.end() && !DownstreamDevicesBusy(machine, waiting->work_id)) {
            AppendDownstreamCommands(result, *waiting, message.timestamp);
        }
    }
    return result;
}

void ProcessOrchestrator::AppendDownstreamCommands(ProcessOrchestrationResult& result, const WorkProcessSnapshot& work,
                                                   std::string_view timestamp) {
    if (config_.line_tracer_enabled) {
        result.commands.push_back(MakeDestinationCommand(work.work_id, work.destination, config_.line_tracer_device_id,
                                                         std::nullopt, timestamp));
        return;
    }
    const auto target = homography_.Enabled() ? gripper_targets_.find(work.work_id) : gripper_targets_.end();
    result.commands.push_back(MakeGripperCommand(
        work.work_id, work.destination, target == gripper_targets_.end() ? nullptr : &target->second, timestamp));
}

void ProcessOrchestrator::RememberDeviceHealth(std::string_view device_id, contracts::DeviceStateMeaning meaning,
                                               mqtt::ConnectionState connection_state,
                                               const std::optional<std::string>& error_code) {
    const bool healthy_state =
        meaning == contracts::DeviceStateMeaning::kIdle || meaning == contracts::DeviceStateMeaning::kWorking ||
        meaning == contracts::DeviceStateMeaning::kStopped || meaning == contracts::DeviceStateMeaning::kCompleted;
    device_health_.insert_or_assign(std::string(device_id), connection_state == mqtt::ConnectionState::kOnline &&
                                                                !error_code.has_value() && healthy_state);
}

ProcessCommandIntent ProcessOrchestrator::MakeInputConveyorCommand(std::string_view work_id,
                                                                   mqtt::ControlCommand command,
                                                                   std::string_view timestamp) {
    const std::string request_id = NextMessageId();
    return {
        .message =
            {
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = request_id,
                .message_type = mqtt::MessageType::kControlCommand,
                .source_id = config_.server_id,
                .timestamp = std::string(timestamp),
                .data =
                    mqtt::ControlCommandPayload{
                        .request_id = request_id,
                        .command = command,
                        .target_device_id = config_.input_device_id,
                        .component_id = "input_conveyor",
                        .params = mqtt::Json{ { "workId", work_id } },
                    },
            },
        .dispatched_event = std::nullopt,
        .work_id = std::string(work_id),
    };
}

ProcessCommandIntent ProcessOrchestrator::MakeSortingControlCommand(std::string_view work_id,
                                                                    mqtt::ControlCommand command,
                                                                    std::string_view component_id,
                                                                    std::string_view timestamp) {
    const std::string request_id = NextMessageId();
    return {
        .message =
            {
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = request_id,
                .message_type = mqtt::MessageType::kControlCommand,
                .source_id = config_.server_id,
                .timestamp = std::string(timestamp),
                .data =
                    mqtt::ControlCommandPayload{
                        .request_id = request_id,
                        .command = command,
                        .target_device_id = config_.sorting_device_id,
                        .component_id = std::string(component_id),
                        .params = mqtt::Json{ { "workId", work_id } },
                    },
            },
        .dispatched_event = std::nullopt,
        .work_id = std::string(work_id),
    };
}

ProcessCommandIntent ProcessOrchestrator::MakeGripperCommand(std::string_view work_id, std::string_view destination,
                                                             const GripperTarget* target, std::string_view timestamp) {
    const std::string request_id = NextMessageId();
    mqtt::Json params{ { "workId", work_id }, { "destination", destination }, { "action", "PICK" } };
    if (target != nullptr) {
        params["coordinateFrame"] = target->coordinate_frame;
        params["unit"] = "mm";
        params["targetPose"] = {
            { "x", target->x_mm }, { "y", target->y_mm }, { "z", target->z_mm },
            { "rollDeg", 180.0 },  { "pitchDeg", 0.0 },   { "yawDeg", target->yaw_deg },
        };
        params["box"] = {
            { "length", target->box_length_mm },
            { "width", target->box_width_mm },
            { "height", target->box_height_mm },
        };
        params["calibrationVersion"] = target->calibration_version;
    }
    return {
        .message =
            {
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = request_id,
                .message_type = mqtt::MessageType::kControlCommand,
                .source_id = config_.server_id,
                .timestamp = std::string(timestamp),
                .data =
                    mqtt::ControlCommandPayload{
                        .request_id = request_id,
                        .command = mqtt::ControlCommand::kExecute,
                        .target_device_id = config_.gripper_device_id,
                        .component_id = "gripper",
                        .params = std::move(params),
                    },
            },
        .dispatched_event = ProcessEventType::kGripperCommandDispatched,
        .work_id = std::string(work_id),
    };
}

ProcessCommandIntent ProcessOrchestrator::MakeDestinationCommand(std::string_view work_id, std::string_view destination,
                                                                 std::string_view target_device_id,
                                                                 std::optional<ProcessEventType> dispatched_event,
                                                                 std::string_view timestamp) {
    const std::string request_id = NextMessageId();
    return {
        .message =
            {
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = request_id,
                .message_type = mqtt::MessageType::kDestinationSet,
                .source_id = config_.server_id,
                .timestamp = std::string(timestamp),
                .data =
                    mqtt::DestinationSetPayload{
                        .request_id = request_id,
                        .work_id = std::string(work_id),
                        .command = mqtt::ControlCommand::kDestinationSet,
                        .target_device_id = std::string(target_device_id),
                        .destination = std::string(destination),
                    },
            },
        .dispatched_event = dispatched_event,
        .work_id = std::string(work_id),
    };
}

std::string ProcessOrchestrator::NextMessageId() {
    const auto sequence = std::to_string(++message_sequence_);
    if (process_epoch_.empty()) {
        return "PROCESS-" + sequence;
    }
    return "PROCESS-" + process_epoch_ + "-" + sequence;
}

}  // namespace logistics::central_server
