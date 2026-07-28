#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "logistics/central_server/process_state_machine.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct ProcessOrchestratorConfig final {
    bool enabled{ false };
    std::string server_id{ "central-server" };
    std::string input_device_id{ "PI-INPUT-01" };
    std::string vision_device_id{ "PI-VISION-01" };
    std::string gripper_device_id{ "PI-GRIPPER-01" };
    std::string sorting_device_id{ "PI-SORTING-01" };
    std::string line_tracer_device_id{ "PI-LT-01" };

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ProcessCommandIntent final {
    contracts::mqtt::MqttMessage message;
    ProcessEventType dispatched_event{ ProcessEventType::kGripperCommandDispatched };
    std::string work_id;
};

struct ProcessOrchestrationResult final {
    bool handled{ false };
    ProcessTransition transition;
    std::vector<ProcessCommandIntent> commands;
};

class ProcessOrchestrator final {
public:
    explicit ProcessOrchestrator(ProcessOrchestratorConfig config = {});

    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] std::string_view VisionDeviceId() const noexcept;
    [[nodiscard]] const ProcessStateMachine& StateMachine() const noexcept;
    [[nodiscard]] ProcessOrchestrationResult Preview(const contracts::mqtt::MqttMessage& message) const;
    [[nodiscard]] ProcessOrchestrationResult Handle(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] ProcessTransition BeginWork(std::string_view message_id, std::string_view work_id,
                                              std::string_view input_device_id);
    [[nodiscard]] ProcessTransition ConfirmVisionAssignment(std::string_view message_id, std::string_view work_id);
    [[nodiscard]] ProcessTransition ConfirmDispatch(const ProcessCommandIntent& intent);
    [[nodiscard]] ProcessTransition FailDispatch(const ProcessCommandIntent& intent, std::string reason);
    [[nodiscard]] ProcessTransition PreviewSystemCommand(contracts::mqtt::ControlCommand command) const;
    [[nodiscard]] ProcessTransition ApplySystemCommand(contracts::mqtt::ControlCommand command);
    [[nodiscard]] ProcessTransition CompleteSystemRecovery();
    [[nodiscard]] bool RestoreAfterServerRestart(ProcessSystemState stored_state,
                                                 std::vector<WorkProcessSnapshot> works,
                                                 std::uint64_t message_sequence);
    [[nodiscard]] std::uint64_t MessageSequence() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;

private:
    [[nodiscard]] ProcessOrchestrationResult HandleWith(ProcessStateMachine& machine,
                                                        const contracts::mqtt::MqttMessage& message,
                                                        bool create_commands);
    [[nodiscard]] ProcessCommandIntent MakeGripperCommand(std::string_view work_id, std::string_view destination,
                                                          std::string_view timestamp);
    [[nodiscard]] ProcessCommandIntent MakeDestinationCommand(std::string_view work_id, std::string_view destination,
                                                              std::string_view target_device_id,
                                                              ProcessEventType dispatched_event,
                                                              std::string_view timestamp);
    [[nodiscard]] std::string NextMessageId();

    ProcessOrchestratorConfig config_;
    ProcessStateMachine state_machine_;
    std::uint64_t message_sequence_{};
    std::uint64_t revision_{};
};

}  // namespace logistics::central_server
