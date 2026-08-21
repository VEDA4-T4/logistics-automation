#include "logistics/central_server/process_state_machine.hpp"

#include <cassert>
#include <string>

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

constexpr auto kWorkOne = "d8e9b2be-bfc0-471c-9000-590123412345";
constexpr auto kWorkTwo = "a8e9b2be-bfc0-471c-9000-590123412346";
constexpr auto kWorkThree = "b8e9b2be-bfc0-471c-9000-590123412347";
constexpr auto kWorkFour = "c8e9b2be-bfc0-471c-9000-590123412348";
constexpr auto kWorkFive = "e8e9b2be-bfc0-471c-9000-590123412349";
constexpr auto kWorkSix = "f8e9b2be-bfc0-471c-9000-590123412350";

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

central_server::WorkProcessSnapshot WorkAtStage(std::string work_id, central_server::WorkStage stage) {
    return {
        .work_id = std::move(work_id),
        .stage = stage,
        .suspended_stage = std::nullopt,
        .destination = {},
        .last_source_id = {},
        .failure_reason = {},
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
    assert(machine.FindWork(work_id)->stage == central_server::WorkStage::kTransporting);
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

void TestErrorRecovery() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-ERROR-WORK")).Applied());
    assert(machine.ApplySystemFailure("gripper timeout").Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kError);
    assert(machine.FindWork(kWorkOne)->failure_reason == "gripper timeout");
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kRecovering);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).disposition ==
           central_server::TransitionDisposition::kDuplicate);
    assert(machine.CompleteSystemRecovery().Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(!machine.FindWork(kWorkOne).has_value());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kInitialize).disposition ==
           central_server::TransitionDisposition::kDuplicate);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
}

void TestWorkFailureKeepsProcessRunning() {
    central_server::ProcessStateMachine machine;
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-WORK-FAILURE")).Applied());

    auto failed = Event(central_server::ProcessEventType::kWorkFailed, "MSG-WORK-FAILURE-EVENT");
    failed.reason = "input conveyor stop timed out";
    assert(machine.Apply(failed).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
    assert(machine.AcceptsNewWork());
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kFailed);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRecovery);
}

void TestAcceptsNewWorkOnlyWhileIdleOrRunning() {
    central_server::ProcessStateMachine machine;
    assert(machine.AcceptsNewWork());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStop).Applied());
    assert(!machine.AcceptsNewWork());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.AcceptsNewWork());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(!machine.AcceptsNewWork());
}

void TestBarcodeFailureStopsAndDiscardsTheWork() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-BARCODE-WORK")).Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kVisionCommandDispatched, "MSG-BARCODE-DISPATCH",
                            kWorkOne, "central-server"))
               .Applied());
    assert(machine
               .Apply(Event(central_server::ProcessEventType::kPositionDetected, "MSG-BARCODE-POSITION", kWorkOne,
                            "PI-VISION-01"))
               .Applied());

    auto failed =
        Event(central_server::ProcessEventType::kBarcodeFailed, "MSG-BARCODE-FAILED", kWorkOne, "PI-VISION-01");
    failed.reason = "barcode region was not detected";
    const auto transition = machine.Apply(failed);

    assert(transition.Applied());
    assert(transition.current_stage == central_server::WorkStage::kFailed);
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
    assert(!machine.FindWork(kWorkOne).has_value());
    assert(machine.ActiveWorks().empty());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.SystemState() == central_server::ProcessSystemState::kRunning);
}

void TestBarcodeFailureSuspendsOtherActiveWork() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-BARCODE-WORK-ONE")).Applied());
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-BARCODE-WORK-TWO", kWorkTwo))
               .Applied());

    auto failed =
        Event(central_server::ProcessEventType::kBarcodeFailed, "MSG-BARCODE-FAILED-ONE", kWorkOne, "PI-VISION-01");
    failed.reason = "barcode region was not detected";
    assert(machine.Apply(failed).Applied());

    assert(!machine.FindWork(kWorkOne).has_value());
    const auto remaining = machine.FindWork(kWorkTwo);
    assert(remaining.has_value());
    assert(remaining->stage == central_server::WorkStage::kStopped);
    assert(remaining->suspended_stage == central_server::WorkStage::kInputDetected);

    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    const auto restored = machine.FindWork(kWorkTwo);
    assert(restored.has_value());
    assert(restored->stage == central_server::WorkStage::kInputDetected);
    assert(!restored->suspended_stage.has_value());
}

void TestRecoveryDiscardsAllActiveWork() {
    central_server::ProcessStateMachine machine;
    std::vector works{
        WorkAtStage(kWorkOne, central_server::WorkStage::kInputDetected),
        WorkAtStage(kWorkTwo, central_server::WorkStage::kVisionAssigned),
        WorkAtStage(kWorkThree, central_server::WorkStage::kBarcodeRecognized),
        WorkAtStage(kWorkFour, central_server::WorkStage::kGripperTransferring),
        WorkAtStage(kWorkFive, central_server::WorkStage::kSorting),
        WorkAtStage(kWorkSix, central_server::WorkStage::kTransporting),
    };

    assert(machine.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning, std::move(works)));
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.ActiveWorks().size() == 6);
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(machine.CompleteSystemRecovery().Applied());
    assert(machine.ActiveWorks().empty());
    assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);

    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(machine.ActiveWorks().empty());
}

void TestRecoveryCompletionClearsProcessedMessages() {
    central_server::ProcessStateMachine machine;
    assert(machine.Apply(Event(central_server::ProcessEventType::kWorkCreated, "MSG-BEFORE-RECOVERY")).Applied());
    assert(machine.ProcessedMessageIds() == std::vector<std::string>{ "MSG-BEFORE-RECOVERY" });
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(machine.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());

    assert(machine.CompleteSystemRecovery().Applied());

    assert(machine.ProcessedMessageIds().empty());
}

void TestServerRestartSuspendsTransportRequest() {
    central_server::ProcessStateMachine machine;
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkOne,
            .stage = central_server::WorkStage::kTransportRequested,
            .suspended_stage = std::nullopt,
            .destination = "1",
            .last_source_id = "PI-LT-01",
            .failure_reason = {},
        },
        central_server::WorkProcessSnapshot{
            .work_id = kWorkTwo,
            .stage = central_server::WorkStage::kStopped,
            .suspended_stage = central_server::WorkStage::kVisionProcessing,
            .destination = {},
            .last_source_id = "PI-VISION-01",
            .failure_reason = {},
        },
    };

    assert(machine.RestoreAfterServerRestart(central_server::ProcessSystemState::kStopped, std::move(works)));
    assert(machine.FindWork(kWorkOne)->stage == central_server::WorkStage::kStopped);
    assert(machine.FindWork(kWorkOne)->suspended_stage == central_server::WorkStage::kTransportRequested);
    assert(machine.FindWork(kWorkTwo)->stage == central_server::WorkStage::kStopped);
    assert(machine.FindWork(kWorkTwo)->suspended_stage == central_server::WorkStage::kVisionProcessing);
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
    TestAcceptsNewWorkOnlyWhileIdleOrRunning();
    TestErrorRecovery();
    TestWorkFailureKeepsProcessRunning();
    TestBarcodeFailureStopsAndDiscardsTheWork();
    TestBarcodeFailureSuspendsOtherActiveWork();
    TestServerRestartSuspendsTransportRequest();
    TestRecoveryDiscardsAllActiveWork();
    TestRecoveryCompletionClearsProcessedMessages();
    TestParallelWorksRemainIndependent();
    TestNodeFailureWithoutWorkStopsTheProcess();
    TestServerRestartRestoresSafeState();
    return 0;
}
