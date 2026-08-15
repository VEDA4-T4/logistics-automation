#include "logistics/device/sorting_node.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/contracts/uart/conveyor_events.h"
#include "logistics/contracts/uart_codec.h"

namespace {

namespace mqtt = logistics::contracts::mqtt;
using logistics::device::SortingCommandStatus;
using logistics::device::SortingNode;
using logistics::device::SortingReport;
using logistics::device::SortingReportChannel;
using logistics::device::UartIoBackend;
using logistics::device::UartIoResult;
using logistics::device::UartIoStatus;
using logistics::device::UartSession;

inline constexpr std::string_view kWorkId = "8cd62467-5b68-4ea0-8946-b979495ea71c";

struct ReadAction {
    UartIoResult result;
    std::vector<std::uint8_t> data;
};

class FakeUartIoBackend final : public UartIoBackend {
public:
    UartIoResult Open(std::string_view) override {
        open = true;
        return { UartIoStatus::kSuccess, 0, 0 };
    }

    void Close() noexcept override {
        open = false;
    }

    bool IsOpen() const noexcept override {
        return open;
    }

    UartIoResult Read(std::span<std::uint8_t> buffer) override {
        if (reads.empty()) {
            return { UartIoStatus::kWouldBlock, 0, 0 };
        }
        ReadAction action = std::move(reads.front());
        reads.pop_front();
        if (action.result.Succeeded()) {
            assert(action.data.size() <= buffer.size());
            std::copy(action.data.begin(), action.data.end(), buffer.begin());
        }
        return action.result;
    }

    UartIoResult Write(std::span<const std::uint8_t> data) override {
        writes.emplace_back(data.begin(), data.end());
        return { UartIoStatus::kSuccess, data.size(), 0 };
    }

    UartIoResult WaitReadable(std::chrono::milliseconds) override {
        return { UartIoStatus::kTimeout, 0, 0 };
    }

    UartIoResult WaitWritable(std::chrono::milliseconds) override {
        return { UartIoStatus::kTimeout, 0, 0 };
    }

    void PushRead(std::vector<std::uint8_t> bytes) {
        reads.push_back({ { UartIoStatus::kSuccess, bytes.size(), 0 }, std::move(bytes) });
    }

    bool open{};
    std::deque<ReadAction> reads;
    std::vector<std::vector<std::uint8_t>> writes;
};

std::vector<std::uint8_t> Encode(const uart_frame_t& frame) {
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length = 0U;
    assert(uart_encode_frame(&frame, encoded.data(), encoded.size(), &length) == UART_CODEC_OK);
    return { encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(length) };
}

struct Fixture {
    Fixture() {
        auto owned_backend = std::make_unique<FakeUartIoBackend>();
        backend = owned_backend.get();
        session = std::make_unique<UartSession>(std::move(owned_backend));
        assert(session->Open());
        node = std::make_unique<SortingNode>("PI-SORT-01", *session);
        node->SetReportHandler([this](const SortingReport& report) { reports.push_back(report); });
        session->SetEventHandler(
            [this](const logistics::device::UartSessionEvent& event) { node->HandleUartEvent(event); });
    }

    [[nodiscard]] uart_frame_t LastCommand() const {
        assert(!backend->writes.empty());
        uart_frame_t frame{};
        const auto& bytes = backend->writes.back();
        assert(uart_decode_frame(bytes.data(), bytes.size(), &frame) == UART_CODEC_OK);
        return frame;
    }

    void PushOperationResult(std::uint8_t status = UART_STATUS_SUCCESS, std::uint8_t error = UART_ERROR_NONE) {
        const uart_frame_t command = LastCommand();
        uart_frame_t response{};
        response.version = UART_PROTOCOL_VERSION;
        response.sequence = command.sequence;
        response.command = UART_CMD_OPERATION_RESULT;
        response.length = UART_OPERATION_RESULT_PAYLOAD_SIZE;
        response.payload[UART_OPERATION_RESULT_STATUS_INDEX] = status;
        response.payload[UART_OPERATION_RESULT_ERROR_INDEX] = error;
        backend->PushRead(Encode(response));
        assert(session->PollOnce().Succeeded());
    }

    void PushSortingStatus(std::uint8_t gate_state, std::uint16_t cycle_id, std::uint8_t destination) {
        const uart_frame_t command = LastCommand();
        uart_frame_t response{};
        response.version = UART_PROTOCOL_VERSION;
        response.sequence = command.sequence;
        response.command = UART_CMD_RESPONSE;
        response.length = UART_SORTING_STATUS_PAYLOAD_SIZE;
        response.payload[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_SUCCESS;
        response.payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_SORTING_GET_STATUS;
        response.payload[UART_RESPONSE_ERROR_INDEX] = UART_ERROR_NONE;
        response.payload[UART_SORTING_STATUS_GATE_STATE_INDEX] = gate_state;
        response.payload[UART_SORTING_STATUS_CYCLE_ID_LOW_INDEX] = static_cast<std::uint8_t>(cycle_id & 0xffU);
        response.payload[UART_SORTING_STATUS_CYCLE_ID_HIGH_INDEX] = static_cast<std::uint8_t>((cycle_id >> 8U) & 0xffU);
        response.payload[UART_SORTING_STATUS_DESTINATION_INDEX] = destination;
        backend->PushRead(Encode(response));
        assert(session->PollOnce().Succeeded());
    }

    void PushConveyorStatus(std::uint8_t state, std::uint8_t speed) {
        const uart_frame_t command = LastCommand();
        uart_frame_t response{};
        response.version = UART_PROTOCOL_VERSION;
        response.sequence = command.sequence;
        response.command = UART_CMD_RESPONSE;
        response.length = UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE;
        response.payload[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_SUCCESS;
        response.payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_SORTING_CONVEYOR_GET_STATUS;
        response.payload[UART_RESPONSE_ERROR_INDEX] = UART_ERROR_NONE;
        response.payload[UART_SORTING_CONVEYOR_STATUS_STATE_INDEX] = state;
        response.payload[UART_SORTING_CONVEYOR_STATUS_SPEED_INDEX] = speed;
        backend->PushRead(Encode(response));
        assert(session->PollOnce().Succeeded());
    }

    void PushCycleComplete(std::uint16_t cycle_id, std::uint8_t destination) {
        uart_frame_t event{};
        event.version = UART_PROTOCOL_VERSION;
        event.sequence = next_event_sequence++;
        event.command = UART_CMD_EVENT;
        event.length = UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE;
        event.payload[UART_EVENT_ID_INDEX] = UART_SORTING_EVENT_CYCLE_COMPLETE;
        event.payload[UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX] = static_cast<std::uint8_t>(cycle_id & 0xffU);
        event.payload[UART_SORTING_EVENT_CYCLE_ID_HIGH_INDEX] = static_cast<std::uint8_t>((cycle_id >> 8U) & 0xffU);
        event.payload[UART_SORTING_EVENT_DESTINATION_INDEX] = destination;
        backend->PushRead(Encode(event));
        assert(session->PollOnce().Succeeded());
    }

    void PushSensorStatus(std::uint8_t sensor_id, std::uint8_t state, std::uint16_t distance_cm) {
        uart_frame_t status{};
        status.version = UART_PROTOCOL_VERSION;
        status.sequence = next_event_sequence++;
        status.command = UART_CMD_SENSOR_STATUS;
        status.length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
        status.payload[UART_SENSOR_ID_INDEX] = sensor_id;
        status.payload[UART_SENSOR_STATE_INDEX] = state;
        status.payload[UART_SENSOR_DISTANCE_CM_LOW_INDEX] = static_cast<std::uint8_t>(distance_cm & 0xffU);
        status.payload[UART_SENSOR_DISTANCE_CM_HIGH_INDEX] = static_cast<std::uint8_t>((distance_cm >> 8U) & 0xffU);
        backend->PushRead(Encode(status));
        assert(session->PollOnce().Succeeded());
    }

    void PushControllerEvent(std::uint8_t event_id, std::uint8_t kind, std::uint8_t cause, std::uint8_t result = 0U,
                             std::uint8_t sensor_id = 0xFFU) {
        uart_frame_t event{};
        event.version = UART_PROTOCOL_VERSION;
        event.sequence = next_event_sequence++;
        event.command = UART_CMD_EVENT;
        event.length = APP_SAFETY_EVENT_PAYLOAD_SIZE;
        event.payload[UART_EVENT_ID_INDEX] = event_id;
        event.payload[APP_SAFETY_EVENT_KIND_INDEX] = kind;
        event.payload[APP_SAFETY_EVENT_CAUSE_INDEX] = cause;
        if (event_id == APP_EVENT_SAFETY) {
            event.payload[APP_SAFETY_EVENT_RESULT_INDEX] = result;
        } else {
            event.payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX] = sensor_id;
        }
        backend->PushRead(Encode(event));
        assert(session->PollOnce().Succeeded());
    }

    void PushHeartbeat(std::uint8_t state, std::uint8_t error) {
        uart_frame_t event{};
        event.version = UART_PROTOCOL_VERSION;
        event.sequence = next_event_sequence++;
        event.command = UART_CMD_EVENT;
        event.length = APP_HEARTBEAT_PAYLOAD_SIZE;
        event.payload[UART_EVENT_ID_INDEX] = APP_EVENT_HEARTBEAT;
        event.payload[APP_HEARTBEAT_STATE_INDEX] = state;
        event.payload[APP_HEARTBEAT_ERROR_INDEX] = error;
        backend->PushRead(Encode(event));
        assert(session->PollOnce().Succeeded());
    }

    FakeUartIoBackend* backend{};
    std::unique_ptr<UartSession> session;
    std::unique_ptr<SortingNode> node;
    std::vector<SortingReport> reports;
    std::uint8_t next_event_sequence{ 100U };
};

template <typename Payload>
const Payload& ReportPayload(const SortingReport& report) {
    const auto* payload = std::get_if<Payload>(&report.data);
    assert(payload != nullptr);
    return *payload;
}

mqtt::MqttMessage MakeDestination(std::string destination = "2", std::string target = "PI-SORT-01",
                                  std::string message_id = "MSG-DEST-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kDestinationSet,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-24T10:00:00+09:00",
        .data =
            mqtt::DestinationSetPayload{
                .request_id = "REQ-DEST-01",
                .work_id = std::string(kWorkId),
                .command = mqtt::ControlCommand::kDestinationSet,
                .target_device_id = std::move(target),
                .destination = std::move(destination),
            },
    };
}

mqtt::MqttMessage MakeControl(mqtt::ControlCommand command, std::string component = {},
                              mqtt::Json params = mqtt::Json::object(), std::string target = "PI-SORT-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-CONTROL-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-24T10:00:01+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "REQ-CONTROL-01",
                .command = command,
                .target_device_id = std::move(target),
                .component_id = std::move(component),
                .params = std::move(params),
            },
    };
}

mqtt::MqttMessage MakeEmergencyStop() {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ESTOP-01",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-24T10:00:02+09:00",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "REQ-ESTOP-01",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = "PI-SORT-01",
            },
    };
}

void ActivateCycle(Fixture& fixture) {
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    fixture.PushOperationResult();
    fixture.reports.clear();
}

void TestDestinationMapsToRouteItemAndActivatesAfterSuccess() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeDestination("DEST-02"));
    const uart_frame_t frame = fixture.LastCommand();

    assert(result.Succeeded());
    assert(result.uart_cycle_id == 1U);
    assert(result.uart_destination == UART_SORTING_DESTINATION_2);
    assert(frame.command == UART_CMD_SORTING_ROUTE_ITEM);
    assert(frame.length == UART_SORTING_ROUTE_PAYLOAD_SIZE);
    assert(uart_sorting_route_cycle_id(frame.payload) == 1U);
    assert(frame.payload[UART_SORTING_ROUTE_DESTINATION_INDEX] == UART_SORTING_DESTINATION_2);
    assert(!fixture.node->HasActiveCycle());

    fixture.PushOperationResult();

    assert(fixture.node->HasActiveCycle());
    assert(fixture.node->ActiveWorkId() == kWorkId);
    assert(fixture.reports.size() == 2U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.back());
    assert(response.result == mqtt::CommandResult::kSuccess);
}

void TestInvalidDestinationAndWrongTargetDoNotWrite() {
    Fixture fixture;

    assert(fixture.node->HandleMqttCommand(MakeDestination("WAREHOUSE-99")).status ==
           SortingCommandStatus::kInvalidDestination);
    assert(fixture.node->HandleMqttCommand(MakeDestination("1", "PI-SORT-02")).status ==
           SortingCommandStatus::kInvalidTarget);
    assert(fixture.backend->writes.empty());
}

void TestDuplicateAndConflictingWorkAreRejectedWithoutSecondMotion() {
    Fixture fixture;
    ActivateCycle(fixture);
    const std::size_t writes = fixture.backend->writes.size();

    const auto duplicate = fixture.node->HandleMqttCommand(MakeDestination("2", "PI-SORT-01", "MSG-DEST-02"));
    assert(duplicate.status == SortingCommandStatus::kDuplicate);
    assert(fixture.backend->writes.size() == writes);

    auto conflict = MakeDestination("3", "PI-SORT-01", "MSG-DEST-03");
    auto* payload = std::get_if<mqtt::DestinationSetPayload>(&conflict.data);
    assert(payload != nullptr);
    payload->work_id = "725a18df-8dbb-4a78-bb6c-f8f806b5ead8";
    assert(fixture.node->HandleMqttCommand(conflict).status == SortingCommandStatus::kActiveCycleConflict);
    assert(fixture.backend->writes.size() == writes);
}

void TestBusyPreservesCommandOrderAtNodeBoundary() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());

    const auto busy = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStop));

    assert(busy.status == SortingCommandStatus::kUartBusy);
    assert(fixture.backend->writes.size() == 1U);
}

void TestNackDoesNotActivateCycleAndReportsFailure() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());

    fixture.PushOperationResult(UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);

    assert(!fixture.node->HasActiveCycle());
    assert(fixture.reports.size() == 1U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.result == mqtt::CommandResult::kRejected);
    assert(response.error_code == "ERR-UART-PAYLOAD");
}

void TestControllerErrorsDistinguishRejectionFromFailure() {
    Fixture rejectedFixture;
    assert(rejectedFixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    rejectedFixture.PushOperationResult(UART_STATUS_ERROR, UART_ERROR_EMERGENCY_STOP);

    assert(!rejectedFixture.node->HasActiveCycle());
    assert(rejectedFixture.reports.size() == 1U);
    const auto& rejected = ReportPayload<mqtt::CommandResponsePayload>(rejectedFixture.reports.front());
    assert(rejected.result == mqtt::CommandResult::kRejected);
    assert(rejected.error_code == "ERR-EMERGENCY-STOP");

    Fixture failedFixture;
    assert(failedFixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    failedFixture.PushOperationResult(UART_STATUS_ERROR, UART_ERROR_MOTOR);

    assert(!failedFixture.node->HasActiveCycle());
    assert(failedFixture.reports.size() == 1U);
    const auto& failed = ReportPayload<mqtt::CommandResponsePayload>(failedFixture.reports.front());
    assert(failed.result == mqtt::CommandResult::kFailed);
    assert(failed.error_code == "ERR-MOTOR");
}

void TestSystemStartKeepsConveyorStopped() {
    Fixture fixture;

    const auto start = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart));
    const auto restart = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRestart));

    assert(start.status == SortingCommandStatus::kAcknowledged);
    assert(restart.status == SortingCommandStatus::kAcknowledged);
    assert(fixture.backend->writes.empty());
}

void TestStartConfiguresSpeedBeforeStartingConveyor() {
    Fixture fixture;
    const auto default_speed_result =
        fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor"));
    assert(default_speed_result.Succeeded());
    assert(fixture.LastCommand().command == UART_CMD_SORTING_CONVEYOR_SET_SPEED);
    assert(fixture.LastCommand().payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] == 50U);
    fixture.PushOperationResult();
    assert(fixture.LastCommand().command == UART_CMD_SORTING_CONVEYOR_START);
    fixture.PushOperationResult();
    fixture.reports.clear();

    mqtt::Json params = mqtt::Json::object();
    params["speed"] = 50;
    const auto result =
        fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor", params));
    assert(result.Succeeded());
    assert(fixture.LastCommand().command == UART_CMD_SORTING_CONVEYOR_SET_SPEED);
    assert(fixture.LastCommand().payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] == 50U);

    fixture.PushOperationResult();
    assert(fixture.LastCommand().command == UART_CMD_SORTING_CONVEYOR_START);
    assert(fixture.session->HasPendingCommand());
    assert(fixture.reports.empty());

    fixture.PushOperationResult();
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.reports.size() == 2U);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "RUNNING");
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.back());
    assert(response.result == mqtt::CommandResult::kSuccess);
}

void TestStartResendsCachedSpeedAfterControllerRestart() {
    Fixture fixture;
    mqtt::Json params = mqtt::Json::object();
    params["speed"] = 70;
    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor", params))
               .Succeeded());
    fixture.PushOperationResult();
    fixture.PushOperationResult();

    fixture.node->ResetControllerHeartbeatMonitor();
    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor")).Succeeded());

    const auto command = fixture.LastCommand();
    assert(command.command == UART_CMD_SORTING_CONVEYOR_SET_SPEED);
    assert(command.payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] == 70U);
}

void TestSpeedNotConfiguredErrorIsDistinctFromMalformedPayload() {
    Fixture speed_fixture;
    assert(speed_fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    speed_fixture.PushOperationResult(UART_STATUS_ERROR, UART_ERROR_SPEED_NOT_CONFIGURED);
    const auto& speed_response = ReportPayload<mqtt::CommandResponsePayload>(speed_fixture.reports.front());
    assert(speed_response.error_code == "ERR-SPEED-NOT-CONFIGURED");

    Fixture payload_fixture;
    assert(payload_fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    payload_fixture.PushOperationResult(UART_STATUS_NACK, UART_ERROR_INVALID_PAYLOAD);
    const auto& payload_response = ReportPayload<mqtt::CommandResponsePayload>(payload_fixture.reports.front());
    assert(payload_response.error_code == "ERR-UART-PAYLOAD");
}

void TestEmergencyStopPreemptsPendingCommandAndIsNotRetried() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    assert(fixture.session->HasPendingCommand());

    const auto result = fixture.node->HandleMqttCommand(MakeEmergencyStop());

    assert(result.status == SortingCommandStatus::kSentNoReply);
    assert(fixture.LastCommand().command == UART_CMD_EMERGENCY_STOP);
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.backend->writes.size() == 2U);
    assert(fixture.reports.size() == 1U);
    const auto& preempted = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(preempted.result == mqtt::CommandResult::kFailed);
    assert(preempted.error_code == "ERR-EMERGENCY-STOP");

    fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS * 4U });
    assert(fixture.backend->writes.size() == 2U);
}

void TestEmergencyStopReportsActiveCycleFailureBeforeRecoveryClearsIt() {
    Fixture fixture;
    ActivateCycle(fixture);

    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop()).status == SortingCommandStatus::kSentNoReply);
    fixture.PushControllerEvent(APP_EVENT_SAFETY, SAFETY_EVENT_ESTOP_LATCHED, 1U);

    const auto error_it = std::find_if(fixture.reports.begin(), fixture.reports.end(), [](const SortingReport& report) {
        return report.message_type == mqtt::MessageType::kErrorOccurred;
    });
    assert(error_it != fixture.reports.end());
    const auto response_it = std::find_if(
        fixture.reports.begin(), fixture.reports.end(),
        [](const SortingReport& report) { return report.message_type == mqtt::MessageType::kCommandResponse; });
    assert(response_it != fixture.reports.end());
    assert(error_it < response_it);
    const auto& error = ReportPayload<mqtt::ErrorOccurredPayload>(*error_it);
    assert(error.job_id == std::optional<std::string>(kWorkId));
    assert(error.error_code == "ERR-SORTING-CYCLE-ABORTED");
    assert(fixture.node->ActiveWorkId() == kWorkId);

    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery)).status ==
           SortingCommandStatus::kSentNoReply);
    fixture.PushControllerEvent(APP_EVENT_SAFETY, SAFETY_EVENT_RESET_COMPLETE, 1U);
    assert(!fixture.node->HasActiveCycle());
    assert(fixture.node->ActiveWorkId().empty());
}

void TestSafetyRecoveryUsesOneWayDeviceReset() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));

    assert(result.status == SortingCommandStatus::kSentNoReply);
    assert(fixture.LastCommand().command == UART_CMD_RESET_DEVICE);
    assert(!fixture.session->HasPendingCommand());
}

void TestRepeatedRecoveryClearsSortingStateAndCachedSpeed() {
    Fixture fixture;
    mqtt::Json speed = mqtt::Json::object();
    speed["speed"] = 70;
    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor", speed))
               .Succeeded());
    fixture.PushOperationResult();
    fixture.PushOperationResult();
    ActivateCycle(fixture);

    for (int attempt = 0; attempt < 2; ++attempt) {
        assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery)).Succeeded());
        fixture.PushControllerEvent(APP_EVENT_SAFETY, SAFETY_EVENT_RESET_COMPLETE, 0U);
        assert(!fixture.node->HasActiveCycle());
        assert(fixture.node->ActiveWorkId().empty());
        assert(fixture.node->ActiveDestination() == UART_SORTING_DESTINATION_NONE);
    }

    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, "sorting_conveyor")).Succeeded());
    const auto command = fixture.LastCommand();
    assert(command.command == UART_CMD_SORTING_CONVEYOR_SET_SPEED);
    assert(command.payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] == 50U);
}

void TestPendingSafetyCommandCannotBeOverwritten() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop()).Succeeded());

    const auto recovery = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));
    const auto duplicate_estop = fixture.node->HandleMqttCommand(MakeEmergencyStop());

    assert(recovery.status == SortingCommandStatus::kSafetyCommandPending);
    assert(duplicate_estop.status == SortingCommandStatus::kSafetyCommandPending);
    assert(fixture.backend->writes.size() == 1U);
    assert(fixture.node->HasPendingSafetyCommand());
}

void TestSystemTargetUsesSafetyRecovery() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(
        MakeControl(mqtt::ControlCommand::kRecovery, {}, mqtt::Json::object(), "SYSTEM"));

    assert(result.status == SortingCommandStatus::kSentNoReply);
    assert(fixture.LastCommand().command == UART_CMD_RESET_DEVICE);
}

void TestSafetyCommandsCompleteWithOriginalRequestId() {
    Fixture estopFixture;
    const auto estopResult = estopFixture.node->HandleMqttCommand(MakeEmergencyStop());
    assert(estopResult.status == SortingCommandStatus::kSentNoReply);
    estopFixture.PushControllerEvent(0x03U, 1U, 0U);
    const auto& estop = ReportPayload<mqtt::CommandResponsePayload>(estopFixture.reports.front());
    assert(estop.request_id == "REQ-ESTOP-01");
    assert(estop.result == mqtt::CommandResult::kSuccess);

    Fixture recoveryFixture;
    const auto recoveryResult = recoveryFixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));
    assert(recoveryResult.status == SortingCommandStatus::kSentNoReply);
    recoveryFixture.PushControllerEvent(0x03U, 2U, 0U);
    assert(recoveryFixture.reports.size() == 2U);
    const auto& recovery = ReportPayload<mqtt::CommandResponsePayload>(recoveryFixture.reports.front());
    assert(recovery.request_id == "REQ-CONTROL-01");
    assert(recovery.result == mqtt::CommandResult::kSuccess);
    const auto& stopped = ReportPayload<mqtt::DeviceStatusPayload>(recoveryFixture.reports.back());
    assert(stopped.current_state == "STOPPED");

    recoveryFixture.PushHeartbeat(UART_DEVICE_STOPPED, UART_ERROR_NONE);
    assert(recoveryFixture.reports.size() == 2U);
}

void TestRepeatedSafetyEventStillConfirmsNewPendingCommand() {
    Fixture fixture;
    ActivateCycle(fixture);
    fixture.PushControllerEvent(APP_EVENT_SAFETY, SAFETY_EVENT_ESTOP_LATCHED, 1U);

    const auto result = fixture.node->HandleMqttCommand(MakeEmergencyStop());
    assert(result.status == SortingCommandStatus::kSentNoReply);
    fixture.PushControllerEvent(APP_EVENT_SAFETY, SAFETY_EVENT_ESTOP_LATCHED, 1U);

    assert(!fixture.node->HasPendingSafetyCommand());
    const auto error_count = std::count_if(
        fixture.reports.begin(), fixture.reports.end(),
        [](const SortingReport& report) { return report.message_type == mqtt::MessageType::kErrorOccurred; });
    assert(error_count == 1U);
    assert(fixture.reports.size() == 3U);
    const auto response_it = std::find_if(
        fixture.reports.begin(), fixture.reports.end(),
        [](const SortingReport& report) { return report.message_type == mqtt::MessageType::kCommandResponse; });
    assert(response_it != fixture.reports.end());
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(*response_it);
    assert(response.request_id == "REQ-ESTOP-01");
    assert(response.result == mqtt::CommandResult::kSuccess);
}

void TestSafetyAndHeartbeatTimeoutsAreReported() {
    Fixture fixture;
    static_cast<void>(fixture.node->HandleMqttCommand(MakeEmergencyStop()));
    fixture.node->Tick(mqtt::kEmergencyStopConfirmationTimeout);
    const auto& timeout = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(timeout.result == mqtt::CommandResult::kTimeout);

    fixture.reports.clear();
    fixture.node->ResetControllerHeartbeatMonitor();
    fixture.node->Tick(std::chrono::seconds{ 3 });
    assert(fixture.reports.size() == 2U);
    fixture.PushHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE);
    const auto& recovered = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.back());
    assert(recovered.current_state == "RUNNING");
}

void TestEmergencyHeartbeatIsSafetyStateNotError() {
    Fixture fixture;

    fixture.PushHeartbeat(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_EMERGENCY_STOP);

    assert(fixture.reports.size() == 1U);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "EMERGENCY_STOP");
    assert(status.status == mqtt::ConnectionState::kOnline);
    assert(!status.error_code.has_value());
}

void TestReturnHomeAndCycleCompletePublishCompletion() {
    Fixture fixture;
    fixture.PushHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE);
    fixture.reports.clear();
    ActivateCycle(fixture);
    const std::uint16_t cycle_id = fixture.node->ActiveCycleId();

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery, "GATE"));
    const uart_frame_t frame = fixture.LastCommand();
    assert(result.Succeeded());
    assert(frame.command == UART_CMD_SORTING_RETURN_HOME);
    assert(uart_sorting_return_home_cycle_id(frame.payload) == cycle_id);
    fixture.PushOperationResult();
    fixture.reports.clear();

    fixture.PushCycleComplete(cycle_id, UART_SORTING_DESTINATION_2);

    assert(!fixture.node->HasActiveCycle());
    assert(fixture.reports.size() == 2U);
    assert(fixture.reports[0].channel == SortingReportChannel::kStatus);
    const auto& completed = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[0]);
    assert(completed.current_state == "CYCLE_COMPLETE");
    assert(completed.job_id == std::optional<std::string>(kWorkId));
    const auto& running = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[1]);
    assert(running.current_state == "RUNNING");
    assert(!running.job_id.has_value());
}

void TestReconnectStatusQueryPublishesControllerState() {
    Fixture fixture;

    const auto result = fixture.node->RequestControllerStatus();
    assert(result.Succeeded());
    assert(fixture.LastCommand().command == UART_CMD_SORTING_GET_STATUS);
    fixture.PushSortingStatus(UART_SORTING_GATE_HOME, UART_SORTING_CYCLE_ID_NONE, UART_SORTING_DESTINATION_NONE);

    assert(!fixture.session->HasPendingCommand());
    assert(fixture.reports.size() == 1U);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "STOPPED");
    assert(!status.job_id.has_value());
}

void TestGateHomeStatusDoesNotOverwriteRunningConveyor() {
    Fixture fixture;
    fixture.PushHeartbeat(UART_DEVICE_RUNNING, UART_ERROR_NONE);
    fixture.reports.clear();

    assert(fixture.node->RequestControllerStatus().Succeeded());
    fixture.PushSortingStatus(UART_SORTING_GATE_HOME, UART_SORTING_CYCLE_ID_NONE, UART_SORTING_DESTINATION_NONE);

    assert(fixture.reports.size() == 1U);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "RUNNING");
}

void TestReconnectStatusRejectsDestinationMappingMismatch() {
    Fixture fixture;
    ActivateCycle(fixture);
    const std::uint16_t cycle_id = fixture.node->ActiveCycleId();

    assert(fixture.node->RequestControllerStatus().Succeeded());
    fixture.PushSortingStatus(UART_SORTING_GATE_WAIT_ITEM, cycle_id, UART_SORTING_DESTINATION_3);

    assert(fixture.node->HasActiveCycle());
    assert(fixture.node->ActiveWorkId().empty());
    assert(fixture.node->ActiveDestination() == UART_SORTING_DESTINATION_3);
    assert(fixture.reports.size() == 2U);
    const auto& error = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.front());
    assert(error.error_code == "ERR-CYCLE-MAPPING-UNKNOWN");
}

void TestConveyorStatusUsesHeartbeatStateNames() {
    Fixture fixture;

    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStatusRequest, "CONVEYOR")).Succeeded());
    fixture.PushConveyorStatus(UART_SORTING_CONVEYOR_RUNNING, 50U);

    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "RUNNING");
}

void TestSensorStatusPublishesEveryDistanceMeasurement() {
    Fixture fixture;

    fixture.PushSensorStatus(UART_SORTING_SENSOR_ID_1, UART_SENSOR_OK, 42U);
    fixture.PushSensorStatus(UART_SORTING_SENSOR_ID_1, UART_SENSOR_OK, 37U);

    assert(fixture.reports.size() == 2U);
    assert(fixture.reports[0].channel == SortingReportChannel::kEvent);
    const auto& first = ReportPayload<mqtt::SensorStatusPayload>(fixture.reports[0]);
    assert(first.sensor_id == UART_SORTING_SENSOR_ID_1);
    assert(first.measurement_status == "OK");
    assert(first.distance_cm == 42);
    // Box arrival is the central server's call, so the node leaves it unset.
    assert(!first.detection_status.has_value());
    const auto& second = ReportPayload<mqtt::SensorStatusPayload>(fixture.reports[1]);
    assert(second.distance_cm == 37);
}

void TestSafetyAndHealthEventsAreDecodedAndDeduplicated() {
    Fixture fixture;

    fixture.PushControllerEvent(0x03U, 1U, 2U);
    fixture.PushControllerEvent(0x03U, 1U, 2U);
    assert(fixture.reports.size() == 1U);
    const auto& safety = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.back());
    assert(safety.current_state == "EMERGENCY_STOP");
    assert(safety.status == mqtt::ConnectionState::kOnline);
    assert(!safety.error_code.has_value());
    fixture.PushHeartbeat(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_NONE);
    assert(fixture.reports.size() == 1U);

    fixture.reports.clear();
    fixture.PushControllerEvent(APP_EVENT_HEALTH, HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT,
                                HEALTH_ISSUE_CAUSE_DEVICE_WIDE);
    fixture.PushControllerEvent(APP_EVENT_HEALTH, HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT,
                                HEALTH_ISSUE_CAUSE_DEVICE_WIDE);
    assert(fixture.reports.size() == 1U);
    const auto& health = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.front());
    assert(health.error_code == "ERR-HEALTH-QUEUE-OVERFLOW");

    fixture.PushHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE);
    fixture.PushControllerEvent(APP_EVENT_HEALTH, HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT,
                                HEALTH_ISSUE_CAUSE_DEVICE_WIDE);
    assert(fixture.reports.size() == 3U);
}

void TestOppositeUartChannelTimeoutIsIgnored() {
    Fixture fixture;

    fixture.PushControllerEvent(0x04U, 1U, 0U);

    assert(fixture.reports.empty());
}

void TestHealthSensorStaleIncludesSensorId() {
    Fixture fixture;

    // kind=3 (SENSOR_STALE), cause=1 (sorting channel), sensorId=UART_SORTING_SENSOR_ID_1.
    fixture.PushControllerEvent(0x04U, 3U, 1U, 0U, UART_SORTING_SENSOR_ID_1);
    assert(fixture.reports.size() == 1U);
    const auto& first = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.front());
    assert(first.error_code == "ERR-HEALTH-SENSOR-STALE");
    assert(first.message.find("sensorId=" + std::to_string(UART_SORTING_SENSOR_ID_1)) != std::string::npos);

    // A different sensor on the same channel/kind must not be swallowed as a duplicate -
    // US2/US3/US4 all share cause=SORTING, so sensorId is what actually tells them apart.
    fixture.PushControllerEvent(0x04U, 3U, 1U, 0U, UART_SORTING_SENSOR_ID_2);
    assert(fixture.reports.size() == 2U);
    const auto& second = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.back());
    assert(second.message.find("sensorId=" + std::to_string(UART_SORTING_SENSOR_ID_2)) != std::string::npos);

    // The same sensorId repeated is still deduplicated.
    fixture.PushControllerEvent(0x04U, 3U, 1U, 0U, UART_SORTING_SENSOR_ID_2);
    assert(fixture.reports.size() == 2U);
}

void TestCommandTimeoutReportsTimeout() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());

    for (std::uint8_t retry = 0U; retry < UART_MAX_RETRY_COUNT; ++retry) {
        fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS });
        fixture.session->Tick(std::chrono::milliseconds{ UART_RETRY_INTERVAL_MS });
    }
    fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS });

    assert(!fixture.session->HasPendingCommand());
    assert(fixture.node->HasActiveCycle());
    assert(fixture.node->ActiveWorkId() == kWorkId);
    assert(fixture.reports.size() == 2U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.result == mqtt::CommandResult::kTimeout);
    assert(response.error_code == "ERR-UART-RESPONSE-TIMEOUT");
    const auto& error = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.back());
    assert(error.error_code == "ERR-UART-RESPONSE-TIMEOUT");

    fixture.reports.clear();
    assert(fixture.node->RequestControllerStatus().Succeeded());
    fixture.PushSortingStatus(UART_SORTING_GATE_HOME, UART_SORTING_CYCLE_ID_NONE, UART_SORTING_DESTINATION_NONE);
    assert(!fixture.node->HasActiveCycle());
}

}  // namespace

int main() {
    TestDestinationMapsToRouteItemAndActivatesAfterSuccess();
    TestInvalidDestinationAndWrongTargetDoNotWrite();
    TestDuplicateAndConflictingWorkAreRejectedWithoutSecondMotion();
    TestBusyPreservesCommandOrderAtNodeBoundary();
    TestNackDoesNotActivateCycleAndReportsFailure();
    TestControllerErrorsDistinguishRejectionFromFailure();
    TestSystemStartKeepsConveyorStopped();
    TestStartConfiguresSpeedBeforeStartingConveyor();
    TestStartResendsCachedSpeedAfterControllerRestart();
    TestSpeedNotConfiguredErrorIsDistinctFromMalformedPayload();
    TestEmergencyStopPreemptsPendingCommandAndIsNotRetried();
    TestEmergencyStopReportsActiveCycleFailureBeforeRecoveryClearsIt();
    TestSafetyRecoveryUsesOneWayDeviceReset();
    TestRepeatedRecoveryClearsSortingStateAndCachedSpeed();
    TestPendingSafetyCommandCannotBeOverwritten();
    TestSystemTargetUsesSafetyRecovery();
    TestSafetyCommandsCompleteWithOriginalRequestId();
    TestRepeatedSafetyEventStillConfirmsNewPendingCommand();
    TestSafetyAndHeartbeatTimeoutsAreReported();
    TestEmergencyHeartbeatIsSafetyStateNotError();
    TestReturnHomeAndCycleCompletePublishCompletion();
    TestReconnectStatusQueryPublishesControllerState();
    TestGateHomeStatusDoesNotOverwriteRunningConveyor();
    TestReconnectStatusRejectsDestinationMappingMismatch();
    TestConveyorStatusUsesHeartbeatStateNames();
    TestSensorStatusPublishesEveryDistanceMeasurement();
    TestSafetyAndHealthEventsAreDecodedAndDeduplicated();
    TestOppositeUartChannelTimeoutIsIgnored();
    TestHealthSensorStaleIncludesSensorId();
    TestCommandTimeoutReportsTimeout();
    return 0;
}
