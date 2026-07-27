#include "logistics/central_server/process_orchestrator.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

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

[[nodiscard]] bool IsConnectionFailure(mqtt::ConnectionState state) noexcept {
    return state == mqtt::ConnectionState::kOffline || state == mqtt::ConnectionState::kRtspError ||
           state == mqtt::ConnectionState::kMqttError || state == mqtt::ConnectionState::kMqttAuthError ||
           state == mqtt::ConnectionState::kTlsError || state == mqtt::ConnectionState::kUartError;
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
    return mqtt::IsValidTopicLevel(server_id) && mqtt::IsValidTopicLevel(input_device_id) &&
           mqtt::IsValidTopicLevel(vision_device_id) && mqtt::IsValidTopicLevel(gripper_device_id) &&
           mqtt::IsValidTopicLevel(sorting_device_id) && mqtt::IsValidTopicLevel(line_tracer_device_id);
}

ProcessOrchestrator::ProcessOrchestrator(ProcessOrchestratorConfig config) : config_(std::move(config)) {
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
    return HandleWith(state_machine_, message, true);
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
    return state_machine_.Apply({
        .type = ProcessEventType::kWorkCreated,
        .message_id = std::string(message_id),
        .work_id = std::string(work_id),
        .source_id = std::string(input_device_id),
        .destination = {},
        .reason = {},
    });
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
    return state_machine_.Apply({
        .type = ProcessEventType::kVisionCommandDispatched,
        .message_id = std::string(message_id) + "-VISION-DISPATCHED",
        .work_id = std::string(work_id),
        .source_id = config_.server_id,
        .destination = {},
        .reason = {},
    });
}

ProcessTransition ProcessOrchestrator::ConfirmDispatch(const ProcessCommandIntent& intent) {
    return state_machine_.Apply({
        .type = intent.dispatched_event,
        .message_id = intent.message.message_id + "-DISPATCHED",
        .work_id = intent.work_id,
        .source_id = config_.server_id,
        .destination = {},
        .reason = {},
    });
}

ProcessTransition ProcessOrchestrator::FailDispatch(const ProcessCommandIntent& intent, std::string reason) {
    return state_machine_.Apply({
        .type = ProcessEventType::kWorkFailed,
        .message_id = intent.message.message_id + "-FAILED",
        .work_id = intent.work_id,
        .source_id = config_.server_id,
        .destination = {},
        .reason = std::move(reason),
    });
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
    return state_machine_.ApplySystemCommand(command);
}

ProcessOrchestrationResult ProcessOrchestrator::HandleWith(ProcessStateMachine& machine,
                                                           const mqtt::MqttMessage& message, bool create_commands) {
    ProcessEvent event;
    bool mapped = true;

    if (const auto* position = mqtt::GetPayload<mqtt::PositionDetectedPayload>(message)) {
        event = Event(ProcessEventType::kPositionDetected, message, position->work_id);
    } else if (const auto* barcode = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(message)) {
        const bool succeeded = barcode->recognition_status == "SUCCESS";
        event = Event(succeeded ? ProcessEventType::kBarcodeSucceeded : ProcessEventType::kBarcodeFailed, message,
                      barcode->work_id, barcode->message.value_or("barcode recognition failed"));
    } else if (const auto* product = mqtt::GetPayload<mqtt::ProductInfoPayload>(message)) {
        const bool succeeded = product->recognition_status == "SUCCESS" && !product->destination.empty();
        event = Event(succeeded ? ProcessEventType::kProductInfoReady : ProcessEventType::kProductInfoFailed, message,
                      product->work_id, product->message.value_or("product information is incomplete"));
        event.destination = product->destination;
    } else if (const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message)) {
        const std::string current_state = Uppercase(status->current_state);
        if (message.source_id == config_.input_device_id &&
            (IsConnectionFailure(status->status) || IsOneOf(current_state, { "FAULT", "ERROR", "ESTOP" }))) {
            return {
                .handled = true,
                .transition = machine.ApplySystemFailure("input node is unavailable: " + current_state),
                .commands = {},
            };
        }
        if (!status->job_id.has_value()) {
            return NotHandled();
        }
        if (message.source_id == config_.gripper_device_id) {
            event =
                Event(IsOneOf(current_state, { "COMPLETED", "PLACED", "READY" }) ? ProcessEventType::kGripperCompleted
                                                                                 : ProcessEventType::kGripperStarted,
                      message, *status->job_id);
        } else if (message.source_id == config_.sorting_device_id) {
            event = Event(IsOneOf(current_state, { "COMPLETED", "CYCLE_COMPLETE", "HOME" })
                              ? ProcessEventType::kSortingCompleted
                              : ProcessEventType::kSortingStarted,
                          message, *status->job_id);
        } else if (message.source_id == config_.line_tracer_device_id) {
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
    if (!result.transition.Applied() || !create_commands) {
        return result;
    }

    const auto work = machine.FindWork(event.work_id);
    if (!work.has_value()) {
        return result;
    }
    if (event.type == ProcessEventType::kProductInfoReady) {
        result.commands.push_back(MakeGripperCommand(work->work_id, work->destination, message.timestamp));
    } else if (event.type == ProcessEventType::kGripperCompleted) {
        result.commands.push_back(MakeDestinationCommand(work->work_id, work->destination, config_.sorting_device_id,
                                                         ProcessEventType::kSortingCommandDispatched,
                                                         message.timestamp));
    } else if (event.type == ProcessEventType::kSortingCompleted) {
        result.commands.push_back(
            MakeDestinationCommand(work->work_id, work->destination, config_.line_tracer_device_id,
                                   ProcessEventType::kTransportCommandDispatched, message.timestamp));
    }
    return result;
}

ProcessCommandIntent ProcessOrchestrator::MakeGripperCommand(std::string_view work_id, std::string_view destination,
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
                        .command = mqtt::ControlCommand::kStart,
                        .target_device_id = config_.gripper_device_id,
                        .component_id = "gripper",
                        .params = mqtt::Json{ { "workId", work_id }, { "destination", destination } },
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
