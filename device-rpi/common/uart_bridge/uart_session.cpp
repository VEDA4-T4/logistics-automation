#include "logistics/device/uart_session.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "logistics/contracts/uart_crc16.h"

namespace logistics::device {
namespace {

[[nodiscard]] std::uint32_t BoundedMilliseconds(std::chrono::milliseconds duration) noexcept {
    if (duration.count() <= 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(
        std::min<std::int64_t>(duration.count(), std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t SaturatingAdd(std::uint32_t left, std::uint32_t right) noexcept {
    if (right > std::numeric_limits<std::uint32_t>::max() - left) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return left + right;
}

[[nodiscard]] std::uint16_t AckPayloadCrc(const uart_frame_t& frame) noexcept {
    return static_cast<std::uint16_t>(frame.payload[UART_ACK_CRC_LOW_INDEX]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.payload[UART_ACK_CRC_HIGH_INDEX]) << 8U);
}

}  // namespace

bool UartSessionSendResult::Succeeded() const noexcept {
    return status == UartSessionSendStatus::kSent;
}

UartSession::UartSession() {
    uart_parser_init(&parser_);
}

UartSession::UartSession(std::unique_ptr<UartIoBackend> backend) : transport_(std::move(backend)) {
    uart_parser_init(&parser_);
}

UartSession::~UartSession() {
    Close();
}

bool UartSession::Open(std::string_view device_path) {
    ResetLinkState();
    return transport_.Open(device_path);
}

void UartSession::Close() noexcept {
    transport_.Close();
    ResetLinkState();
}

bool UartSession::IsOpen() const noexcept {
    return transport_.IsOpen();
}

void UartSession::SetEventHandler(UartSessionEventHandler handler) {
    event_handler_ = std::move(handler);
}

UartSessionSendResult UartSession::SendCommand(std::uint8_t command, std::span<const std::uint8_t> payload) {
    if (!IsOpen()) {
        return {
            UartSessionSendStatus::kNotOpen, 0, UART_CODEC_OK, { UartIoStatus::kNotOpen, 0, transport_.LastError() }
        };
    }
    if (pending_.active) {
        return { UartSessionSendStatus::kBusy, pending_.frame.sequence, UART_CODEC_OK, {} };
    }
    if (payload.size() > UART_MAX_PAYLOAD_SIZE || UART_IS_VALID_COMMAND(command) == 0U ||
        UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(command, payload.size()) == 0U) {
        return { UartSessionSendStatus::kInvalidArgument, 0, UART_CODEC_INVALID_ARGUMENT, {} };
    }

    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = AllocateSequence();
    frame.command = command;
    frame.length = static_cast<std::uint8_t>(payload.size());
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), frame.payload);
    }

    PendingCommand pending{};
    pending.frame = frame;
    pending.payload_crc = uart_crc16_ccitt(frame.payload, frame.length);
    const uart_codec_result_t codec_result =
        uart_encode_frame(&pending.frame, pending.encoded.data(), pending.encoded.size(), &pending.encoded_length);
    if (codec_result != UART_CODEC_OK) {
        return { UartSessionSendStatus::kEncodeError, frame.sequence, codec_result, {} };
    }

    const UartIoResult io_result =
        transport_.WriteAll(std::span<const std::uint8_t>(pending.encoded.data(), pending.encoded_length),
                            std::chrono::milliseconds{ UART_RETRY_INTERVAL_MS });
    if (!io_result.Succeeded()) {
        HandleTransportFailure(io_result);
        return { UartSessionSendStatus::kTransportError, frame.sequence, UART_CODEC_OK, io_result };
    }

    pending.active = true;
    pending_ = pending;
    return { UartSessionSendStatus::kSent, frame.sequence, UART_CODEC_OK, io_result };
}

UartIoResult UartSession::PollOnce(std::chrono::milliseconds read_timeout) {
    if (!IsOpen()) {
        return { UartIoStatus::kNotOpen, 0, transport_.LastError() };
    }

    std::array<std::uint8_t, kReceiveBufferSize> receive_buffer{};
    const UartIoResult result = transport_.Read(receive_buffer, read_timeout);
    if (result.Succeeded()) {
        ProcessBytes(std::span<const std::uint8_t>(receive_buffer.data(), result.bytes_transferred));
        return result;
    }
    if (result.status == UartIoStatus::kWouldBlock || result.status == UartIoStatus::kTimeout) {
        return result;
    }
    HandleTransportFailure(result);
    return result;
}

void UartSession::Tick(std::chrono::milliseconds elapsed) {
    const std::uint32_t elapsed_ms = BoundedMilliseconds(elapsed);
    if (elapsed_ms == 0U) {
        return;
    }

    HandleParserResult(uart_parser_tick(&parser_, elapsed_ms));
    if (!pending_.active) {
        return;
    }

    pending_.elapsed_ms = SaturatingAdd(pending_.elapsed_ms, elapsed_ms);
    if (pending_.waiting_to_retry) {
        if (pending_.elapsed_ms >= UART_RETRY_INTERVAL_MS) {
            RetryPendingCommand();
        }
        return;
    }

    if (pending_.elapsed_ms < UART_ACK_TIMEOUT_MS) {
        return;
    }
    if (pending_.retry_count >= UART_MAX_RETRY_COUNT) {
        ++diagnostics_.ack_timeouts;
        const PendingCommand expired = pending_;
        pending_ = {};
        Emit({ .type = UartSessionEventType::kAckTimeout,
               .pending_sequence = expired.frame.sequence,
               .pending_command = expired.frame.command,
               .retry_count = expired.retry_count });
        return;
    }

    pending_.waiting_to_retry = true;
    pending_.elapsed_ms = 0U;
}

bool UartSession::HasPendingCommand() const noexcept {
    return pending_.active;
}

std::uint8_t UartSession::PendingSequence() const noexcept {
    return pending_.active ? pending_.frame.sequence : 0U;
}

std::uint8_t UartSession::PendingCommandCode() const noexcept {
    return pending_.active ? pending_.frame.command : static_cast<std::uint8_t>(UART_CMD_NONE);
}

const UartSessionDiagnostics& UartSession::Diagnostics() const noexcept {
    return diagnostics_;
}

void UartSession::ResetLinkState() noexcept {
    uart_parser_reset(&parser_);
    pending_ = {};
    recent_frames_ = {};
    recent_frame_index_ = 0U;
}

void UartSession::ProcessBytes(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        uart_frame_t frame{};
        const uart_parser_result_t result = uart_parser_feed(&parser_, byte, &frame);
        if (result == UART_PARSER_FRAME_READY) {
            HandleReadyFrame(frame);
        } else {
            HandleParserResult(result);
        }
    }
}

void UartSession::HandleReadyFrame(const uart_frame_t& frame) {
    if (IsDuplicate(frame)) {
        ++diagnostics_.duplicate_frames;
        Emit({ .type = UartSessionEventType::kDuplicateFrame, .frame = frame });
        return;
    }

    ++diagnostics_.frames_received;
    if (frame.command == UART_CMD_ACK) {
        HandleAck(frame);
        return;
    }
    if (frame.command == UART_CMD_OPERATION_RESULT || frame.command == UART_CMD_RESPONSE) {
        HandleCommandResponse(frame);
        return;
    }
    Remember(frame);
    Emit({ .type = UartSessionEventType::kFrameReceived, .frame = frame });
}

void UartSession::HandleAck(const uart_frame_t& frame) {
    const bool matches_pending =
        pending_.active && frame.length == UART_ACK_PAYLOAD_SIZE &&
        frame.payload[UART_ACK_STATUS_INDEX] <= static_cast<std::uint8_t>(UART_STATUS_ERROR) &&
        frame.sequence == pending_.frame.sequence && frame.payload[UART_ACK_COMMAND_INDEX] == pending_.frame.command &&
        frame.payload[UART_ACK_LENGTH_INDEX] == pending_.frame.length && AckPayloadCrc(frame) == pending_.payload_crc;

    if (!matches_pending) {
        ++diagnostics_.unexpected_acks;
        Emit({ .type = UartSessionEventType::kUnexpectedAck,
               .frame = frame,
               .pending_sequence = PendingSequence(),
               .pending_command = PendingCommandCode(),
               .retry_count = pending_.retry_count });
        return;
    }

    Remember(frame);
    const PendingCommand acknowledged = pending_;
    pending_ = {};
    Emit({ .type = UartSessionEventType::kAckReceived,
           .frame = frame,
           .pending_sequence = acknowledged.frame.sequence,
           .pending_command = acknowledged.frame.command,
           .retry_count = acknowledged.retry_count });
}

void UartSession::HandleCommandResponse(const uart_frame_t& frame) {
    bool payload_valid = false;
    if (frame.command == UART_CMD_OPERATION_RESULT) {
        payload_valid =
            frame.length == UART_OPERATION_RESULT_PAYLOAD_SIZE &&
            frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] <= static_cast<std::uint8_t>(UART_STATUS_ERROR);
    } else {
        payload_valid = frame.length >= UART_RESPONSE_HEADER_SIZE &&
                        frame.payload[UART_RESPONSE_STATUS_INDEX] <= static_cast<std::uint8_t>(UART_STATUS_ERROR) &&
                        pending_.active && frame.payload[UART_RESPONSE_COMMAND_INDEX] == pending_.frame.command;
    }

    const bool matches_pending = payload_valid && pending_.active && frame.sequence == pending_.frame.sequence;
    if (!matches_pending) {
        ++diagnostics_.unexpected_command_responses;
        Emit({ .type = UartSessionEventType::kUnexpectedCommandResponse,
               .frame = frame,
               .pending_sequence = PendingSequence(),
               .pending_command = PendingCommandCode(),
               .retry_count = pending_.retry_count });
        return;
    }

    ++diagnostics_.command_responses;
    Remember(frame);
    const PendingCommand completed = pending_;
    pending_ = {};
    Emit({ .type = UartSessionEventType::kCommandResponseReceived,
           .frame = frame,
           .pending_sequence = completed.frame.sequence,
           .pending_command = completed.frame.command,
           .retry_count = completed.retry_count });
}

void UartSession::HandleParserResult(uart_parser_result_t result) {
    if (result == UART_PARSER_NO_FRAME || result == UART_PARSER_FRAME_READY) {
        return;
    }
    ++diagnostics_.parser_errors;
    if (result == UART_PARSER_CRC_ERROR) {
        ++diagnostics_.crc_errors;
    }
    Emit({ .type = UartSessionEventType::kParserError, .parser_result = result });
}

void UartSession::HandleTransportFailure(UartIoResult result) {
    const bool disconnected = result.status == UartIoStatus::kDisconnected || result.status == UartIoStatus::kNotOpen;
    if (disconnected) {
        ++diagnostics_.transport_disconnects;
    } else {
        ++diagnostics_.transport_errors;
    }

    const PendingCommand failed = pending_;
    transport_.Close();
    ResetLinkState();
    Emit({ .type = disconnected ? UartSessionEventType::kTransportDisconnected : UartSessionEventType::kTransportError,
           .io_result = result,
           .pending_sequence = failed.active ? failed.frame.sequence : static_cast<std::uint8_t>(0U),
           .pending_command = failed.active ? failed.frame.command : static_cast<std::uint8_t>(UART_CMD_NONE),
           .retry_count = failed.retry_count });
}

void UartSession::RetryPendingCommand() {
    const UartIoResult result =
        transport_.WriteAll(std::span<const std::uint8_t>(pending_.encoded.data(), pending_.encoded_length),
                            std::chrono::milliseconds{ UART_RETRY_INTERVAL_MS });
    if (!result.Succeeded()) {
        HandleTransportFailure(result);
        return;
    }

    pending_.waiting_to_retry = false;
    pending_.elapsed_ms = 0U;
    ++pending_.retry_count;
    ++diagnostics_.command_retries;
    Emit({ .type = UartSessionEventType::kCommandRetried,
           .io_result = result,
           .pending_sequence = pending_.frame.sequence,
           .pending_command = pending_.frame.command,
           .retry_count = pending_.retry_count });
}

void UartSession::Emit(UartSessionEvent event) const {
    if (event_handler_) {
        event_handler_(event);
    }
}

bool UartSession::IsDuplicate(const uart_frame_t& frame) const noexcept {
    return std::any_of(recent_frames_.begin(), recent_frames_.end(), [&frame](const FrameSignature& signature) {
        return signature.valid && signature.sequence == frame.sequence && signature.command == frame.command &&
               signature.crc == frame.crc;
    });
}

void UartSession::Remember(const uart_frame_t& frame) noexcept {
    recent_frames_[recent_frame_index_] = { true, frame.sequence, frame.command, frame.crc };
    recent_frame_index_ = (recent_frame_index_ + 1U) % recent_frames_.size();
}

std::uint8_t UartSession::AllocateSequence() noexcept {
    const std::uint8_t allocated = next_sequence_;
    ++next_sequence_;
    return allocated;
}

}  // namespace logistics::device
