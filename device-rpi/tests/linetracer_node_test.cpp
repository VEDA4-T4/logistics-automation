#include "logistics/device/linetracer_node.hpp"

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
#include "logistics/contracts/uart_crc16.h"

namespace {

namespace mqtt = logistics::contracts::mqtt;
using logistics::device::LineTracerCommandStatus;
using logistics::device::LineTracerNode;
using logistics::device::LineTracerReport;
using logistics::device::LineTracerReportChannel;
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

struct Fixture {
    Fixture() {
        auto owned_backend = std::make_unique<FakeUartIoBackend>();
        backend = owned_backend.get();
        session = std::make_unique<UartSession>(std::move(owned_backend));
        assert(session->Open());
        node = std::make_unique<LineTracerNode>("PI-LT-01", *session);
        node->SetReportHandler([this](const LineTracerReport& report) { reports.push_back(report); });
        session->SetEventHandler(
            [this](const logistics::device::UartSessionEvent& event) { node->HandleUartEvent(event); });
    }

    [[nodiscard]] uart_frame_t LastFrame() const {
        assert(!backend->writes.empty());
        uart_frame_t frame{};
        const auto& bytes = backend->writes.back();
        assert(uart_decode_frame(bytes.data(), bytes.size(), &frame) == UART_CODEC_OK);
        return frame;
    }

    void AcknowledgeLastFrame() {
        const uart_frame_t command = LastFrame();
        uart_frame_t ack{};
        ack.version = UART_PROTOCOL_VERSION;
        ack.sequence = command.sequence;
        ack.command = UART_CMD_ACK;
        ack.length = UART_ACK_PAYLOAD_SIZE;
        ack.payload[UART_ACK_STATUS_INDEX] = UART_STATUS_ACK;
        ack.payload[UART_ACK_COMMAND_INDEX] = command.command;
        ack.payload[UART_ACK_LENGTH_INDEX] = command.length;
        const std::uint16_t payload_crc = uart_crc16_ccitt(command.payload, command.length);
        ack.payload[UART_ACK_CRC_LOW_INDEX] = UART_CRC_LOW_BYTE(payload_crc);
        ack.payload[UART_ACK_CRC_HIGH_INDEX] = UART_CRC_HIGH_BYTE(payload_crc);

        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length = 0;
        assert(uart_encode_frame(&ack, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);
        backend->PushRead({ encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_length) });
        assert(session->PollOnce().Succeeded());
        assert(!session->HasPendingCommand());
    }

    void PushEvent(std::uint8_t event_id, std::uint8_t detail = 0U, std::uint16_t job_id = 0U,
                   std::uint8_t route_id = UART_LINETRACER_ROUTE_NONE) {
        if (job_id == UART_LINETRACER_JOB_ID_NONE) {
            job_id = node->ActiveUartJobId();
        }
        if (route_id == UART_LINETRACER_ROUTE_NONE) {
            route_id = node->ActiveRouteId();
        }

        uart_frame_t frame{};
        frame.version = UART_PROTOCOL_VERSION;
        frame.sequence = next_event_sequence++;
        frame.command = UART_CMD_EVENT;
        frame.length = event_id == UART_LINETRACER_EVENT_STATE_CHANGED
                           ? UART_LINETRACER_STATE_EVENT_PAYLOAD_SIZE
                           : (event_id == UART_LINETRACER_EVENT_FAULT ? UART_LINETRACER_FAULT_EVENT_PAYLOAD_SIZE
                                                                      : UART_LINETRACER_JOB_EVENT_PAYLOAD_SIZE);
        frame.payload[UART_EVENT_ID_INDEX] = event_id;
        frame.payload[UART_LINETRACER_EVENT_JOB_ID_LOW_INDEX] = static_cast<std::uint8_t>(job_id & 0xffU);
        frame.payload[UART_LINETRACER_EVENT_JOB_ID_HIGH_INDEX] = static_cast<std::uint8_t>((job_id >> 8U) & 0xffU);
        frame.payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX] = route_id;
        if (event_id == UART_LINETRACER_EVENT_STATE_CHANGED) {
            frame.payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX] = detail;
        } else if (event_id == UART_LINETRACER_EVENT_FAULT) {
            frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX] = detail;
        }

        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length = 0;
        assert(uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);
        backend->PushRead({ encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_length) });
        assert(session->PollOnce().Succeeded());
    }

    FakeUartIoBackend* backend{};
    std::unique_ptr<UartSession> session;
    std::unique_ptr<LineTracerNode> node;
    std::vector<LineTracerReport> reports;
    std::uint8_t next_event_sequence{ 100U };
};

template <typename Payload>
const Payload& ReportPayload(const LineTracerReport& report) {
    const auto* payload = std::get_if<Payload>(&report.data);
    assert(payload != nullptr);
    return *payload;
}

mqtt::MqttMessage MakeDestination(std::string destination = "DEST-02", std::string target = "PI-LT-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-DEST-01",
        .message_type = mqtt::MessageType::kDestinationSet,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-22T10:00:00+09:00",
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

mqtt::MqttMessage MakeControl(mqtt::ControlCommand command, std::string target = "PI-LT-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-CONTROL-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-22T10:00:01+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "REQ-CONTROL-01",
                .command = command,
                .target_device_id = std::move(target),
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };
}

mqtt::MqttMessage MakeEmergencyStop(std::string target = "PI-LT-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ESTOP-01",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-22T10:00:02+09:00",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "REQ-ESTOP-01",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = std::move(target),
            },
    };
}

void AssignAndAcknowledge(Fixture& fixture) {
    const auto result = fixture.node->HandleMqttCommand(MakeDestination());
    assert(result.Succeeded());
    fixture.AcknowledgeLastFrame();
    fixture.reports.clear();
}

void TestDestinationMapsToAssignRoute() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeDestination());
    const uart_frame_t frame = fixture.LastFrame();

    assert(result.Succeeded());
    assert(result.uart_job_id == 1U);
    assert(result.uart_route_id == UART_LINETRACER_ROUTE_B);
    assert(frame.command == UART_CMD_LINETRACER_ASSIGN_ROUTE);
    assert(frame.length == UART_LINETRACER_START_PAYLOAD_SIZE);
    assert(uart_linetracer_start_job_id(frame.payload) == 1U);
    assert(frame.payload[UART_LINETRACER_START_ROUTE_ID_INDEX] == UART_LINETRACER_ROUTE_B);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(frame.command, frame.payload, frame.length) != 0U);
    assert(!fixture.node->HasActiveJob());
    fixture.AcknowledgeLastFrame();
    assert(fixture.node->ActiveWorkId() == kWorkId);
}

void TestStopUsesActiveJobId() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStop));
    const uart_frame_t frame = fixture.LastFrame();

    assert(result.Succeeded());
    assert(frame.command == UART_CMD_LINETRACER_STOP_DRIVE);
    assert(frame.length == UART_LINETRACER_STOP_PAYLOAD_SIZE);
    assert(uart_linetracer_stop_job_id(frame.payload) == fixture.node->ActiveUartJobId());
}

void TestRestartMapsToResume() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRestart));
    const uart_frame_t frame = fixture.LastFrame();

    assert(result.Succeeded());
    assert(frame.command == UART_CMD_LINETRACER_RESUME_DRIVE);
    assert(frame.length == UART_LINETRACER_RESUME_DRIVE_PAYLOAD_SIZE);
}

void TestInitializeMapsToResetAndClearsActiveJob() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kInitialize));
    const uart_frame_t frame = fixture.LastFrame();

    assert(result.Succeeded());
    assert(frame.command == UART_CMD_LINETRACER_RESET_SYSTEM);
    assert(frame.length == UART_LINETRACER_RESET_PAYLOAD_SIZE);
    assert(fixture.node->HasActiveJob());
    fixture.AcknowledgeLastFrame();
    assert(!fixture.node->HasActiveJob());
    assert(fixture.node->ActiveWorkId().empty());
}

void TestRecoveryUsesCommonDeviceResetAndClearsActiveJob() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));
    const uart_frame_t frame = fixture.LastFrame();

    assert(result.Succeeded());
    assert(frame.command == UART_CMD_RESET_DEVICE);
    assert(frame.length == 0U);
    assert(fixture.node->HasActiveJob());
    fixture.AcknowledgeLastFrame();
    assert(!fixture.node->HasActiveJob());
}

void TestEmergencyStopPreemptsPendingAndCompletesFromSafetyFault() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    assert(fixture.session->HasPendingCommand());

    const auto result = fixture.node->HandleMqttCommand(MakeEmergencyStop());

    assert(result.status == LineTracerCommandStatus::kSentNoReply);
    assert(fixture.LastFrame().command == UART_CMD_EMERGENCY_STOP);
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.reports.size() == 1U);
    const auto& preempted = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(preempted.result == mqtt::CommandResult::kFailed);
    assert(preempted.error_code == "ERR-EMERGENCY-STOP");

    fixture.PushEvent(UART_LINETRACER_EVENT_FAULT, UART_ERROR_EMERGENCY_STOP);

    assert(fixture.reports.size() == 3U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports[1]);
    assert(response.request_id == "REQ-ESTOP-01");
    assert(response.result == mqtt::CommandResult::kSuccess);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[2]);
    assert(status.current_state == "EMERGENCY_STOP");
    assert(status.status == mqtt::ConnectionState::kOnline);
    assert(!status.error_code.has_value());
}

void TestPendingSafetyCommandCannotBeOverwritten() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop()).Succeeded());

    const auto recovery = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kRecovery));
    const auto duplicate_estop = fixture.node->HandleMqttCommand(MakeEmergencyStop());

    assert(recovery.status == LineTracerCommandStatus::kSafetyCommandPending);
    assert(duplicate_estop.status == LineTracerCommandStatus::kSafetyCommandPending);
    assert(fixture.backend->writes.size() == 1U);
    assert(fixture.node->HasPendingSafetyCommand());
}

void TestEmergencyStopConfirmationTimesOut() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop()).Succeeded());

    fixture.node->Tick(mqtt::kEmergencyStopConfirmationTimeout);

    assert(fixture.reports.size() == 1U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.result == mqtt::CommandResult::kTimeout);
    assert(response.error_code == "ERR-SAFETY-CONFIRMATION-TIMEOUT");
}

void TestEmergencyStopFailsImmediatelyWhenUartDisconnects() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeEmergencyStop()).Succeeded());

    fixture.node->HandleUartEvent({
        .type = logistics::device::UartSessionEventType::kTransportDisconnected,
    });

    assert(fixture.reports.size() == 1U);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.result == mqtt::CommandResult::kFailed);
    assert(response.error_code == "ERR-UART-DISCONNECTED");
}

void TestInvalidDestinationIsRejectedWithoutUartWrite() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeDestination("WAREHOUSE-99"));

    assert(result.status == LineTracerCommandStatus::kInvalidDestination);
    assert(fixture.backend->writes.empty());
    assert(!fixture.node->HasActiveJob());
}

void TestStopWithoutActiveJobIsRejected() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kStop));

    assert(result.status == LineTracerCommandStatus::kNoActiveJob);
    assert(fixture.backend->writes.empty());
}

void TestWrongTargetIsRejected() {
    Fixture fixture;

    const auto result = fixture.node->HandleMqttCommand(MakeDestination("DEST-01", "PI-LT-02"));

    assert(result.status == LineTracerCommandStatus::kInvalidTarget);
    assert(fixture.backend->writes.empty());
}

void TestPendingUartCommandReportsBusy() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());

    const auto result = fixture.node->HandleMqttCommand(MakeControl(mqtt::ControlCommand::kInitialize));

    assert(result.status == LineTracerCommandStatus::kUartBusy);
    assert(fixture.backend->writes.size() == 1U);
    assert(!fixture.node->HasActiveJob());
}

void TestRejectedAssignDoesNotActivateJob() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    const uart_frame_t command = fixture.LastFrame();

    uart_frame_t nack{};
    nack.version = UART_PROTOCOL_VERSION;
    nack.sequence = command.sequence;
    nack.command = UART_CMD_ACK;
    nack.length = UART_ACK_PAYLOAD_SIZE;
    nack.payload[UART_ACK_STATUS_INDEX] = UART_STATUS_NACK;
    nack.payload[UART_ACK_COMMAND_INDEX] = command.command;
    nack.payload[UART_ACK_LENGTH_INDEX] = command.length;
    const std::uint16_t payload_crc = uart_crc16_ccitt(command.payload, command.length);
    nack.payload[UART_ACK_CRC_LOW_INDEX] = UART_CRC_LOW_BYTE(payload_crc);
    nack.payload[UART_ACK_CRC_HIGH_INDEX] = UART_CRC_HIGH_BYTE(payload_crc);

    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length = 0;
    assert(uart_encode_frame(&nack, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);
    fixture.backend->PushRead({ encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_length) });
    assert(fixture.session->PollOnce().Succeeded());

    assert(!fixture.node->HasActiveJob());
    assert(fixture.reports.size() == 1U);
    assert(fixture.reports.front().channel == LineTracerReportChannel::kResponse);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.result == mqtt::CommandResult::kRejected);
    assert(response.error_code == "ERR-UART-NACK");
}

void TestAcceptedAssignReportsSuccessAfterAck() {
    Fixture fixture;
    assert(fixture.node->HandleMqttCommand(MakeDestination()).Succeeded());
    assert(fixture.reports.empty());

    fixture.AcknowledgeLastFrame();

    assert(fixture.reports.size() == 1U);
    assert(fixture.reports.front().channel == LineTracerReportChannel::kResponse);
    const auto& response = ReportPayload<mqtt::CommandResponsePayload>(fixture.reports.front());
    assert(response.request_id == "REQ-DEST-01");
    assert(response.command == mqtt::ControlCommand::kDestinationSet);
    assert(response.result == mqtt::CommandResult::kSuccess);
    assert(!response.error_code.has_value());
}

void TestStateAndArrivalEventsReportPickupReady() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    fixture.PushEvent(UART_LINETRACER_EVENT_STATE_CHANGED, UART_LINETRACER_STATE_LOAD_WAIT);
    fixture.PushEvent(UART_LINETRACER_EVENT_ARRIVED);

    assert(fixture.reports.size() == 2U);
    for (const auto& report : fixture.reports) {
        assert(report.channel == LineTracerReportChannel::kStatus);
        assert(report.message_type == mqtt::MessageType::kDeviceStatus);
        const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(report);
        assert(status.current_state == "PICKUP_READY_B");
        assert(status.job_id == kWorkId);
    }
}

void TestLoadDetectedReportsLoadOn() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    fixture.PushEvent(UART_LINETRACER_EVENT_LOAD_DETECTED);

    assert(fixture.reports.size() == 1U);
    const auto& status = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports.front());
    assert(status.current_state == "LOAD_ON_B");
    assert(status.job_id == kWorkId);
}

void TestUnloadCompleteReportsCompletionAndClearsMapping() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    fixture.PushEvent(UART_LINETRACER_EVENT_UNLOAD_COMPLETE);

    assert(fixture.reports.size() == 3U);
    const auto& load_off = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[0]);
    assert(load_off.current_state == "LOAD_OFF_B");
    assert(load_off.job_id == kWorkId);

    assert(fixture.reports[1].channel == LineTracerReportChannel::kEvent);
    assert(fixture.reports[1].message_type == mqtt::MessageType::kWorkCompleted);
    const auto& completed = ReportPayload<mqtt::WorkCompletedPayload>(fixture.reports[1]);
    assert(completed.work_id == kWorkId);
    assert(completed.result == "SUCCESS");

    const auto& parked = ReportPayload<mqtt::DeviceStatusPayload>(fixture.reports[2]);
    assert(parked.current_state == "PARKED_B");
    assert(!parked.job_id.has_value());
    assert(!fixture.node->HasActiveJob());
    assert(fixture.node->ActiveWorkId().empty());
}

void TestFaultReportsMappedError() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    fixture.PushEvent(UART_LINETRACER_EVENT_FAULT, UART_ERROR_SENSOR);

    assert(fixture.reports.size() == 1U);
    assert(fixture.reports.front().channel == LineTracerReportChannel::kError);
    assert(fixture.reports.front().message_type == mqtt::MessageType::kErrorOccurred);
    const auto& error = ReportPayload<mqtt::ErrorOccurredPayload>(fixture.reports.front());
    assert(error.job_id == kWorkId);
    assert(error.error_code == "ERR-SENSOR");
    assert(error.error_level == "ERROR");
    assert(error.current_state == "FAULT");
}

void TestStaleJobEventIsIgnored() {
    Fixture fixture;
    AssignAndAcknowledge(fixture);

    fixture.PushEvent(UART_LINETRACER_EVENT_LOAD_DETECTED, 0U,
                      static_cast<std::uint16_t>(fixture.node->ActiveUartJobId() + 1U));

    assert(fixture.reports.empty());
    assert(fixture.node->HasActiveJob());
}

}  // namespace

int main() {
    TestDestinationMapsToAssignRoute();
    TestStopUsesActiveJobId();
    TestRestartMapsToResume();
    TestInitializeMapsToResetAndClearsActiveJob();
    TestRecoveryUsesCommonDeviceResetAndClearsActiveJob();
    TestEmergencyStopPreemptsPendingAndCompletesFromSafetyFault();
    TestPendingSafetyCommandCannotBeOverwritten();
    TestEmergencyStopConfirmationTimesOut();
    TestEmergencyStopFailsImmediatelyWhenUartDisconnects();
    TestInvalidDestinationIsRejectedWithoutUartWrite();
    TestStopWithoutActiveJobIsRejected();
    TestWrongTargetIsRejected();
    TestPendingUartCommandReportsBusy();
    TestRejectedAssignDoesNotActivateJob();
    TestAcceptedAssignReportsSuccessAfterAck();
    TestStateAndArrivalEventsReportPickupReady();
    TestLoadDetectedReportsLoadOn();
    TestUnloadCompleteReportsCompletionAndClearsMapping();
    TestFaultReportsMappedError();
    TestStaleJobEventIsIgnored();
    return 0;
}
