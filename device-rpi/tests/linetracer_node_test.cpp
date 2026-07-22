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

    FakeUartIoBackend* backend{};
    std::unique_ptr<UartSession> session;
    std::unique_ptr<LineTracerNode> node;
};

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

void AssignAndAcknowledge(Fixture& fixture) {
    const auto result = fixture.node->HandleMqttCommand(MakeDestination());
    assert(result.Succeeded());
    fixture.AcknowledgeLastFrame();
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
}

}  // namespace

int main() {
    TestDestinationMapsToAssignRoute();
    TestStopUsesActiveJobId();
    TestRestartMapsToResume();
    TestInitializeMapsToResetAndClearsActiveJob();
    TestInvalidDestinationIsRejectedWithoutUartWrite();
    TestStopWithoutActiveJobIsRejected();
    TestWrongTargetIsRejected();
    TestPendingUartCommandReportsBusy();
    TestRejectedAssignDoesNotActivateJob();
    return 0;
}
