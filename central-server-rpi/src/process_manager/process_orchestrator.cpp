#include "logistics/central_server/process_orchestrator.hpp"

#include <algorithm>
#include <cctype>
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
    if (source_id == config.line_tracer_device_id) {
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

ProcessOrchestrator::ProcessOrchestrator(ProcessOrchestratorConfig config)
    : config_(std::move(config)), homography_(config_.homography) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid process orchestrator device identifier");
    }
}

bool ProcessOrchestrator::Enabled() const noexcept {
    return config_.enabled;
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

ProcessTransition ProcessOrchestrator::BeginWork(std::string_view message_id, std::string_view work_id,
                                                 std::string_view input_device_id) {
    if (!config_.enabled) {
        return {
            .disposition = TransitionDisposition::kApplied,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = {},
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
    return transition;
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

ProcessTransition ProcessOrchestrator::ConfirmDispatch(const ProcessCommandIntent& intent) {
    auto transition = state_machine_.Apply({
        .type = intent.dispatched_event,
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

ProcessTransition ProcessOrchestrator::FailDispatch(const ProcessCommandIntent& intent, std::string reason) {
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
        ++revision_;
    }
    return transition;
}

ProcessRestoreResult ProcessOrchestrator::RestoreAfterServerRestart(
    ProcessSystemState stored_state, std::vector<WorkProcessSnapshot> works,
    std::unordered_map<std::string, GripperTarget> gripper_targets, std::uint64_t message_sequence) {
    std::vector<InvalidatedRestoredWork> invalidated_works;
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
    if (!state_machine_.RestoreAfterServerRestart(stored_state, std::move(works))) {
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
    } else if (const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message)) {
        const std::string current_state = Uppercase(status->current_state);
        const auto role = DeviceRoleForSource(config_, message.source_id);
        const auto meaning = role.has_value() ? contracts::DeviceStateMeaningFor(*role, current_state)
                                              : contracts::DeviceStateMeaning::kUnknown;
        const bool expected_position_reset =
            machine.SystemState() == ProcessSystemState::kRecovery && role == contracts::DeviceRole::kLineTracer &&
            current_state == "POSITION_UNKNOWN" && status->status == mqtt::ConnectionState::kOnline &&
            !status->error_code.has_value() && status->position_reset;
        const bool connection_failure = mqtt::IsConnectionFailure(status->status);
        if (role.has_value() && !expected_position_reset && !connection_failure &&
            meaning == contracts::DeviceStateMeaning::kEmergencyStop) {
            return {
                .handled = true,
                .transition = machine.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop),
                .commands = {},
            };
        }
        if (role.has_value() && !expected_position_reset &&
            (connection_failure || meaning == contracts::DeviceStateMeaning::kError)) {
            return {
                .handled = true,
                .transition = machine.ApplySystemFailure(std::string(contracts::ToString(*role)) +
                                                         " node is unavailable: " + current_state),
                .commands = {},
            };
        }
        if (!status->job_id.has_value()) {
            return NotHandled();
        }
        if (message.source_id == config_.gripper_device_id) {
            if (meaning != contracts::DeviceStateMeaning::kWorking &&
                meaning != contracts::DeviceStateMeaning::kCompleted) {
                return NotHandled();
            }
            event = Event(meaning == contracts::DeviceStateMeaning::kCompleted ? ProcessEventType::kGripperCompleted
                                                                               : ProcessEventType::kGripperStarted,
                          message, *status->job_id);
        } else if (message.source_id == config_.sorting_device_id) {
            if (meaning != contracts::DeviceStateMeaning::kWorking &&
                meaning != contracts::DeviceStateMeaning::kCompleted) {
                return NotHandled();
            }
            event = Event(meaning == contracts::DeviceStateMeaning::kCompleted ? ProcessEventType::kSortingCompleted
                                                                               : ProcessEventType::kSortingStarted,
                          message, *status->job_id);
        } else if (message.source_id == config_.line_tracer_device_id) {
            if (meaning != contracts::DeviceStateMeaning::kWorking) {
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
        const std::string reason = error->error_code + ": " + error->message;
        const std::string error_level = Uppercase(error->error_level);
        if (!IsOneOf(error_level, { "ERROR", "CRITICAL" })) {
            return NotHandled();
        }
        if (!error->job_id.has_value()) {
            return {
                .handled = true,
                .transition = machine.ApplySystemFailure(reason),
                .commands = {},
            };
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
    if (!result.transition.Applied() || !create_commands) {
        return result;
    }

    const auto work = machine.FindWork(event.work_id);
    if (!work.has_value()) {
        return result;
    }
    if (event.type == ProcessEventType::kProductInfoReady) {
        const auto target = homography_.Enabled() ? gripper_targets_.find(work->work_id) : gripper_targets_.end();
        result.commands.push_back(MakeGripperCommand(work->work_id, work->destination,
                                                     target == gripper_targets_.end() ? nullptr : &target->second,
                                                     message.timestamp));
    } else if (event.type == ProcessEventType::kGripperCompleted) {
        result.commands.push_back(MakeDestinationCommand(work->work_id, work->destination, config_.sorting_device_id,
                                                         ProcessEventType::kSortingCommandDispatched,
                                                         message.timestamp));
    } else if (event.type == ProcessEventType::kSortingCompleted) {
        result.commands.push_back(
            MakeDestinationCommand(work->work_id, work->destination, config_.line_tracer_device_id,
                                   ProcessEventType::kTransportCommandDispatched, message.timestamp));
    }
    if (event.type == ProcessEventType::kWorkCompleted) {
        gripper_targets_.erase(event.work_id);
    }
    return result;
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
                        .command = mqtt::ControlCommand::kStart,
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
                                                                 ProcessEventType dispatched_event,
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
    return "PROCESS-" + std::to_string(++message_sequence_);
}

}  // namespace logistics::central_server
