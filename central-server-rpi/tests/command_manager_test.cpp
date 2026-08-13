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
                                            mqtt::ControlCommand command = mqtt::ControlCommand::kStart) {
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
                .component_id = {},
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
    assert(late.disposition == central_server::CommandResponseDisposition::kUnknownRequest);
    assert(!manager.TrackCommand(MakeCommand("REQ-AGGREGATE", "SYSTEM"), { "PI-01", "PI-02" }));
    assert(manager.LastError() == "requestId was already received");
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

}  // namespace

int main() {
    TestCommandTargetsAreResolvedByDeviceAndRole();
    TestLineTracerInitializeIncludesConfiguredPosition();
    TestResponsesAreAggregatedAndDuplicatesIgnored();
    TestDeviceDuplicatedResultIsAggregatedAsSuccess();
    TestPartialDispatchFailureIsIncludedInFinalResult();
    TestNoTargetProducesImmediateRejection();
    TestWrongDeviceIsRejectedAndTimeoutIsGenerated();
    TestEmergencyStopUsesShortConfirmationTimeout();
    TestRecoveryUsesExtendedCompletionTimeout();
    return 0;
}
