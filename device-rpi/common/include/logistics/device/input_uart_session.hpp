#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart_protocol.h"
#include "logistics/device/uart_transport.hpp"

namespace logistics::device {

/*
 * Request/response UART session for the input conveyor controller.
 *
 * Unlike the line tracer controller (which confirms commands with an ACK
 * frame), the input controller answers a command with a sequence-matched
 * UART_CMD_RESPONSE (GET_STATUS) or UART_CMD_OPERATION_RESULT frame. This
 * session therefore treats one of those, matched to the pending sequence, as
 * the acknowledgement, applying the shared retry and timeout constants from
 * the contract. The transact logic mirrors the field-proven tools/vedauart CLI
 * round-trip test, kept here as a reusable component.
 *
 * SENSOR_STATUS/DEVICE_STATUS/EVENT frames and any frame that does not match
 * the pending command are handed to the spontaneous-frame handler so that
 * unsolicited controller reports are never lost, even while a command is
 * awaiting its response.
 */
enum class InputTransactStatus {
    kSuccess,          // controller reported SUCCESS / NONE
    kRejected,         // controller answered with NACK, BUSY or an error status
    kTimeout,          // no matching response after the configured retries
    kNotOpen,          // the UART device is not open
    kInvalidArgument,  // command or payload failed contract validation
    kEncodeError,      // the frame could not be encoded
    kTransportError,   // the UART device disconnected or reported an I/O error
};

struct InputTransactResult {
    InputTransactStatus status{ InputTransactStatus::kTransportError };
    std::uint8_t sequence{};
    std::uint8_t command{};
    std::uint8_t response_command{};
    std::uint8_t response_status{ UART_STATUS_ERROR };
    std::uint8_t response_error{ UART_ERROR_NONE };
    uart_frame_t response_frame{};
    std::uint8_t retries{};
    UartIoResult io_result{ UartIoStatus::kSuccess, 0, 0 };

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == InputTransactStatus::kSuccess;
    }
};

struct InputUartDiagnostics {
    std::uint64_t commands_sent{};
    std::uint64_t responses_matched{};
    std::uint64_t retries{};
    std::uint64_t timeouts{};
    std::uint64_t spontaneous_frames{};
    std::uint64_t parser_errors{};
    std::uint64_t crc_errors{};
    std::uint64_t transport_errors{};
};

using InputSpontaneousFrameHandler = std::function<void(const uart_frame_t&)>;

class InputUartSession final {
public:
    InputUartSession();
    explicit InputUartSession(std::unique_ptr<UartIoBackend> backend);
    ~InputUartSession();

    InputUartSession(const InputUartSession&) = delete;
    InputUartSession& operator=(const InputUartSession&) = delete;
    InputUartSession(InputUartSession&&) = delete;
    InputUartSession& operator=(InputUartSession&&) = delete;

    [[nodiscard]] bool Open(std::string_view device_path = UartTransport::kDefaultDevicePath);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    void SetSpontaneousFrameHandler(InputSpontaneousFrameHandler handler);

    [[nodiscard]] InputTransactResult Transact(std::uint8_t command, std::span<const std::uint8_t> payload = {});
    [[nodiscard]] UartIoResult PollSpontaneous(std::chrono::milliseconds timeout);

    [[nodiscard]] const InputUartDiagnostics& Diagnostics() const noexcept;

private:
    static constexpr std::size_t kReceiveBufferSize = UART_MAX_FRAME_SIZE * 2U;

    enum class WaitOutcome {
        kMatched,
        kTimeout,
        kTransportError,
    };

    [[nodiscard]] WaitOutcome WaitForResponse(std::uint8_t sequence, InputTransactResult& result);
    void Classify(const uart_frame_t& frame, InputTransactResult& result) noexcept;
    [[nodiscard]] static bool IsResponseFrame(const uart_frame_t& frame, std::uint8_t sequence) noexcept;
    void RouteSpontaneous(const uart_frame_t& frame);
    void HandleParserResult(uart_parser_result_t result);
    [[nodiscard]] std::uint8_t AllocateSequence() noexcept;

    UartTransport transport_;
    uart_parser_t parser_{};
    std::uint8_t next_sequence_{ 1U };
    InputSpontaneousFrameHandler spontaneous_handler_;
    InputUartDiagnostics diagnostics_{};
};

}  // namespace logistics::device
