#include "logistics/device/uart_session.hpp"

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
#include <utility>
#include <vector>

#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/contracts/uart_crc16.h"

namespace {

using logistics::device::UartIoBackend;
using logistics::device::UartIoResult;
using logistics::device::UartIoStatus;
using logistics::device::UartSession;
using logistics::device::UartSessionEvent;
using logistics::device::UartSessionEventType;

struct ReadAction {
    UartIoResult result;
    std::vector<std::uint8_t> data;
};

class FakeUartIoBackend final : public UartIoBackend {
public:
    UartIoResult Open(std::string_view device_path) override {
        opened_path = device_path;
        open = open_result.Succeeded();
        return open_result;
    }

    void Close() noexcept override {
        open = false;
    }

    bool IsOpen() const noexcept override {
        return open;
    }

    UartIoResult Read(std::span<std::uint8_t> buffer) override {
        assert(!read_actions.empty());
        ReadAction action = std::move(read_actions.front());
        read_actions.pop_front();
        if (action.result.Succeeded()) {
            assert(action.data.size() == action.result.bytes_transferred);
            assert(action.data.size() <= buffer.size());
            std::copy(action.data.begin(), action.data.end(), buffer.begin());
        }
        return action.result;
    }

    UartIoResult Write(std::span<const std::uint8_t> data) override {
        assert(!write_actions.empty());
        UartIoResult result = write_actions.front();
        write_actions.pop_front();
        if (result.Succeeded()) {
            assert(result.bytes_transferred <= data.size());
            writes.emplace_back(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(result.bytes_transferred));
        }
        return result;
    }

    UartIoResult WaitReadable(std::chrono::milliseconds) override {
        assert(!read_wait_actions.empty());
        const UartIoResult result = read_wait_actions.front();
        read_wait_actions.pop_front();
        return result;
    }

    UartIoResult WaitWritable(std::chrono::milliseconds) override {
        assert(!write_wait_actions.empty());
        const UartIoResult result = write_wait_actions.front();
        write_wait_actions.pop_front();
        return result;
    }

    void PushRead(std::span<const std::uint8_t> bytes) {
        read_actions.push_back(
            { { UartIoStatus::kSuccess, bytes.size(), 0 }, std::vector<std::uint8_t>(bytes.begin(), bytes.end()) });
    }

    void PushWrite(std::size_t length) {
        write_actions.push_back({ UartIoStatus::kSuccess, length, 0 });
    }

    UartIoResult open_result{ UartIoStatus::kSuccess, 0, 0 };
    bool open{};
    std::string opened_path;
    std::deque<ReadAction> read_actions;
    std::deque<UartIoResult> write_actions;
    std::deque<UartIoResult> read_wait_actions;
    std::deque<UartIoResult> write_wait_actions;
    std::vector<std::vector<std::uint8_t>> writes;
};

struct Fixture {
    Fixture() {
        auto owned_backend = std::make_unique<FakeUartIoBackend>();
        backend = owned_backend.get();
        session = std::make_unique<UartSession>(std::move(owned_backend));
        session->SetEventHandler([this](const UartSessionEvent& event) { events.push_back(event); });
        assert(session->Open());
    }

    FakeUartIoBackend* backend{};
    std::unique_ptr<UartSession> session;
    std::vector<UartSessionEvent> events;
};

std::vector<std::uint8_t> EncodeFrame(const uart_frame_t& frame) {
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length = 0;
    assert(uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);
    return { encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_length) };
}

uart_frame_t MakeEventFrame(std::uint8_t sequence, std::uint16_t cycle_id = 1U) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_EVENT;
    frame.length = UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE;
    frame.payload[UART_EVENT_ID_INDEX] = UART_SORTING_EVENT_CYCLE_COMPLETE;
    frame.payload[UART_SORTING_EVENT_CYCLE_ID_LOW_INDEX] = static_cast<std::uint8_t>(cycle_id & 0xffU);
    frame.payload[UART_SORTING_EVENT_CYCLE_ID_HIGH_INDEX] = static_cast<std::uint8_t>((cycle_id >> 8U) & 0xffU);
    frame.payload[UART_SORTING_EVENT_DESTINATION_INDEX] = UART_SORTING_DESTINATION_1;
    return frame;
}

uart_frame_t MakeAckFrame(std::uint8_t sequence, std::uint8_t original_command,
                          std::span<const std::uint8_t> original_payload) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_ACK;
    frame.length = UART_ACK_PAYLOAD_SIZE;
    frame.payload[UART_ACK_STATUS_INDEX] = UART_STATUS_ACK;
    frame.payload[UART_ACK_COMMAND_INDEX] = original_command;
    frame.payload[UART_ACK_LENGTH_INDEX] = static_cast<std::uint8_t>(original_payload.size());
    const std::uint16_t payload_crc = uart_crc16_ccitt(original_payload.data(), original_payload.size());
    frame.payload[UART_ACK_CRC_LOW_INDEX] = UART_CRC_LOW_BYTE(payload_crc);
    frame.payload[UART_ACK_CRC_HIGH_INDEX] = UART_CRC_HIGH_BYTE(payload_crc);
    return frame;
}

uart_frame_t MakeOperationResultFrame(std::uint8_t sequence, std::uint8_t status = UART_STATUS_SUCCESS,
                                      std::uint8_t error = UART_ERROR_NONE) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_OPERATION_RESULT;
    frame.length = UART_OPERATION_RESULT_PAYLOAD_SIZE;
    frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] = status;
    frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] = error;
    return frame;
}

std::uint8_t SendRouteCommand(Fixture& fixture) {
    constexpr std::array<std::uint8_t, UART_SORTING_ROUTE_PAYLOAD_SIZE> kPayload{ 0x34U, 0x12U,
                                                                                  UART_SORTING_DESTINATION_2 };
    fixture.backend->PushWrite(UART_FRAME_OVERHEAD_SIZE + kPayload.size());
    const auto result = fixture.session->SendCommand(UART_CMD_SORTING_ROUTE_ITEM, kPayload);
    assert(result.Succeeded());
    return result.sequence;
}

void TestPartialFrameIsDeliveredOnce() {
    Fixture fixture;
    const auto encoded = EncodeFrame(MakeEventFrame(10U));
    fixture.backend->PushRead(std::span<const std::uint8_t>(encoded.data(), 3U));
    fixture.backend->PushRead(std::span<const std::uint8_t>(encoded.data() + 3U, encoded.size() - 3U));

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.empty());
    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kFrameReceived);
    assert(fixture.events[0].frame.sequence == 10U);
}

void TestContinuousFramesAreBothDelivered() {
    Fixture fixture;
    const auto first = EncodeFrame(MakeEventFrame(11U, 1U));
    const auto second = EncodeFrame(MakeEventFrame(12U, 2U));
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());
    fixture.backend->PushRead(combined);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 2U);
    assert(fixture.events[0].frame.sequence == 11U);
    assert(fixture.events[1].frame.sequence == 12U);
}

void TestGarbageBeforeFrameIsIgnored() {
    Fixture fixture;
    const auto encoded = EncodeFrame(MakeEventFrame(13U));
    std::vector<std::uint8_t> input{ 0x00U, 0x55U, 0x7fU };
    input.insert(input.end(), encoded.begin(), encoded.end());
    fixture.backend->PushRead(input);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kFrameReceived);
}

void TestCrcErrorIsRejected() {
    Fixture fixture;
    auto encoded = EncodeFrame(MakeEventFrame(14U));
    encoded[UART_FRAME_HEADER_SIZE] ^= 0x01U;
    fixture.backend->PushRead(encoded);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kParserError);
    assert(fixture.events[0].parser_result == UART_PARSER_CRC_ERROR);
    assert(fixture.session->Diagnostics().crc_errors == 1U);
}

void TestPartialFrameTimeoutThenRecovery() {
    Fixture fixture;
    const auto encoded = EncodeFrame(MakeEventFrame(15U));
    fixture.backend->PushRead(std::span<const std::uint8_t>(encoded.data(), 3U));
    assert(fixture.session->PollOnce().Succeeded());

    fixture.session->Tick(std::chrono::milliseconds{ UART_PARSER_TIMEOUT_MS });
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kParserError);
    assert(fixture.events[0].parser_result == UART_PARSER_TIMEOUT);

    fixture.backend->PushRead(encoded);
    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 2U);
    assert(fixture.events[1].type == UartSessionEventType::kFrameReceived);
}

void TestMatchingAckCompletesPendingCommand() {
    Fixture fixture;
    constexpr std::array<std::uint8_t, UART_SORTING_ROUTE_PAYLOAD_SIZE> kPayload{ 0x34U, 0x12U,
                                                                                  UART_SORTING_DESTINATION_2 };
    const std::uint8_t sequence = SendRouteCommand(fixture);
    const auto ack = EncodeFrame(MakeAckFrame(sequence, UART_CMD_SORTING_ROUTE_ITEM, kPayload));
    fixture.backend->PushRead(ack);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kAckReceived);
    assert(!fixture.session->HasPendingCommand());
}

void TestWrongSequenceAckDoesNotCompleteCommand() {
    Fixture fixture;
    constexpr std::array<std::uint8_t, UART_SORTING_ROUTE_PAYLOAD_SIZE> kPayload{ 0x34U, 0x12U,
                                                                                  UART_SORTING_DESTINATION_2 };
    const std::uint8_t sequence = SendRouteCommand(fixture);
    const auto ack =
        EncodeFrame(MakeAckFrame(static_cast<std::uint8_t>(sequence + 1U), UART_CMD_SORTING_ROUTE_ITEM, kPayload));
    fixture.backend->PushRead(ack);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kUnexpectedAck);
    assert(fixture.session->HasPendingCommand());
}

void TestWrongPayloadCrcAckDoesNotCompleteCommand() {
    Fixture fixture;
    constexpr std::array<std::uint8_t, UART_SORTING_ROUTE_PAYLOAD_SIZE> kPayload{ 0x34U, 0x12U,
                                                                                  UART_SORTING_DESTINATION_2 };
    const std::uint8_t sequence = SendRouteCommand(fixture);
    uart_frame_t ack = MakeAckFrame(sequence, UART_CMD_SORTING_ROUTE_ITEM, kPayload);
    ack.payload[UART_ACK_CRC_LOW_INDEX] ^= 0x01U;
    const auto encoded = EncodeFrame(ack);
    fixture.backend->PushRead(encoded);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kUnexpectedAck);
    assert(fixture.session->HasPendingCommand());
}

void TestOperationResultCompletesPendingCommand() {
    Fixture fixture;
    const std::uint8_t sequence = SendRouteCommand(fixture);
    fixture.backend->PushRead(EncodeFrame(MakeOperationResultFrame(sequence)));

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kCommandResponseReceived);
    assert(fixture.events[0].pending_sequence == sequence);
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.session->Diagnostics().command_responses == 1U);
}

void TestWrongSequenceOperationResultDoesNotCompleteCommand() {
    Fixture fixture;
    const std::uint8_t sequence = SendRouteCommand(fixture);
    fixture.backend->PushRead(EncodeFrame(MakeOperationResultFrame(static_cast<std::uint8_t>(sequence + 1U))));

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kUnexpectedCommandResponse);
    assert(fixture.session->HasPendingCommand());
    assert(fixture.session->Diagnostics().unexpected_command_responses == 1U);
}

void TestUnexpectedResponseDoesNotPoisonNextSequence() {
    Fixture fixture;
    const std::uint8_t first_sequence = SendRouteCommand(fixture);
    const std::uint8_t next_sequence = static_cast<std::uint8_t>(first_sequence + 1U);
    const auto early_next_response = EncodeFrame(MakeOperationResultFrame(next_sequence));
    fixture.backend->PushRead(early_next_response);
    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.session->HasPendingCommand());

    fixture.backend->PushRead(EncodeFrame(MakeOperationResultFrame(first_sequence)));
    assert(fixture.session->PollOnce().Succeeded());
    assert(!fixture.session->HasPendingCommand());

    assert(SendRouteCommand(fixture) == next_sequence);
    fixture.backend->PushRead(early_next_response);
    assert(fixture.session->PollOnce().Succeeded());
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.events.back().type == UartSessionEventType::kCommandResponseReceived);
}

void TestDuplicateFrameIsNotDeliveredTwice() {
    Fixture fixture;
    const auto encoded = EncodeFrame(MakeEventFrame(20U));
    fixture.backend->PushRead(encoded);
    fixture.backend->PushRead(encoded);

    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.session->PollOnce().Succeeded());
    assert(fixture.events.size() == 2U);
    assert(fixture.events[0].type == UartSessionEventType::kFrameReceived);
    assert(fixture.events[1].type == UartSessionEventType::kDuplicateFrame);
    assert(fixture.session->Diagnostics().duplicate_frames == 1U);
}

void TestAckTimeoutRetriesSameFrameThreeTimes() {
    Fixture fixture;
    static_cast<void>(SendRouteCommand(fixture));
    assert(fixture.backend->writes.size() == 1U);

    for (std::uint8_t retry = 1U; retry <= UART_MAX_RETRY_COUNT; ++retry) {
        fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS });
        fixture.backend->PushWrite(fixture.backend->writes.front().size());
        fixture.session->Tick(std::chrono::milliseconds{ UART_RETRY_INTERVAL_MS });
        assert(fixture.session->HasPendingCommand());
        assert(fixture.backend->writes.back() == fixture.backend->writes.front());
    }

    fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS });
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.session->Diagnostics().command_retries == UART_MAX_RETRY_COUNT);
    assert(fixture.session->Diagnostics().ack_timeouts == 1U);
    assert(fixture.backend->writes.size() == 1U + UART_MAX_RETRY_COUNT);
    assert(fixture.events.back().type == UartSessionEventType::kAckTimeout);
}

void TestOneWayCommandIsWrittenOnceWithoutPendingRetry() {
    Fixture fixture;
    fixture.backend->PushWrite(UART_FRAME_OVERHEAD_SIZE);

    const auto result = fixture.session->SendOneWayCommand(UART_CMD_EMERGENCY_STOP);

    assert(result.Succeeded());
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.backend->writes.size() == 1U);
    fixture.session->Tick(std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS * 4U });
    assert(fixture.backend->writes.size() == 1U);
    assert(fixture.session->Diagnostics().command_retries == 0U);
    assert(fixture.session->Diagnostics().ack_timeouts == 0U);
}

void TestPendingCommandCanBeCancelledBeforeOneWaySafetyCommand() {
    Fixture fixture;
    static_cast<void>(SendRouteCommand(fixture));
    assert(fixture.session->HasPendingCommand());

    assert(fixture.session->CancelPendingCommand());
    assert(!fixture.session->HasPendingCommand());
    assert(!fixture.session->CancelPendingCommand());

    fixture.backend->PushWrite(UART_FRAME_OVERHEAD_SIZE);
    const auto result = fixture.session->SendOneWayCommand(UART_CMD_RESET_DEVICE);
    assert(result.Succeeded());
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.backend->writes.size() == 2U);
}

void TestDisconnectFailsPendingCommand() {
    Fixture fixture;
    const std::uint8_t sequence = SendRouteCommand(fixture);
    fixture.backend->read_actions.push_back({ { UartIoStatus::kDisconnected, 0, 19 }, {} });

    const UartIoResult result = fixture.session->PollOnce();

    assert(result.status == UartIoStatus::kDisconnected);
    assert(!fixture.session->IsOpen());
    assert(!fixture.session->HasPendingCommand());
    assert(fixture.events.size() == 1U);
    assert(fixture.events[0].type == UartSessionEventType::kTransportDisconnected);
    assert(fixture.events[0].pending_sequence == sequence);
}

}  // namespace

int main() {
    TestPartialFrameIsDeliveredOnce();
    TestContinuousFramesAreBothDelivered();
    TestGarbageBeforeFrameIsIgnored();
    TestCrcErrorIsRejected();
    TestPartialFrameTimeoutThenRecovery();
    TestMatchingAckCompletesPendingCommand();
    TestWrongSequenceAckDoesNotCompleteCommand();
    TestWrongPayloadCrcAckDoesNotCompleteCommand();
    TestOperationResultCompletesPendingCommand();
    TestWrongSequenceOperationResultDoesNotCompleteCommand();
    TestUnexpectedResponseDoesNotPoisonNextSequence();
    TestDuplicateFrameIsNotDeliveredTwice();
    TestAckTimeoutRetriesSameFrameThreeTimes();
    TestOneWayCommandIsWrittenOnceWithoutPendingRetry();
    TestPendingCommandCanBeCancelledBeforeOneWaySafetyCommand();
    TestDisconnectFailsPendingCommand();
    return 0;
}
