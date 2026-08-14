#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "logistics/central_server/homography.hpp"
#include "logistics/central_server/process_state_machine.hpp"
#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct ProcessOrchestratorConfig final {
    bool enabled{ true };
    std::string server_id{ "central-server" };
    std::string input_device_id{ "PI-INPUT-01" };
    std::string vision_device_id{ "PI-VISION-01" };
    std::string gripper_device_id{ "PI-GRIPPER-01" };
    std::string sorting_device_id{ "PI-SORTING-01" };
    std::string line_tracer_device_id{ "PI-LT-01" };
    std::string line_tracer_initial_position;
    std::string default_destination{ "3" };
    HomographyConfig homography;

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ProcessCommandIntent final {
    contracts::mqtt::MqttMessage message;
    std::optional<ProcessEventType> dispatched_event;
    std::string work_id;
    bool dispatch_confirmed{ false };
};

class ProcessCommandTracker final {
public:
    [[nodiscard]] bool Track(const ProcessCommandIntent& intent);
    [[nodiscard]] bool Restore(std::vector<ProcessCommandIntent> intents);
    [[nodiscard]] bool Remove(std::string_view request_id);
    [[nodiscard]] bool MarkDispatched(std::string_view request_id);
    [[nodiscard]] std::optional<ProcessCommandIntent> HandleResponse(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] std::vector<ProcessCommandIntent> PendingCommands() const;
    [[nodiscard]] std::size_t PendingCount() const noexcept;

private:
    std::unordered_map<std::string, ProcessCommandIntent> pending_;
};

struct ProcessOrchestrationResult final {
    bool handled{ false };
    ProcessTransition transition;
    std::vector<ProcessCommandIntent> commands;
};

struct InvalidatedRestoredWork final {
    std::string work_id;
    std::string reason;
};

struct ProcessRestoreResult final {
    bool restored{ false };
    std::vector<InvalidatedRestoredWork> invalidated_works;
};

class ProcessOrchestrator final {
public:
    explicit ProcessOrchestrator(ProcessOrchestratorConfig config = {});

    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] std::string_view VisionDeviceId() const noexcept;
    [[nodiscard]] const ProcessStateMachine& StateMachine() const noexcept;
    [[nodiscard]] ProcessOrchestrationResult Preview(const contracts::mqtt::MqttMessage& message) const;
    [[nodiscard]] ProcessOrchestrationResult Handle(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] ProcessOrchestrationResult BeginWork(std::string_view message_id, std::string_view work_id,
                                                       std::string_view input_device_id, std::string_view timestamp);
    [[nodiscard]] std::vector<ProcessCommandIntent> SortingDetectionCommands(std::string_view work_id,
                                                                             std::string_view timestamp);
    [[nodiscard]] ProcessTransition ConfirmVisionAssignment(std::string_view message_id, std::string_view work_id);
    [[nodiscard]] ProcessTransition ConfirmDispatch(const ProcessCommandIntent& intent);
    [[nodiscard]] ProcessTransition FailDispatch(const ProcessCommandIntent& intent, std::string reason);
    [[nodiscard]] ProcessTransition PreviewSystemCommand(contracts::mqtt::ControlCommand command) const;
    [[nodiscard]] ProcessTransition ApplySystemCommand(contracts::mqtt::ControlCommand command);
    [[nodiscard]] ProcessTransition FailSystemCommand(contracts::mqtt::ControlCommand command, std::string reason);
    [[nodiscard]] ProcessTransition CompleteSystemRecovery();
    [[nodiscard]] ProcessRestoreResult RestoreAfterServerRestart(
        ProcessSystemState stored_state, std::vector<WorkProcessSnapshot> works,
        std::unordered_map<std::string, GripperTarget> gripper_targets, std::uint64_t message_sequence,
        std::vector<std::string> processed_message_ids = {});
    [[nodiscard]] const std::unordered_map<std::string, GripperTarget>& GripperTargets() const noexcept;
    [[nodiscard]] std::uint64_t MessageSequence() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;

private:
    [[nodiscard]] ProcessOrchestrationResult HandleWith(ProcessStateMachine& machine,
                                                        const contracts::mqtt::MqttMessage& message,
                                                        bool create_commands);
    [[nodiscard]] ProcessCommandIntent MakeInputConveyorCommand(std::string_view work_id,
                                                                contracts::mqtt::ControlCommand command,
                                                                std::string_view timestamp);
    [[nodiscard]] ProcessCommandIntent MakeSortingControlCommand(std::string_view work_id,
                                                                 contracts::mqtt::ControlCommand command,
                                                                 std::string_view component_id,
                                                                 std::string_view timestamp);
    [[nodiscard]] ProcessCommandIntent MakeGripperCommand(std::string_view work_id, std::string_view destination,
                                                          const GripperTarget* target, std::string_view timestamp);
    [[nodiscard]] ProcessCommandIntent MakeDestinationCommand(std::string_view work_id, std::string_view destination,
                                                              std::string_view target_device_id,
                                                              std::optional<ProcessEventType> dispatched_event,
                                                              std::string_view timestamp);
    void AppendDownstreamCommands(ProcessOrchestrationResult& result, const WorkProcessSnapshot& work,
                                  std::string_view timestamp);
    void RememberDeviceHealth(std::string_view device_id, contracts::DeviceStateMeaning meaning,
                              contracts::mqtt::ConnectionState connection_state,
                              const std::optional<std::string>& error_code);
    [[nodiscard]] bool AllProcessDevicesHealthy() const;
    [[nodiscard]] std::string NextMessageId();

    ProcessOrchestratorConfig config_;
    HomographyTransformer homography_;
    std::unordered_map<std::string, GripperTarget> gripper_targets_;
    std::unordered_map<std::string, bool> device_health_;
    ProcessStateMachine state_machine_;
    std::uint64_t message_sequence_{};
    std::uint64_t revision_{};
};

}  // namespace logistics::central_server
