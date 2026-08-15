#include <utility>

#include "logistics/central_server/application.hpp"
#include "logistics/central_server/work_invalidation.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {

std::optional<contracts::mqtt::MqttMessage> Application::StampProcessEpoch(contracts::mqtt::MqttMessage message,
                                                                           std::string_view process_epoch) {
    bool carries_process_epoch = contracts::mqtt::IsProcessScopedMessage(message) ||
                                 message.message_type == contracts::mqtt::MessageType::kEmergencyStop;
    if (const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message);
        command != nullptr && command->command != contracts::mqtt::ControlCommand::kStatusRequest) {
        carries_process_epoch = true;
    }
    if (!carries_process_epoch) {
        return message;
    }
    if (message.process_epoch.has_value() && *message.process_epoch != process_epoch) {
        return std::nullopt;
    }
    message.process_epoch = std::string(process_epoch);
    return message;
}

bool Application::CommitRecoveryResponse(
    ProcessOrchestrator& process_orchestrator, ProcessCommandTracker& process_command_tracker,
    CommandManager& command_manager,
    std::unordered_map<std::string, contracts::mqtt::ControlCommand>& pending_system_commands,
    const contracts::mqtt::MqttMessage& device_response, std::string_view qt_client_id, std::string_view source_id,
    std::string_view completed_at, std::string_view process_epoch, const RecoveryPersistence& persist,
    const RecoveryPublisher& publish) {
    const auto decision = command_manager.PreviewResponse(device_response);
    if (decision.disposition != CommandResponseDisposition::kForward || !decision.message.has_value()) {
        return false;
    }
    const auto* response = contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(*decision.message);
    if (response == nullptr || response->command != contracts::mqtt::ControlCommand::kRecovery ||
        (response->result != contracts::mqtt::CommandResult::kSuccess &&
         response->result != contracts::mqtt::CommandResult::kDuplicated)) {
        return false;
    }
    const auto pending = pending_system_commands.find(response->request_id);
    if (pending == pending_system_commands.end() || pending->second != contracts::mqtt::ControlCommand::kRecovery) {
        return false;
    }

    auto response_message = *decision.message;
    response_message.process_epoch = std::string(process_epoch);
    const std::uint64_t command_message_sequence = command_manager.Snapshot().message_sequence + 1;
    std::vector<PendingMqttDelivery> completions;
    const auto transition =
        process_orchestrator.CommitSystemRecovery([&](const std::vector<WorkProcessSnapshot>& active_works) {
            completions.reserve(active_works.size() + 1);
            completions.push_back({
                .topic = contracts::mqtt::QtResponseTopic(qt_client_id),
                .message = response_message,
            });
            for (const auto& work : active_works) {
                auto completion = MakeWorkFailureCompletion(source_id, "RECOVERY-FAILED-" + work.work_id, work.work_id,
                                                            "CANCELLED_BY_RECOVERY", std::string(completed_at));
                completion.process_epoch = std::string(process_epoch);
                completions.push_back({
                    .topic = contracts::mqtt::QtEventTopic(qt_client_id),
                    .message = std::move(completion),
                });
            }
            return persist(process_orchestrator.MessageSequence(), command_message_sequence, completions);
        });
    if (!transition.Applied()) {
        return false;
    }

    static_cast<void>(command_manager.HandleResponse(device_response));
    process_command_tracker.Clear();
    command_manager.Clear();
    pending_system_commands.clear();
    for (const auto& completion : completions) {
        publish(completion);
    }
    return true;
}

}  // namespace logistics::central_server
