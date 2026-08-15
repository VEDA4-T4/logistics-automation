#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "logistics/central_server/command_manager.hpp"
#include "logistics/central_server/database.hpp"
#include "logistics/central_server/process_orchestrator.hpp"

namespace logistics::central_server {

struct StoredProcessState final {
    ProcessSystemState system_state{ ProcessSystemState::kIdle };
    std::uint64_t message_sequence{};
    std::vector<WorkProcessSnapshot> works;
    std::unordered_map<std::string, GripperTarget> gripper_targets;
    std::vector<ProcessCommandIntent> pending_commands;
    std::vector<std::string> processed_message_ids;
    CommandManagerSnapshot command_manager;
    std::unordered_map<std::string, contracts::mqtt::ControlCommand> pending_system_commands;
};

struct PendingMqttDelivery final {
    std::string topic;
    contracts::mqtt::MqttMessage message;
};

[[nodiscard]] bool IsVisionWorkCreatedDelivery(const PendingMqttDelivery& delivery, std::string_view vision_device_id);
[[nodiscard]] std::optional<std::string> AcknowledgedVisionWorkId(const contracts::mqtt::MqttMessage& message,
                                                                  std::string_view vision_device_id);

class ProcessStateStore final {
public:
    explicit ProcessStateStore(Database& database) : database_(database) {}

    [[nodiscard]] DatabaseStatus Load(std::optional<StoredProcessState>& output);
    [[nodiscard]] DatabaseStatus Save(
        ProcessSystemState system_state, std::uint64_t message_sequence, const std::vector<WorkProcessSnapshot>& works,
        const std::unordered_map<std::string, GripperTarget>& gripper_targets,
        const std::vector<ProcessCommandIntent>& pending_commands, std::int64_t updated_at_ms,
        const std::vector<PendingMqttDelivery>& deliveries = {},
        const std::vector<std::string>& processed_message_ids = {}, const CommandManagerSnapshot& command_manager = {},
        const std::unordered_map<std::string, contracts::mqtt::ControlCommand>& pending_system_commands = {});
    [[nodiscard]] DatabaseStatus CommitRecovery(std::uint64_t message_sequence, std::int64_t updated_at_ms,
                                                const std::vector<PendingMqttDelivery>& terminal_deliveries);
    [[nodiscard]] DatabaseStatus LoadPendingMqttDeliveries(std::vector<PendingMqttDelivery>& output);
    [[nodiscard]] DatabaseStatus EnqueueMqttDelivery(std::string_view topic,
                                                     const contracts::mqtt::MqttMessage& message,
                                                     std::int64_t created_at_ms);
    [[nodiscard]] DatabaseStatus EnqueueMqttDeliveries(const std::vector<PendingMqttDelivery>& deliveries,
                                                       std::int64_t created_at_ms);
    [[nodiscard]] DatabaseStatus RemoveMqttDelivery(std::string_view topic, std::string_view message_id);

private:
    [[nodiscard]] DatabaseStatus SaveSnapshot(
        ProcessSystemState system_state, std::uint64_t message_sequence, const std::vector<WorkProcessSnapshot>& works,
        const std::unordered_map<std::string, GripperTarget>& gripper_targets,
        const std::vector<ProcessCommandIntent>& pending_commands, std::int64_t updated_at_ms,
        const std::vector<PendingMqttDelivery>& deliveries, const std::vector<std::string>& processed_message_ids,
        const CommandManagerSnapshot& command_manager,
        const std::unordered_map<std::string, contracts::mqtt::ControlCommand>& pending_system_commands,
        bool replace_mqtt_outbox);

    Database& database_;
};

}  // namespace logistics::central_server
