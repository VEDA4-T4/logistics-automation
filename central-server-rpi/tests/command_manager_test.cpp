#include "logistics/central_server/command_manager.hpp"

#include <cassert>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "logistics/contracts/mqtt_validation.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

[[nodiscard]] mqtt::MqttMessage MakeCommand(std::string request_id = "REQ-01", std::string target_device_id = "PI-01",
                                            mqtt::ControlCommand command = mqtt::ControlCommand::kStart,
                                            std::string component_id = {}) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-COMMAND-" + request_id,
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "control-center",
        .timestamp = "2026-07-25T01:00:00Z",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = std::move(request_id),
                .command = command,
                .target_device_id = std::move(target_device_id),
                .component_id = std::move(component_id),
                .params = mqtt::Json::object(),
            },
    };
}

[[nodiscard]] mqtt::MqttMessage MakeResponse(std::string source_id, std::string message_id, std::string request_id,
                                             mqtt::CommandResult result = mqtt::CommandResult::kSuccess,
                                             mqtt::ControlCommand command = mqtt::ControlCommand::kStart) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kCommandResponse,
        .source_id = std::move(source_id),
        .timestamp = "2026-07-25T01:00:01Z",
        .data =
            mqtt::CommandResponsePayload{
                .request_id = std::move(request_id),
                .command = command,
                .result = result,
                .error_code = std::nullopt,
                .message = "done",
            },
    };
}

[[nodiscard]] mqtt::MqttMessage MakeEmergencyStop(std::string request_id = "REQ-ESTOP") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ESTOP-" + request_id,
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "control-center",
        .timestamp = "2026-07-25T01:00:00Z",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = std::move(request_id),
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = "SYSTEM",
            },
    };
}

[[nodiscard]] central_server::DeviceSnapshot Device(std::string id, std::string type,
                                                    mqtt::ConnectionState state = mqtt::ConnectionState::kOnline) {
    central_server::DeviceSnapshot device;
    device.device_id = std::move(id);
    device.device_type = std::move(type);
    device.connection_state = state;
    device.registered = true;
    return device;
}

void TestCommandTargetsAreResolvedByDeviceAndRole() {
    const std::vector devices{
        Device("PI-VISION-01", "vision"),
        Device("PI-SORTING-01", "sorting"),
        Device("PI-LT-01", "linetracer"),
        Device("PI-OFFLINE", "linetracer", mqtt::ConnectionState::kOffline),
    };

    const auto explicit_target = central_server::ResolveCommandTargets(MakeCommand("REQ-1", "PI-VISION-01"), devices);
    assert(explicit_target.target_device_ids == std::vector<std::string>{ "PI-VISION-01" });
    assert(!explicit_target.broadcast);

    const auto system_target = central_server::ResolveCommandTargets(MakeCommand("REQ-2", "SYSTEM"), devices);
    assert(
        (system_target.target_device_ids == std::vector<std::string>{ "PI-LT-01", "PI-SORTING-01", "PI-VISION-01" }));

    const auto emergency_target = central_server::ResolveCommandTargets(MakeEmergencyStop(), devices);
    assert((emergency_target.target_device_ids ==
            std::vector<std::string>{ "PI-LT-01", "PI-SORTING-01", "PI-VISION-01" }));
    assert(emergency_target.broadcast);

    auto destination = MakeCommand("REQ-3", "SYSTEM", mqtt::ControlCommand::kDestinationSet);
    destination.message_type = mqtt::MessageType::kDestinationSet;
    destination.data = mqtt::DestinationSetPayload{
        .request_id = "REQ-3",
        .work_id = "d8e9b2be-bfc0-471c-590123412345",
        .command = mqtt::ControlCommand::kDestinationSet,
        .target_device_id = "SYSTEM",
        .destination = "1",
    };
    const auto destination_target = central_server::ResolveCommandTargets(destination, devices);
    assert(destination_target.target_device_ids == std::vector<std::string>{ "PI-LT-01" });

    auto sorting_destination = destination;
    auto* sorting_payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(sorting_destination);
    assert(sorting_payload != nullptr);
    sorting_payload->target_device_id = "PI-SORTING-01";
    const auto sorting_target = central_server::ResolveCommandTargets(sorting_destination, devices);
    assert(sorting_target.target_device_ids == std::vector<std::string>{ "PI-SORTING-01" });
}

void TestLineTracerInitializeIncludesConfiguredPosition() {
    const auto initialize = MakeCommand("REQ-INIT", "SYSTEM", mqtt::ControlCommand::kInitialize);

    const auto line_tracer = central_server::PrepareCommandForDevice(initialize, "PI-LT-01", "PI-LT-01", "A");
    const auto* line_tracer_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(line_tracer);
    assert(line_tracer.message_id == "MSG-COMMAND-REQ-INIT-PI-LT-01");
    assert(line_tracer_payload != nullptr);
    assert(line_tracer_payload->target_device_id == "PI-LT-01");
    assert(line_tracer_payload->params.at("currentPosition") == "A");

    const auto sorting = central_server::PrepareCommandForDevice(initialize, "PI-SORTING-01", "PI-LT-01", "A");
    const auto* sorting_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(sorting);
    assert(sorting_payload != nullptr);
    assert(sorting_payload->target_device_id == "PI-SORTING-01");
    assert(!sorting_payload->params.contains("currentPosition"));

    const auto* original_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(initialize);
    assert(original_payload != nullptr);
    assert(original_payload->target_device_id == "SYSTEM");
    assert(!original_payload->params.contains("currentPosition"));

    const auto recovery = MakeCommand("REQ-RECOVERY", "SYSTEM", mqtt::ControlCommand::kRecovery);
    const auto line_tracer_recovery = central_server::PrepareCommandForDevice(recovery, "PI-LT-01", "PI-LT-01", "A");
    const auto* recovery_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(line_tracer_recovery);
    assert(recovery_payload != nullptr);
    assert(recovery_payload->params.at("currentPosition") == "A");
}

void TestResponsesAreAggregatedAndDuplicatesIgnored() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-AGGREGATE", "SYSTEM"), { "PI-01", "PI-02" }));

    const auto first = manager.HandleResponse(MakeResponse("PI-01", "RESP-01", "REQ-AGGREGATE"));
    assert(first.disposition == central_server::CommandResponseDisposition::kForward);
    assert(first.message.has_value());
    const auto* progress = mqtt::GetPayload<mqtt::CommandResponsePayload>(*first.message);
    assert(progress != nullptr);
    assert(progress->result == mqtt::CommandResult::kProcessing);
    assert(manager.PendingCount() == 1);

    const auto duplicate = manager.HandleResponse(MakeResponse("PI-01", "RESP-01", "REQ-AGGREGATE"));
    assert(duplicate.disposition == central_server::CommandResponseDisposition::kDuplicate);
    assert(!duplicate.message.has_value());

    const auto second = manager.HandleResponse(MakeResponse("PI-02", "RESP-02", "REQ-AGGREGATE"));
    assert(second.disposition == central_server::CommandResponseDisposition::kForward);
    const auto* completed = mqtt::GetPayload<mqtt::CommandResponsePayload>(*second.message);
    assert(completed != nullptr);
    assert(completed->result == mqtt::CommandResult::kSuccess);
    assert(manager.PendingCount() == 0);

    const auto late = manager.HandleResponse(MakeResponse("PI-02", "RESP-03", "REQ-AGGREGATE"));
    assert(late.disposition == central_server::CommandResponseDisposition::kLateResponse);
    assert(!manager.TrackCommand(MakeCommand("REQ-AGGREGATE", "SYSTEM"), { "PI-01", "PI-02" }));
    assert(manager.LastError() == "requestId was already received");
}

void TestPreviewDoesNotConsumeResponse() {
    central_server::CommandManager manager;
    assert(manager.TrackCommand(MakeCommand("REQ-PREVIEW", "PI-01"), { "PI-01" }));
    const auto preview = manager.PreviewResponse(MakeResponse("PI-01", "RESP-PREVIEW", "REQ-PREVIEW"));
    assert(preview.disposition == central_server::CommandResponseDisposition::kForward);
    assert(preview.message.has_value());
    assert(manager.PendingCount() == 1);
    const auto committed = manager.HandleResponse(MakeResponse("PI-01", "RESP-PREVIEW", "REQ-PREVIEW"));
    assert(committed.disposition == central_server::CommandResponseDisposition::kForward);
    assert(manager.PendingCount() == 0);
}

void TestClearPreservesCommandResultSequence() {
    central_server::CommandManager manager;
    const auto first = manager.MakeImmediateResult(MakeCommand("REQ-SEQUENCE-1"), mqtt::CommandResult::kRejected,
                                                   "2026-08-15T00:00:00Z", std::nullopt, "first");
    assert(first.has_value());
    assert(first->message_id == "COMMAND-RESULT-1");

    manager.Clear();

    const auto second = manager.MakeImmediateResult(MakeCommand("REQ-SEQUENCE-2"), mqtt::CommandResult::kRejected,
                                                    "2026-08-15T00:00:01Z", std::nullopt, "second");
    assert(second.has_value());
    assert(second->message_id == "COMMAND-RESULT-2");
}

void TestSnapshotRestoresAggregateProgress() {
    central_server::CommandManager original;
    assert(original.TrackCommand(MakeCommand("REQ-RESTORE", "SYSTEM"), { "PI-01", "PI-02" }));
    assert(original.HandleResponse(MakeResponse("PI-01", "RESP-RESTORE-1", "REQ-RESTORE")).message.has_value());
    const auto snapshot = original.Snapshot();
    central_server::CommandManager restored;
    assert(restored.Restore(snapshot));
    const auto final = restored.HandleResponse(MakeResponse("PI-02", "RESP-RESTORE-2", "REQ-RESTORE"));
    assert(final.disposition == central_server::CommandResponseDisposition::kForward);
    assert(final.message.has_value());
    assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*final.message)->result == mqtt::CommandResult::kSuccess);
}

void TestPreviewTimeoutDoesNotConsumePendingCommand() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-TIMEOUT-PREVIEW", "PI-01"), { "PI-01" }));
    now += mqtt::kMqttResponseTimeout;
    const auto preview = manager.PreviewTimeouts("2026-08-13T00:00:00Z");
    assert(preview.size() == 1);
    assert(manager.PendingCount() == 1);  // A failed durable enqueue leaves the request retryable.
    assert(manager.CheckTimeouts("2026-08-13T00:00:00Z").size() == 1);
    assert(manager.PendingCount() == 0);
}

void TestDeviceDuplicatedResultIsAggregatedAsSuccess() {
    central_server::CommandManager manager;
    assert(manager.TrackCommand(MakeCommand("REQ-DEVICE-DUPLICATE", "SYSTEM"), { "PI-01", "PI-02" }));

    const auto first = manager.HandleResponse(
        MakeResponse("PI-01", "RESP-DUPLICATE", "REQ-DEVICE-DUPLICATE", mqtt::CommandResult::kDuplicated));
    assert(first.message.has_value());
    assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*first.message)->result == mqtt::CommandResult::kProcessing);

    const auto second = manager.HandleResponse(MakeResponse("PI-02", "RESP-SUCCESS", "REQ-DEVICE-DUPLICATE"));
    assert(second.message.has_value());
    assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*second.message)->result == mqtt::CommandResult::kSuccess);
}

void TestPartialDispatchFailureIsIncludedInFinalResult() {
    central_server::CommandManager manager;
    assert(manager.TrackCommand(MakeCommand("REQ-PARTIAL", "SYSTEM"), { "PI-01", "PI-02" }));

    const auto progress = manager.HandleDispatchFailures("REQ-PARTIAL", { "PI-02" }, "2026-07-25T01:00:01Z");
    assert(progress.has_value());
    const auto* progress_payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(*progress);
    assert(progress_payload != nullptr);
    assert(progress_payload->result == mqtt::CommandResult::kProcessing);
    assert(progress_payload->error_code == std::optional<std::string>("ERR-COMMAND-DISPATCH"));
    assert(manager.PendingCount() == 1);

    const auto response = manager.HandleResponse(MakeResponse("PI-01", "RESP-PARTIAL", "REQ-PARTIAL"));
    assert(response.disposition == central_server::CommandResponseDisposition::kForward);
    assert(response.message.has_value());
    const auto* final_payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(*response.message);
    assert(final_payload != nullptr);
    assert(final_payload->result == mqtt::CommandResult::kFailed);
    assert(final_payload->error_code == std::optional<std::string>("ERR-COMMAND-DISPATCH"));
    assert(manager.PendingCount() == 0);
}

void TestNoTargetProducesImmediateRejection() {
    central_server::CommandManager manager;
    const auto rejected = manager.MakeImmediateResult(MakeCommand("REQ-NO-TARGET"), mqtt::CommandResult::kRejected,
                                                      "2026-07-25T01:00:00Z", std::string("ERR-COMMAND-NO-TARGET"),
                                                      "command has no reachable target devices");
    assert(rejected.has_value());
    const auto* payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(*rejected);
    assert(payload != nullptr);
    assert(payload->request_id == "REQ-NO-TARGET");
    assert(payload->result == mqtt::CommandResult::kRejected);
    assert(payload->error_code == std::optional<std::string>("ERR-COMMAND-NO-TARGET"));
    assert(mqtt::ValidateTopicMessage(mqtt::QtResponseTopic("control-center"), *rejected).IsSuccess());
}

void TestWrongDeviceIsRejectedAndTimeoutIsGenerated() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-TIMEOUT"), { "PI-01" }));

    const auto wrong_device = manager.HandleResponse(MakeResponse("PI-02", "RESP-WRONG", "REQ-TIMEOUT"));
    assert(wrong_device.disposition == central_server::CommandResponseDisposition::kRejected);
    assert(manager.PendingCount() == 1);

    now += mqtt::kMqttResponseTimeout - std::chrono::seconds(1);
    assert(manager.CheckTimeouts("2026-07-25T01:00:02Z").empty());
    now += std::chrono::seconds(1);
    const auto timed_out = manager.CheckTimeouts("2026-07-25T01:00:03Z");
    assert(timed_out.size() == 1);
    const auto* timeout = mqtt::GetPayload<mqtt::CommandResponsePayload>(timed_out[0]);
    assert(timeout != nullptr);
    assert(timeout->request_id == "REQ-TIMEOUT");
    assert(timeout->result == mqtt::CommandResult::kTimeout);
    assert(timeout->error_code == std::optional<std::string>("ERR-COMMAND-TIMEOUT"));
    assert(mqtt::ValidateTopicMessage(mqtt::QtResponseTopic("control-center"), timed_out[0]).IsSuccess());
    assert(manager.PendingCount() == 0);
}

void TestEmergencyStopUsesShortConfirmationTimeout() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeEmergencyStop(), { "PI-01" }));

    now += mqtt::kEmergencyStopConfirmationTimeout;
    const auto timed_out = manager.CheckTimeouts("2026-07-25T01:00:01Z");
    assert(timed_out.size() == 1);
    const auto* timeout = mqtt::GetPayload<mqtt::CommandResponsePayload>(timed_out[0]);
    assert(timeout != nullptr);
    assert(timeout->command == mqtt::ControlCommand::kEmergencyStop);
    assert(timeout->result == mqtt::CommandResult::kTimeout);
}

void TestRecoveryUsesExtendedCompletionTimeout() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-RECOVERY", "PI-01", mqtt::ControlCommand::kRecovery), { "PI-01" }));

    now += mqtt::kMqttResponseTimeout;
    assert(manager.CheckTimeouts("2026-07-25T01:00:03Z").empty());

    now += mqtt::kRecoveryCompletionTimeout - mqtt::kMqttResponseTimeout;
    const auto timed_out = manager.CheckTimeouts("2026-07-25T01:00:30Z");
    assert(timed_out.size() == 1);
    const auto* timeout = mqtt::GetPayload<mqtt::CommandResponsePayload>(timed_out[0]);
    assert(timeout != nullptr);
    assert(timeout->command == mqtt::ControlCommand::kRecovery);
    assert(timeout->result == mqtt::CommandResult::kTimeout);
}

void TestExecuteUsesFullCompletionTimeout() {
    central_server::CommandManager::Clock::time_point silent_now{};
    central_server::CommandManager silent_manager([&silent_now] { return silent_now; });
    assert(silent_manager.TrackCommand(
        MakeCommand("REQ-EXECUTE-SILENT", "PI-GRIPPER-01", mqtt::ControlCommand::kExecute), { "PI-GRIPPER-01" }));
    silent_now += mqtt::kGripperExecuteCompletionTimeout - std::chrono::seconds(1);
    assert(silent_manager.CheckTimeouts("2026-07-25T01:02:59Z").empty());
    silent_now += std::chrono::seconds(1);
    assert(silent_manager.CheckTimeouts("2026-07-25T01:03:00Z").size() == 1);

    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-EXECUTE", "PI-GRIPPER-01", mqtt::ControlCommand::kExecute),
                                { "PI-GRIPPER-01" }));

    now += std::chrono::seconds(2);
    const auto processing =
        manager.HandleResponse(MakeResponse("PI-GRIPPER-01", "RESP-EXECUTE-PROCESSING", "REQ-EXECUTE",
                                            mqtt::CommandResult::kProcessing, mqtt::ControlCommand::kExecute));
    assert(processing.disposition == central_server::CommandResponseDisposition::kForward);

    now += std::chrono::seconds(27);
    assert(manager.CheckTimeouts("2026-07-25T01:00:29Z").empty());

    const auto completed =
        manager.HandleResponse(MakeResponse("PI-GRIPPER-01", "RESP-EXECUTE-SUCCESS", "REQ-EXECUTE",
                                            mqtt::CommandResult::kSuccess, mqtt::ControlCommand::kExecute));
    assert(completed.disposition == central_server::CommandResponseDisposition::kForward);
    assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*completed.message)->result == mqtt::CommandResult::kSuccess);
    assert(manager.PendingCount() == 0);
}

void TestInputStopAcceptsSlowProcessingAndSuccess() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(
        MakeCommand("REQ-INPUT-STOP", "PI-INPUT-01", mqtt::ControlCommand::kStop, "input_conveyor"),
        { "PI-INPUT-01" }));

    now += std::chrono::seconds(3);
    const auto processing =
        manager.HandleResponse(MakeResponse("PI-INPUT-01", "RESP-INPUT-STOP-PROCESSING", "REQ-INPUT-STOP",
                                            mqtt::CommandResult::kProcessing, mqtt::ControlCommand::kStop));
    assert(processing.disposition == central_server::CommandResponseDisposition::kForward);

    now += std::chrono::seconds(8);
    assert(manager.CheckTimeouts("2026-08-15T00:00:11Z").empty());
    const auto success =
        manager.HandleResponse(MakeResponse("PI-INPUT-01", "RESP-INPUT-STOP-SUCCESS", "REQ-INPUT-STOP",
                                            mqtt::CommandResult::kSuccess, mqtt::ControlCommand::kStop));
    assert(success.disposition == central_server::CommandResponseDisposition::kForward);
    assert(success.message.has_value());
    assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*success.message)->result == mqtt::CommandResult::kSuccess);
}

void TestSystemCommandsUseLongestResolvedTargetDeadline() {
    auto destination = MakeCommand("REQ-SYSTEM-DESTINATION", "SYSTEM", mqtt::ControlCommand::kDestinationSet);
    destination.message_type = mqtt::MessageType::kDestinationSet;
    destination.data = mqtt::DestinationSetPayload{
        .request_id = "REQ-SYSTEM-DESTINATION",
        .work_id = "d8e9b2be-bfc0-471c-590123412345",
        .command = mqtt::ControlCommand::kDestinationSet,
        .target_device_id = "SYSTEM",
        .destination = "1",
    };

    central_server::CommandManager::Clock::time_point destination_now{};
    central_server::CommandManager destination_manager([&destination_now] { return destination_now; });
    assert(destination_manager.TrackCommand(destination, { "CUSTOM-LINE-01" }));
    destination_now += std::chrono::milliseconds(4999);
    assert(destination_manager.CheckTimeouts("2026-08-15T00:00:04.999Z").empty());
    destination_now += std::chrono::milliseconds(1);
    assert(destination_manager.CheckTimeouts("2026-08-15T00:00:05Z").size() == 1);

    for (const auto command : { mqtt::ControlCommand::kStart, mqtt::ControlCommand::kStop }) {
        central_server::CommandManager::Clock::time_point now{};
        central_server::CommandManager manager([&now] { return now; });
        assert(manager.TrackCommand(
            MakeCommand("REQ-SYSTEM-LONGEST-" + std::string(mqtt::ToString(command)), "SYSTEM", command),
            { "CUSTOM-A", "CUSTOM-B", "CUSTOM-C" }));
        now += std::chrono::milliseconds(14999);
        assert(manager.CheckTimeouts("2026-08-15T00:00:14.999Z").empty());
        now += std::chrono::milliseconds(1);
        assert(manager.CheckTimeouts("2026-08-15T00:00:15Z").size() == 1);
    }
}

void TestRestorePreservesSubsecondDeadlinePrecision() {
    central_server::CommandManager original;
    assert(original.TrackCommand(MakeCommand("REQ-PRECISE-RESTORE"), { "PI-01" }));
    auto snapshot = original.Snapshot();
    assert(snapshot.pending.size() == 1);
    const auto wall_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    snapshot.pending.front().deadline_at_ms = wall_now + 1500;

    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager restored([&now] { return now; });
    assert(restored.Restore(std::move(snapshot)));
    now += std::chrono::milliseconds(1500);
    assert(restored.CheckTimeouts("2026-08-15T00:00:01.500Z").size() == 1);
}

void TestTimeoutNamesCommandAndMissingDevicesDeterministically() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    auto command = MakeCommand("REQ-MISSING", "PI-INPUT-01", mqtt::ControlCommand::kStop, "input_conveyor");
    command.process_epoch = "123e4567-e89b-42d3-a456-426614174000";
    assert(manager.TrackCommand(command, { "PI-INPUT-03", "PI-INPUT-01", "PI-INPUT-02" }));
    assert(manager
               .HandleResponse(MakeResponse("PI-INPUT-02", "RESP-MISSING-2", "REQ-MISSING",
                                            mqtt::CommandResult::kSuccess, mqtt::ControlCommand::kStop))
               .message.has_value());

    now += std::chrono::seconds(15);
    const auto timed_out = manager.CheckTimeouts("2026-08-15T00:00:15Z");
    assert(timed_out.size() == 1);
    const auto* timeout = mqtt::GetPayload<mqtt::CommandResponsePayload>(timed_out.front());
    assert(timeout != nullptr);
    assert(timeout->command == mqtt::ControlCommand::kStop);
    assert(timeout->error_code == std::optional<std::string>{ "ERR-COMMAND-TIMEOUT" });
    assert(timeout->message == "STOP command timed out waiting for devices: PI-INPUT-01, PI-INPUT-03");
    assert(timed_out.front().process_epoch == command.process_epoch);
}

void TestLateSuccessIsClassifiedAcrossSnapshotRestore() {
    central_server::CommandManager::Clock::time_point now{};
    central_server::CommandManager manager([&now] { return now; });
    assert(manager.TrackCommand(MakeCommand("REQ-LATE", "PI-INPUT-01", mqtt::ControlCommand::kStop, "input_conveyor"),
                                { "PI-INPUT-01" }));
    const auto pending_snapshot = manager.Snapshot();
    assert(pending_snapshot.pending.size() == 1);
    assert(pending_snapshot.pending.front().deadline_at_ms > 0);
    central_server::CommandManager restored_pending([&now] { return now; });
    assert(restored_pending.Restore(pending_snapshot));
    assert(restored_pending.Snapshot().pending.front().deadline_at_ms ==
           pending_snapshot.pending.front().deadline_at_ms);

    now += std::chrono::seconds(15);
    assert(manager.CheckTimeouts("2026-08-15T00:00:15Z").size() == 1);
    central_server::CommandManager restored([&now] { return now; });
    assert(restored.Restore(manager.Snapshot()));

    const auto late = restored.HandleResponse(MakeResponse("PI-INPUT-01", "RESP-LATE-SUCCESS", "REQ-LATE",
                                                           mqtt::CommandResult::kSuccess, mqtt::ControlCommand::kStop));
    assert(late.disposition == central_server::CommandResponseDisposition::kLateResponse);
    assert(late.message.has_value());
    assert(late.message->message_id == "RESP-LATE-SUCCESS");
    assert(restored.PendingCount() == 0);
}

void TestCompletedRequestMemoryIsBounded() {
    central_server::CommandManager manager;
    for (int index = 0; index < 300; ++index) {
        const std::string request_id = "REQ-COMPLETED-" + std::to_string(index);
        assert(manager
                   .MakeImmediateResult(MakeCommand(request_id), mqtt::CommandResult::kRejected, "2026-08-15T00:00:00Z",
                                        std::nullopt, "rejected")
                   .has_value());
    }

    const auto snapshot = manager.Snapshot();
    assert(snapshot.completed_requests.size() == 256);
    assert(manager.HandleResponse(MakeResponse("PI-01", "RESP-FORGOTTEN", "REQ-COMPLETED-0")).disposition ==
           central_server::CommandResponseDisposition::kUnknownRequest);
    assert(manager.HandleResponse(MakeResponse("PI-01", "RESP-REMEMBERED", "REQ-COMPLETED-299")).disposition ==
           central_server::CommandResponseDisposition::kLateResponse);
}

}  // namespace

int main() {
    TestPreviewDoesNotConsumeResponse();
    TestClearPreservesCommandResultSequence();
    TestSnapshotRestoresAggregateProgress();
    TestPreviewTimeoutDoesNotConsumePendingCommand();
    TestCommandTargetsAreResolvedByDeviceAndRole();
    TestLineTracerInitializeIncludesConfiguredPosition();
    TestResponsesAreAggregatedAndDuplicatesIgnored();
    TestDeviceDuplicatedResultIsAggregatedAsSuccess();
    TestPartialDispatchFailureIsIncludedInFinalResult();
    TestNoTargetProducesImmediateRejection();
    TestWrongDeviceIsRejectedAndTimeoutIsGenerated();
    TestEmergencyStopUsesShortConfirmationTimeout();
    TestRecoveryUsesExtendedCompletionTimeout();
    TestExecuteUsesFullCompletionTimeout();
    TestInputStopAcceptsSlowProcessingAndSuccess();
    TestRestorePreservesSubsecondDeadlinePrecision();
    TestSystemCommandsUseLongestResolvedTargetDeadline();
    TestTimeoutNamesCommandAndMissingDevicesDeterministically();
    TestLateSuccessIsClassifiedAcrossSnapshotRestore();
    TestCompletedRequestMemoryIsBounded();
    return 0;
}
