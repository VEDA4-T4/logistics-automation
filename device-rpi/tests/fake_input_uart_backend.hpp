#pragma once

// Shared test helpers: a scripted UART I/O backend and frame builders used by
// the input UART session and input node unit tests. Each test translation unit
// is its own executable, so these inline helpers never clash across tests.

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_protocol.h"
#include "logistics/device/uart_transport.hpp"

namespace input_test {

[[nodiscard]] inline std::vector<std::uint8_t> Encode(const uart_frame_t& frame) {
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> buffer{};
    std::size_t length = 0U;
    const uart_codec_result_t result = uart_encode_frame(&frame, buffer.data(), buffer.size(), &length);
    assert(result == UART_CODEC_OK);
    return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length));
}

[[nodiscard]] inline uart_frame_t MakeOperationResult(std::uint8_t sequence, std::uint8_t status, std::uint8_t error) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_OPERATION_RESULT;
    frame.length = UART_OPERATION_RESULT_PAYLOAD_SIZE;
    frame.payload[UART_OPERATION_RESULT_STATUS_INDEX] = status;
    frame.payload[UART_OPERATION_RESULT_ERROR_INDEX] = error;
    return frame;
}

[[nodiscard]] inline uart_frame_t MakeStatusResponse(std::uint8_t sequence, std::uint8_t conveyor_state,
                                                     std::uint8_t speed) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = sequence;
    frame.command = UART_CMD_RESPONSE;
    frame.length = UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE;
    frame.payload[UART_RESPONSE_STATUS_INDEX] = UART_STATUS_SUCCESS;
    frame.payload[UART_RESPONSE_COMMAND_INDEX] = UART_CMD_INPUT_CONVEYOR_GET_STATUS;
    frame.payload[UART_RESPONSE_ERROR_INDEX] = UART_ERROR_NONE;
    frame.payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] = conveyor_state;
    frame.payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] = speed;
    return frame;
}

[[nodiscard]] inline uart_frame_t MakeSensorStatus(std::uint8_t sensor_state, std::uint16_t distance_cm) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 200U;
    frame.command = UART_CMD_SENSOR_STATUS;
    frame.length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
    frame.payload[UART_SENSOR_ID_INDEX] = UART_INPUT_SENSOR_ID_1;
    frame.payload[UART_SENSOR_STATE_INDEX] = sensor_state;
    frame.payload[UART_SENSOR_DISTANCE_LOW_INDEX] = static_cast<std::uint8_t>(distance_cm & 0xFFU);
    frame.payload[UART_SENSOR_DISTANCE_HIGH_INDEX] = static_cast<std::uint8_t>((distance_cm >> 8U) & 0xFFU);
    return frame;
}

[[nodiscard]] inline uart_frame_t MakeDeviceStatus(std::uint8_t device_state, std::uint8_t error_code) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 201U;
    frame.command = UART_CMD_DEVICE_STATUS;
    frame.length = UART_DEVICE_STATUS_PAYLOAD_SIZE;
    frame.payload[UART_DEVICE_STATUS_STATE_INDEX] = device_state;
    frame.payload[UART_DEVICE_STATUS_ERROR_INDEX] = error_code;
    return frame;
}

[[nodiscard]] inline uart_frame_t MakeControllerEvent(std::uint8_t event_id) {
    uart_frame_t frame{};
    frame.version = UART_PROTOCOL_VERSION;
    frame.sequence = 202U;
    frame.command = UART_CMD_EVENT;
    frame.length = UART_EVENT_HEADER_SIZE;
    frame.payload[UART_EVENT_ID_INDEX] = event_id;
    return frame;
}

// UART backend that decodes each written request frame and, using a
// configurable responder, enqueues zero or more reply frames to be read back.
class AutoResponderBackend final : public logistics::device::UartIoBackend {
public:
    using Responder = std::function<std::vector<uart_frame_t>(const uart_frame_t& request)>;
    using UartIoResult = logistics::device::UartIoResult;
    using UartIoStatus = logistics::device::UartIoStatus;

    UartIoResult Open(std::string_view) override {
        open_ = true;
        return { UartIoStatus::kSuccess, 0, 0 };
    }
    void Close() noexcept override {
        open_ = false;
    }
    bool IsOpen() const noexcept override {
        return open_;
    }

    UartIoResult Read(std::span<std::uint8_t> buffer) override {
        if (incoming_.empty()) {
            return { UartIoStatus::kTimeout, 0, 0 };
        }
        const std::vector<std::uint8_t> chunk = std::move(incoming_.front());
        incoming_.pop_front();
        assert(chunk.size() <= buffer.size());
        std::copy(chunk.begin(), chunk.end(), buffer.begin());
        return { UartIoStatus::kSuccess, chunk.size(), 0 };
    }

    UartIoResult Write(std::span<const std::uint8_t> data) override {
        ++write_calls;
        if (fail_write) {
            return { UartIoStatus::kDisconnected, 0, ENODEV };
        }
        uart_frame_t request{};
        const uart_codec_result_t decoded = uart_decode_frame(data.data(), data.size(), &request);
        assert(decoded == UART_CODEC_OK);
        last_written = request;
        written_commands.push_back(request.command);
        if (responder) {
            for (const uart_frame_t& reply : responder(request)) {
                incoming_.push_back(Encode(reply));
            }
        }
        return { UartIoStatus::kSuccess, data.size(), 0 };
    }

    UartIoResult WaitReadable(std::chrono::milliseconds) override {
        return { UartIoStatus::kTimeout, 0, 0 };
    }
    UartIoResult WaitWritable(std::chrono::milliseconds) override {
        return { UartIoStatus::kSuccess, 0, 0 };
    }

    void PreloadIncoming(const uart_frame_t& frame) {
        incoming_.push_back(Encode(frame));
    }

    Responder responder;
    bool fail_write{ false };
    int write_calls{ 0 };
    uart_frame_t last_written{};
    std::vector<std::uint8_t> written_commands;

private:
    bool open_{ false };
    std::deque<std::vector<std::uint8_t>> incoming_;
};

// Responder that always acknowledges with OPERATION_RESULT SUCCESS.
[[nodiscard]] inline AutoResponderBackend::Responder AlwaysSucceed() {
    return [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_SUCCESS, UART_ERROR_NONE) };
    };
}

}  // namespace input_test
