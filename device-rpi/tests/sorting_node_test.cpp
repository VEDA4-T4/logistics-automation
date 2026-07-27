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

    void PushControllerEvent(std::uint8_t event_id, std::uint8_t kind, std::uint8_t cause, std::uint8_t result = 0U) {
        uart_frame_t event{};
        event.version = UART_PROTOCOL_VERSION;
        event.sequence = next_event_sequence++;
        event.command = UART_CMD_EVENT;
        event.length = event_id == 0x03U ? 8U : 7U;
        event.payload[UART_EVENT_ID_INDEX] = event_id;
        event.payload[1U] = kind;
        event.payload[2U] = cause;
        if (event.length == 8U) {
            event.payload[7U] = result;
        }
        backend->PushRead(Encode(event));
        assert(session->PollOnce().Succeeded());
    }

    void PushHeartbeat(std::uint8_t state, std::uint8_t error) {
        uart_frame_t event{};
        event.version = UART_PROTOCOL_VERSION;
        event.sequence = next_event_sequence++;
        event.command = UART_CMD_EVENT;
        event.length = 9U;
        event.payload[UART_EVENT_ID_INDEX] = 0x01U;
        event.payload[1U] = state;
        event.payload[2U] = error;
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
                              mqtt::Json params = mqtt::Json::object()) {
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
                .target_device_id = "PI-SORT-01",
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

void TestStartConfiguresSpeedBeforeStartingConveyor() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart)).status ==
           SortingCommandStatus::kInvalidSpeed);
    assert(fixture.backend->writes.empty());

    mqtt::Json params = mqtt::Json::object();
    params["speed"] = 50;
    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStart, {}, params));
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
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.back());
    assert(response.result == mqtt::CommandResult::kSuccess);
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

void TestSafetyRecoveryUsesOneWayDeviceReset() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery, "SAFETY"));

    assert(result.status == SortingCommandStatus::kSentNoReply);
    assert(fixture.LastCommand().command == UART_CMD_RESET_DEVICE);
    assert(!fixture.session->HasPendingCommand());
}

void TestReturnHomeAndCycleCompletePublishCompletion() {
    Fixture fixture;
    ActivateCycle(fixture);
    const std::uint16_t cycle_id = fixture.node->ActiveCycleId();

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));
    const uart_frame_t frame = fixture.LastCommand();
    assert(result.Succeeded());
    assert(frame.command == UART_CMD_SORTING_RETURN_HOME);
    assert(uart_sorting_return_home_cycle_id(frame.payload) == cycle_id);
    fixture.PushOperationResult();
    fixture.reports.clear();

    fixture.PushCycleComplete(cycle_id, UART_SORTING_DESTINATION_2);

    assert(!fixture.node->HasActiveCycle());
    assert(fixture.reports.size() == 2U);
    assert(fixture.reports[0].channel == SortingReportChannel::kEvent);
    const auto& completed = ReportPayload<mqtt::WorkCompletedPayload>(fixture.reports[0]);
    assert(completed.work_id == kWorkId);
    assert(completed.result == "SUCCESS");
    const auto& idle = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[1]);
    assert(idle.current_state == "IDLE");
    assert(!idle.job_id.has_value());
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
    assert(status.current_state == "IDLE");
    assert(!status.job_id.has_value());
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

void TestSensorStatusPublishesEveryDistanceMeasurement() {
    Fixture fixture;

    fixture.PushSensorStatus(UART_SORTING_SENSOR_ID_1, UART_SENSOR_CLEAR, 42U);
    fixture.PushSensorStatus(UART_SORTING_SENSOR_ID_1, UART_SENSOR_CLEAR, 37U);

    assert(fixture.reports.size() == 3U);
    assert(fixture.reports[0].channel == SortingReportChannel::kEvent);
    const auto& first = ReportPayload<mqtt::SensorStatusPayload>(fixture.reports[0]);
    assert(first.sensor_id == UART_SORTING_SENSOR_ID_1);
    assert(first.measurement_status == "CLEAR");
    assert(first.distance_cm == 42);
    const auto& second = ReportPayload<mqtt::SensorStatusPayload>(fixture.reports[2]);
    assert(second.distance_cm == 37);
}

void TestSafetyAndHealthEventsAreDecodedAndDeduplicated() {
    Fixture fixture;

    fixture.PushControllerEvent(0x03U, 1U, 2U);
    fixture.PushControllerEvent(0x03U, 1U, 2U);
    assert(fixture.reports.size() == 2U);
    const auto& safety = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.back());
    assert(safety.error_code == "ERR-EMERGENCY-STOP");

    fixture.reports.clear();
    fixture.PushControllerEvent(0x04U, 2U, 3U);
    fixture.PushControllerEvent(0x04U, 2U, 3U);
    assert(fixture.reports.size() == 1U);
    const auto& health = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.front());
    assert(health.error_code == "ERR-HEALTH-QUEUE-OVERFLOW");

    fixture.PushHeartbeat(UART_DEVICE_READY, UART_ERROR_NONE);
    fixture.PushControllerEvent(0x04U, 2U, 3U);
    assert(fixture.reports.size() == 3U);
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
    TestStartConfiguresSpeedBeforeStartingConveyor();
    TestEmergencyStopPreemptsPendingCommandAndIsNotRetried();
    TestSafetyRecoveryUsesOneWayDeviceReset();
    TestReturnHomeAndCycleCompletePublishCompletion();
    TestReconnectStatusQueryPublishesControllerState();
    TestReconnectStatusRejectsDestinationMappingMismatch();
    TestSensorStatusPublishesEveryDistanceMeasurement();
    TestSafetyAndHealthEventsAreDecodedAndDeduplicated();
    TestCommandTimeoutReportsTimeout();
    return 0;
}
