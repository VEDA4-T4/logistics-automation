#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "logistics/central_server/process_state_store.hpp"

namespace logistics::central_server {

class Application final {
public:
    using RecoveryPersistence =
        std::function<bool(std::uint64_t, std::uint64_t, const std::vector<PendingMqttDelivery>&)>;
    using RecoveryPublisher = std::function<void(const PendingMqttDelivery&)>;

    [[nodiscard]] static bool CommitRecoveryResponse(
        ProcessOrchestrator& process_orchestrator, ProcessCommandTracker& process_command_tracker,
        CommandManager& command_manager,
        std::unordered_map<std::string, contracts::mqtt::ControlCommand>& pending_system_commands,
        const contracts::mqtt::MqttMessage& device_response, std::string_view qt_client_id, std::string_view source_id,
        std::string_view completed_at, const RecoveryPersistence& persist, const RecoveryPublisher& publish);
    [[nodiscard]] static int Run(int argc, char* argv[]);
};

}  // namespace logistics::central_server
