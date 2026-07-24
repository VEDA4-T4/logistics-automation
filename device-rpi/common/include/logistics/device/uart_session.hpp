#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart_protocol.h"
#include "logistics/device/uart_transport.hpp"

namespace logistics::device {

enum class UartSessionEventType {
    kFrameReceived,
    kAckReceived,
    kCommandResponseReceived,
    kDuplicateFrame,
    kUnexpectedAck,
    kUnexpectedCommandResponse,
    kParserError,
    kCommandRetried,
    kAckTimeout,
    kTransportDisconnected,
    kTransportError,
};

struct UartSessionEvent {
    UartSessionEventType type{ UartSessionEventType::kTransportError };
    uart_frame_t frame{};
    uart_parser_result_t parser_result{ UART_PARSER_NO_FRAME };
    UartIoResult io_result{ UartIoStatus::kSuccess, 0, 0 };
    std::uint8_t pending_sequence{};
    std::uint8_t pending_command{};
    std::uint8_t retry_count{};
};

using UartSessionEventHandler = std::function<void(const UartSessionEvent&)>;

enum class UartSessionSendStatus {
    kSent,
    kNotOpen,
    kBusy,
    kInvalidArgument,
    kEncodeError,
    kTransportError,
};

struct UartSessionSendResult {
    UartSessionSendStatus status{ UartSessionSendStatus::kTransportError };
    std::uint8_t sequence{};
    uart_codec_result_t codec_result{ UART_CODEC_OK };
    UartIoResult io_result{ UartIoStatus::kSuccess, 0, 0 };

    [[nodiscard]] bool Succeeded() const noexcept;
};

struct UartSessionDiagnostics {
    std::uint64_t frames_received{};
    std::uint64_t duplicate_frames{};
    std::uint64_t parser_errors{};
    std::uint64_t crc_errors{};
    std::uint64_t unexpected_acks{};
    std::uint64_t command_responses{};
    std::uint64_t unexpected_command_responses{};
    std::uint64_t command_retries{};
    std::uint64_t ack_timeouts{};
    std::uint64_t transport_disconnects{};
    std::uint64_t transport_errors{};
};

class UartSession final {
public:
    UartSession();
    explicit UartSession(std::unique_ptr<UartIoBackend> backend);
    ~UartSession();

    UartSession(const UartSession&) = delete;
    UartSession& operator=(const UartSession&) = delete;
    UartSession(UartSession&&) = delete;
    UartSession& operator=(UartSession&&) = delete;

    [[nodiscard]] bool Open(std::string_view device_path = UartTransport::kDefaultDevicePath);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    void SetEventHandler(UartSessionEventHandler handler);

    [[nodiscard]] UartSessionSendResult SendCommand(std::uint8_t command, std::span<const std::uint8_t> payload = {});
    [[nodiscard]] UartIoResult PollOnce(std::chrono::milliseconds read_timeout = std::chrono::milliseconds{ 0 });
    void Tick(std::chrono::milliseconds elapsed);

    [[nodiscard]] bool HasPendingCommand() const noexcept;
    [[nodiscard]] std::uint8_t PendingSequence() const noexcept;
    [[nodiscard]] std::uint8_t PendingCommandCode() const noexcept;
    [[nodiscard]] const UartSessionDiagnostics& Diagnostics() const noexcept;

private:
    static constexpr std::size_t kReceiveBufferSize = UART_MAX_FRAME_SIZE * 2U;
    static constexpr std::size_t kRecentFrameCount = 16U;

    struct PendingCommand {
        bool active{};
        bool waiting_to_retry{};
        uart_frame_t frame{};
        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length{};
        std::uint16_t payload_crc{};
        std::uint32_t elapsed_ms{};
        std::uint8_t retry_count{};
    };

    struct FrameSignature {
        bool valid{};
        std::uint8_t sequence{};
        std::uint8_t command{};
        std::uint16_t crc{};
    };

    void ResetLinkState() noexcept;
    void ProcessBytes(std::span<const std::uint8_t> bytes);
    void HandleReadyFrame(const uart_frame_t& frame);
    void HandleAck(const uart_frame_t& frame);
    void HandleCommandResponse(const uart_frame_t& frame);
    void HandleParserResult(uart_parser_result_t result);
    void HandleTransportFailure(UartIoResult result);
    void RetryPendingCommand();
    void Emit(UartSessionEvent event) const;
    [[nodiscard]] bool IsDuplicate(const uart_frame_t& frame) const noexcept;
    void Remember(const uart_frame_t& frame) noexcept;
    [[nodiscard]] std::uint8_t AllocateSequence() noexcept;

    UartTransport transport_;
    uart_parser_t parser_{};
    PendingCommand pending_{};
    std::array<FrameSignature, kRecentFrameCount> recent_frames_{};
    std::size_t recent_frame_index_{};
    std::uint8_t next_sequence_{ 1U };
    UartSessionEventHandler event_handler_;
    UartSessionDiagnostics diagnostics_{};
};

}  // namespace logistics::device
