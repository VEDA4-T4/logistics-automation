#include "logistics/device/input_uart_session.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "logistics/contracts/uart_codec.h"

namespace logistics::device {
namespace {

constexpr auto kAckTimeout = std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS };
constexpr auto kWriteTimeout = std::chrono::milliseconds{ UART_ACK_TIMEOUT_MS };

}  // namespace

InputUartSession::InputUartSession() {
    uart_parser_init(&parser_);
}

InputUartSession::InputUartSession(std::unique_ptr<UartIoBackend> backend) : transport_(std::move(backend)) {
    uart_parser_init(&parser_);
}

InputUartSession::~InputUartSession() {
    Close();
}

bool InputUartSession::Open(std::string_view device_path) {
    uart_parser_reset(&parser_);
    return transport_.Open(device_path);
}

void InputUartSession::Close() noexcept {
    transport_.Close();
    uart_parser_reset(&parser_);
}

bool InputUartSession::IsOpen() const noexcept {
    return transport_.IsOpen();
}

void InputUartSession::SetSpontaneousFrameHandler(InputSpontaneousFrameHandler handler) {
    spontaneous_handler_ = std::move(handler);
}

const InputUartDiagnostics& InputUartSession::Diagnostics() const noexcept {
    return diagnostics_;
}

std::uint8_t InputUartSession::AllocateSequence() noexcept {
    const std::uint8_t allocated = next_sequence_;
    ++next_sequence_;
    return allocated;
}

bool InputUartSession::IsResponseFrame(const uart_frame_t& frame, std::uint8_t sequence) noexcept {
    return frame.sequence == sequence &&
           (frame.command == UART_CMD_RESPONSE || frame.command == UART_CMD_OPERATION_RESULT);
}

void InputUartSession::Classify(const uart_frame_t& frame, InputTransactResult& result) noexcept {
    result.response_command = frame.command;
    result.response_frame = frame;

    if (frame.command == UART_CMD_OPERATION_RESULT) {
        if (frame.length != UART_OPERATION_RESULT_PAYLOAD_SIZE) {
            result.status = InputTransactStatus::kRejected;
            result.response_status = UART_STATUS_ERROR;
            result.response_error = UART_ERROR_INVALID_PAYLOAD;
            return;
        }
        result.response_status = frame.payload[UART_OPERATION_RESULT_STATUS_INDEX];
        result.response_error = frame.payload[UART_OPERATION_RESULT_ERROR_INDEX];
    } else {
        if (frame.length < UART_RESPONSE_HEADER_SIZE) {
            result.status = InputTransactStatus::kRejected;
            result.response_status = UART_STATUS_ERROR;
            result.response_error = UART_ERROR_INVALID_PAYLOAD;
            return;
        }
        result.response_status = frame.payload[UART_RESPONSE_STATUS_INDEX];
        result.response_error = frame.payload[UART_RESPONSE_ERROR_INDEX];
    }

    result.status = (result.response_status == UART_STATUS_SUCCESS && result.response_error == UART_ERROR_NONE)
                        ? InputTransactStatus::kSuccess
                        : InputTransactStatus::kRejected;
}

void InputUartSession::RouteSpontaneous(const uart_frame_t& frame) {
    ++diagnostics_.spontaneous_frames;
    if (!spontaneous_handler_) {
        return;
    }
    try {
        spontaneous_handler_(frame);
    } catch (...) {
        // A reporting failure must not unwind into the UART receive loop.
    }
}

void InputUartSession::HandleParserResult(uart_parser_result_t result) {
    if (result == UART_PARSER_NO_FRAME || result == UART_PARSER_FRAME_READY) {
        return;
    }
    ++diagnostics_.parser_errors;
    if (result == UART_PARSER_CRC_ERROR) {
        ++diagnostics_.crc_errors;
    }
}

InputUartSession::WaitOutcome InputUartSession::WaitForResponse(std::uint8_t sequence, InputTransactResult& result) {
    const auto deadline = std::chrono::steady_clock::now() + kAckTimeout;
    std::array<std::uint8_t, kReceiveBufferSize> receive_buffer{};

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return WaitOutcome::kTimeout;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        const UartIoResult io = transport_.Read(receive_buffer, remaining);
        if (io.status == UartIoStatus::kTimeout || io.status == UartIoStatus::kWouldBlock) {
            return WaitOutcome::kTimeout;
        }
        if (!io.Succeeded()) {
            ++diagnostics_.transport_errors;
            result.io_result = io;
            result.status = InputTransactStatus::kTransportError;
            return WaitOutcome::kTransportError;
        }

        for (std::size_t index = 0; index < io.bytes_transferred; ++index) {
            uart_frame_t frame{};
            const uart_parser_result_t parser_result = uart_parser_feed(&parser_, receive_buffer[index], &frame);
            if (parser_result == UART_PARSER_FRAME_READY) {
                if (IsResponseFrame(frame, sequence)) {
                    ++diagnostics_.responses_matched;
                    Classify(frame, result);
                    return WaitOutcome::kMatched;
                }
                RouteSpontaneous(frame);
            } else {
                HandleParserResult(parser_result);
            }
        }
    }
}

bool InputUartSession::EncodeCommandFrame(std::uint8_t command, std::span<const std::uint8_t> payload,
                                          uart_frame_t& frame, std::array<std::uint8_t, UART_MAX_FRAME_SIZE>& encoded,
                                          std::size_t& encoded_length, InputTransactResult& result) {
    if (payload.size() > UART_MAX_PAYLOAD_SIZE || UART_IS_VALID_COMMAND(command) == 0U ||
        UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(command, payload.size()) == 0U) {
        result.status = InputTransactStatus::kInvalidArgument;
        return false;
    }

    frame = uart_frame_t{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = AllocateSequence();
    frame.command = command;
    frame.length = static_cast<std::uint8_t>(payload.size());
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), frame.payload);
    }

    if (uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length) != UART_CODEC_OK) {
        result.status = InputTransactStatus::kEncodeError;
        return false;
    }
    return true;
}

InputTransactResult InputUartSession::Transact(std::uint8_t command, std::span<const std::uint8_t> payload) {
    InputTransactResult result{};
    result.command = command;

    if (!IsOpen()) {
        result.status = InputTransactStatus::kNotOpen;
        result.io_result = { UartIoStatus::kNotOpen, 0, transport_.LastError() };
        return result;
    }

    uart_frame_t frame{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length = 0U;
    if (!EncodeCommandFrame(command, payload, frame, encoded, encoded_length, result)) {
        return result;
    }
    result.sequence = frame.sequence;

    ++diagnostics_.commands_sent;
    for (std::uint32_t attempt = 0U; attempt <= UART_MAX_RETRY_COUNT; ++attempt) {
        const UartIoResult io =
            transport_.WriteAll(std::span<const std::uint8_t>(encoded.data(), encoded_length), kWriteTimeout);
        if (!io.Succeeded()) {
            ++diagnostics_.transport_errors;
            result.io_result = io;
            result.status = InputTransactStatus::kTransportError;
            return result;
        }

        const WaitOutcome outcome = WaitForResponse(frame.sequence, result);
        if (outcome == WaitOutcome::kMatched || outcome == WaitOutcome::kTransportError) {
            return result;
        }

        // Timed out: discard any half-received frame so a retry's response is
        // not corrupted by stale parser state before the next send.
        uart_parser_reset(&parser_);
        if (attempt < UART_MAX_RETRY_COUNT) {
            ++diagnostics_.retries;
            ++result.retries;
            continue;
        }
    }

    ++diagnostics_.timeouts;
    result.status = InputTransactStatus::kTimeout;
    return result;
}

InputTransactResult InputUartSession::SendCommand(std::uint8_t command, std::span<const std::uint8_t> payload) {
    InputTransactResult result{};
    result.command = command;

    if (!IsOpen()) {
        result.status = InputTransactStatus::kNotOpen;
        result.io_result = { UartIoStatus::kNotOpen, 0, transport_.LastError() };
        return result;
    }

    uart_frame_t frame{};
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length = 0U;
    if (!EncodeCommandFrame(command, payload, frame, encoded, encoded_length, result)) {
        return result;
    }
    result.sequence = frame.sequence;

    ++diagnostics_.commands_sent;
    const UartIoResult io =
        transport_.WriteAll(std::span<const std::uint8_t>(encoded.data(), encoded_length), kWriteTimeout);
    if (!io.Succeeded()) {
        ++diagnostics_.transport_errors;
        result.io_result = io;
        result.status = InputTransactStatus::kTransportError;
        return result;
    }

    result.status = InputTransactStatus::kSent;
    return result;
}

UartIoResult InputUartSession::PollSpontaneous(std::chrono::milliseconds timeout) {
    if (!IsOpen()) {
        return { UartIoStatus::kNotOpen, 0, transport_.LastError() };
    }

    std::array<std::uint8_t, kReceiveBufferSize> receive_buffer{};
    const UartIoResult io = transport_.Read(receive_buffer, timeout);
    if (io.Succeeded()) {
        for (std::size_t index = 0; index < io.bytes_transferred; ++index) {
            uart_frame_t frame{};
            const uart_parser_result_t parser_result = uart_parser_feed(&parser_, receive_buffer[index], &frame);
            if (parser_result == UART_PARSER_FRAME_READY) {
                RouteSpontaneous(frame);
            } else {
                HandleParserResult(parser_result);
            }
        }
        return io;
    }
    if (io.status == UartIoStatus::kTimeout || io.status == UartIoStatus::kWouldBlock) {
        return io;
    }
    ++diagnostics_.transport_errors;
    return io;
}

}  // namespace logistics::device
