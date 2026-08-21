#include "logistics/central_server/process_state_machine.hpp"

#include <cassert>
#include <string>

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

constexpr auto kWorkOne = "d8e9b2be-bfc0-471c-9000-590123412345";
constexpr auto kWorkTwo = "a8e9b2be-bfc0-471c-9000-590123412346";

central_server::ProcessEvent Event(central_server::ProcessEventType type, std::string message_id,
                                   std::string work_id = kWorkOne, std::string source_id = "PI-VISION-01") {
    return {
        .type = type,
        .message_id = std::move(message_id),
        .work_id = std::move(work_id),
        .source_id = std::move(source_id),
        .destination = {},
        .reason = {},
    };
}

void ApplyNormalFlow(central_server::ProcessStateMachine& machine, std::string_view work_id = kWorkOne) {
    auto created = Event(central_server::ProcessEventType::kWorkCreated, "MSG-WORK", std::string(work_id));
    assert(machine.Apply(created).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kVisionCommandDispatched, "MSG-VISION-COMMAND",
                            std::string(work_id), "central-server"))
               .Applied());

    assert(
        machine.Apply(Event(central_server::ProcessEventType::kPositionDetected, "MSG-POSITION", std::string(work_id)))
            .Applied());
    assert(
        machine.Apply(Event(central_server::ProcessEventType::kBarcodeSucceeded, "MSG-BARCODE", std::string(work_id)))
            .Applied());

    auto product = Event(central_server::ProcessEventType::kProductInfoReady, "MSG-PRODUCT", std::string(work_id),
                         "central-server");
    product.destination = "1";
    assert(machine.Apply(product).Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kGripperCommandDispatched, "MSG-GRIPPER-COMMAND",
                            std::string(work_id), "central-server"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kGripperStarted, "MSG-GRIPPER-START",
                            std::string(work_id), "PI-GRIPPER-01"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kGripperCompleted, "MSG-GRIPPER-DONE",
                            std::string(work_id), "PI-GRIPPER-01"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kSortingCommandDispatched, "MSG-SORT-COMMAND",
                            std::string(work_id), "central-server"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kSortingStarted, "MSG-SORT-START", std::string(work_id),
                            "PI-SORTING-01"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kSortingCompleted, "MSG-SORT-DONE", std::string(work_id),
                            "PI-SORTING-01"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kTransportCommandDispatched, "MSG-TRANSPORT-COMMAND",
                            std::string(work_id), "central-server"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kTransportStarted, "MSG-TRANSPORT-START",
                            std::string(work_id), "PI-LT-01"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kWorkCompleted, "MSG-DONE", std::string(work_id),
                            "PI-LT-01"))
               .Applied());
}

void TestNormalFlowAndInvalidTransitions() {
    central_server::ProcessStateMachine machine;
    assert(machine.SystemState() == central_server::ProcessSystemState::kIdle);

    const auto unknown =
        machine.Apply(Event(central_server::ProcessEventType::kSortingStarted, "MSG-UNKNOWN", kWorkOne));
    assert(unknown.disposition == central_server::TransitionDisposition::kRejected);

    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-CREATE")).Applied());
    const auto out_of_order =
        machine.Apply(Event(central_server::ProcessEventType::kSortingStarted, "MSG-OUT-OF-ORDER"));
    assert(out_of_order.disposition == central_server::TransitionDisposition::kRejected);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kInputDetected);

    const auto duplicate = machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-CREATE"));
    assert(duplicate.disposition == central_server::TransitionDisposition::kDuplicate);
}

void TestCompleteNormalFlow() {
    central_server::ProcessStateMachine machine;
    ApplyNormalFlow(machine);
    const auto work = machine.FindWork(kWorkOne);
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kCompleted);
    assert(work->destination == "1");
    assert(machine.SystemState() == central_server::ProcessSystemState::kIdle);
    assert(machine.ActiveWorks().empty());
}

void TestStopAndRestartRestoreWork() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-STOP-WORK")).Applied());
    assert(
        machine.Apply(Event(central_server::ProcessEventType::kVisionCommandDispatched, "MSG-STOP-VISION")).Applied());
    assert(machine.Apply(Event(central_server::ProcessEventType::kPositionDetected, "MSG-STOP-POSITION")).Applied());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStop).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kStopped);

    const auto blocked = machine.Apply(Event(central_server::ProcessEventType::kBarcodeSucceeded, "MSG-BLOCKED"));
    assert(blocked.disposition == central_server::TransitionDisposition::kRejected);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kVisionProcessing);
}

void TestStartEnablesAnIdleSystem() {
    central_server::ProcessStateMachine machine;
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).disposition ==
           central_server::TransitionDisposition::kDuplicate);
}

void TestErrorEmergencyStopAndRecovery() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-ERROR-WORK")).Applied());
    auto failed = Event(central_server::ProcessEventType::kWorkFailed, "MSG-ERROR");
    failed.reason = "gripper timeout";
    assert(machine.Apply(failed).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kError);
    assert(machine.FindWork(kWorkOne)->failure_reason == "gripper timeout");
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kRecovering);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).disposition ==
           central_server::TransitionDisposition::kDuplicate);
    assert(machine.CompleteSystemRecovery().Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kStopped);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kInitialize).disposition ==
           central_server::TransitionDisposition::kDuplicate);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kInputDetected);

    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kEmergencyStop);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kEmergencyStopped);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kRecovering);
    assert(machine.CompleteSystemRecovery().Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kStopped);
}

void TestParallelWorksRemainIndependent() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-WORK-1", kWorkOne)).Applied());
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-WORK-2", kWorkTwo)).Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kVisionCommandDispatched, "MSG-WORK-1-VISION", kWorkOne,
                            "central-server"))
               .Applied());
    assert(machine.Apply(Event(central_server::ProcessEventType::kBarcodeSucceeded, "MSG-WORK-1-BARCODE", kWorkOne))
               .Applied());
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kBarcodeRecognized);
    assert(machine.FindWork(kWorkTwo)->stage == central_server::WorkStage::kInputDetected);
    assert(machine.ActiveWorks().size() == 2);
}

void TestNodeFailureWithoutWorkStopsTheProcess() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-INPUT-WORK")).Applied());
    assert(machine.ApplySystemFailure("input conveyor fault").Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kError);
    const auto work = machine.FindWork(kWorkOne);
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kFailed);
    assert(work->suspended_stage == central_server::WorkStage::kInputDetected);
    assert(work->failure_reason == "input conveyor fault");
}

void TestServerRestartRestoresSafeState() {
    central_server::ProcessStateMachine machine;
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkOne,
            .stage = central_server::WorkStage::kVisionProcessing,
            .suspended_stage = std::nullopt,
            .destination = "1",
            .last_source_id = "PI-VISION-01",
            .failure_reason = {},
        },
    };
    assert(machine.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning, works));
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kStopped);
    assert(machine.FindWork(kWorkOne)->suspended_stage == central_server::WorkStage::kVisionProcessing);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kVisionProcessing);

    assert(machine.RestoreAfterServerRestart(central_server::ProcessSystemState::kEmergencyStop, works));
    assert(machine.SystemState() == central_server::ProcessSystemState::kEmergencyStop);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kEmergencyStopped);
}

}  // namespace

int main() {
    TestNormalFlowAndInvalidTransitions();
    TestCompleteNormalFlow();
    TestStopAndRestartRestoreWork();
    TestStartEnablesAnIdleSystem();
    TestErrorEmergencyStopAndRecovery();
    TestParallelWorksRemainIndependent();
    TestNodeFailureWithoutWorkStopsTheProcess();
    TestServerRestartRestoresSafeState();
    return 0;
}
