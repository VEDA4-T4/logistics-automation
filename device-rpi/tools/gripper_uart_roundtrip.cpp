#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>

#include "logistics/contracts/uart/gripper_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_parser.h"
#include "logistics/device/uart_transport.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using logistics::device::UartIoStatus;
using logistics::device::UartTransport;

constexpr auto kResponseTimeout = 500ms;
constexpr auto kHomeTimeout = 4s;
constexpr auto kMotionTimeout = 3s;
constexpr auto kSafetyEventTimeout = 1s;
constexpr auto kSafetySettleDelay = 100ms;
constexpr std::uint8_t kMaxAttempts = UART_MAX_RETRY_COUNT + 1U;
constexpr std::uint8_t kSafetyEventId = 0x02U;
constexpr std::uint8_t kSafetyEventLatchedIndex = 1U;
constexpr std::uint8_t kSafetyEventCauseIndex = 2U;
constexpr std::uint8_t kSafetyEventPayloadSize = 3U;

struct StatusSnapshot {
    std::uint8_t state{};
    std::uint16_t motion_id{};
    std::uint16_t base_angle{};
    std::uint16_t shoulder_angle{};
    std::uint16_t elbow_angle{};
    std::uint8_t gripper_position{};
    std::uint8_t homed{};
};

[[nodiscard]] std::uint16_t ReadU16(const std::uint8_t* payload, std::size_t low_index) {
    return static_cast<std::uint16_t>(payload[low_index]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(payload[low_index + 1U]) << 8U);
}

void WriteU16(std::uint8_t* payload, std::size_t low_index, std::uint16_t value) {
    payload[low_index] = static_cast<std::uint8_t>(value & 0xFFU);
    payload[low_index + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

[[nodiscard]] const char* CommandName(std::uint8_t command) {
    switch (command) {
        case UART_CMD_GRIPPER_MOVE_ARM:
            return "MOVE_ARM";
        case UART_CMD_GRIPPER_SET_GRIPPER:
            return "SET_GRIPPER";
        case UART_CMD_GRIPPER_HOME:
            return "HOME";
        case UART_CMD_GRIPPER_GET_STATUS:
            return "GET_STATUS";
        case UART_CMD_RESET_DEVICE:
            return "RESET_DEVICE";
        case UART_CMD_RESPONSE:
            return "RESPONSE";
        case UART_CMD_EVENT:
            return "EVENT";
        case UART_CMD_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "OTHER";
    }
}

[[nodiscard]] const char* StatusName(std::uint8_t status) {
    switch (status) {
        case UART_STATUS_ACK:
            return "ACK";
        case UART_STATUS_NACK:
            return "NACK";
        case UART_STATUS_BUSY:
            return "BUSY";
        case UART_STATUS_SUCCESS:
            return "SUCCESS";
        case UART_STATUS_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] const char* StateName(std::uint8_t state) {
    switch (state) {
        case UART_GRIPPER_STATE_IDLE:
            return "IDLE";
        case UART_GRIPPER_STATE_MOVING_ARM:
            return "MOVING_ARM";
        case UART_GRIPPER_STATE_MOVING_GRIPPER:
            return "MOVING_GRIPPER";
        case UART_GRIPPER_STATE_HOMING:
            return "HOMING";
        case UART_GRIPPER_STATE_STOPPED:
            return "STOPPED";
        case UART_GRIPPER_STATE_FAULT:
            return "FAULT";
        case UART_GRIPPER_STATE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

void PrintFrame(const char* direction, const uart_frame_t& frame) {
    std::printf("  %s seq=%u command=%s(0x%02X) length=%u", direction, frame.sequence, CommandName(frame.command),
                frame.command, frame.length);
    if (frame.command == UART_CMD_RESPONSE && frame.length >= UART_RESPONSE_HEADER_SIZE) {
        std::printf(" status=%s original=0x%02X error=0x%02X", StatusName(frame.payload[UART_RESPONSE_STATUS_INDEX]),
                    frame.payload[UART_RESPONSE_COMMAND_INDEX], frame.payload[UART_RESPONSE_ERROR_INDEX]);
    }
    std::putchar('\n');
}

class Roundtrip final {
public:
    Roundtrip() {
        uart_parser_init(&parser_);
    }

    [[nodiscard]] bool Open(std::string_view path) {
        if (!transport_.Open(path)) {
            std::fprintf(stderr, "open %.*s failed: status=%d error=%d\n", static_cast<int>(path.size()), path.data(),
                         static_cast<int>(transport_.LastStatus()), transport_.LastError());
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Transact(std::uint8_t sequence, std::uint8_t command, std::span<const std::uint8_t> payload,
                                std::uint8_t expected_status, uart_frame_t* response = nullptr,
                                std::uint8_t expected_error = UART_ERROR_NONE) {
        const uart_frame_t request = BuildRequest(sequence, command, payload);

        for (std::uint8_t attempt = 1U; attempt <= kMaxAttempts; ++attempt) {
            if (!Send(request)) {
                return false;
            }
            uart_frame_t received{};
            if (WaitForResponse(sequence, command, kResponseTimeout, &received)) {
                const std::uint8_t status = received.payload[UART_RESPONSE_STATUS_INDEX];
                const std::uint8_t error = received.payload[UART_RESPONSE_ERROR_INDEX];
                if (status != expected_status || error != expected_error) {
                    std::fprintf(stderr,
                                 "  unexpected response for %s: status=%s error=0x%02X "
                                 "(expected status=%s error=0x%02X)\n",
                                 CommandName(command), StatusName(status), error, StatusName(expected_status),
                                 expected_error);
                    return false;
                }
                if (response != nullptr) {
                    *response = received;
                }
                return true;
            }
            if (attempt < kMaxAttempts) {
                std::printf("  timeout; retrying same sequence (%u/%u)\n", attempt, kMaxAttempts - 1U);
            }
        }
        std::fprintf(stderr, "  response timeout: %s sequence=%u\n", CommandName(command), sequence);
        return false;
    }

    [[nodiscard]] bool SendWithoutResponse(std::uint8_t sequence, std::uint8_t command,
                                           std::span<const std::uint8_t> payload = {}) {
        return Send(BuildRequest(sequence, command, payload));
    }

    [[nodiscard]] bool WaitForSafetyEvent(std::uint8_t expected_latched, std::uint8_t expected_cause,
                                          std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            uart_frame_t frame{};
            if (!ReadFrame(&frame, Remaining(deadline))) {
                continue;
            }
            PrintFrame("RX", frame);
            if (frame.command == UART_CMD_EVENT && frame.length == kSafetyEventPayloadSize &&
                frame.payload[UART_EVENT_ID_INDEX] == kSafetyEventId &&
                frame.payload[kSafetyEventLatchedIndex] == expected_latched &&
                frame.payload[kSafetyEventCauseIndex] == expected_cause) {
                std::printf("  safety state: latched=%u cause=0x%02X\n", expected_latched, expected_cause);
                return true;
            }
        }
        std::fprintf(stderr, "  safety event timeout: latched=%u cause=0x%02X\n", expected_latched, expected_cause);
        return false;
    }

    [[nodiscard]] bool WaitForMotion(std::uint16_t motion_id, std::uint8_t motion_type,
                                     std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            uart_frame_t frame{};
            if (!ReadFrame(&frame, Remaining(deadline))) {
                continue;
            }
            PrintFrame("RX", frame);
            if (frame.command != UART_CMD_EVENT) {
                continue;
            }
            if (frame.length == UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE &&
                frame.payload[UART_EVENT_ID_INDEX] == UART_GRIPPER_EVENT_FAULT) {
                std::fprintf(stderr, "  motion fault: id=%u type=%u error=0x%02X\n",
                             ReadU16(frame.payload, UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX),
                             frame.payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX],
                             frame.payload[UART_GRIPPER_FAULT_EVENT_ERROR_INDEX]);
                return false;
            }
            if (frame.length == UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE &&
                frame.payload[UART_EVENT_ID_INDEX] == UART_GRIPPER_EVENT_MOTION_COMPLETE &&
                ReadU16(frame.payload, UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX) == motion_id &&
                frame.payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX] == motion_type) {
                std::printf("  motion complete: id=%u type=%u\n", motion_id, motion_type);
                return true;
            }
        }
        std::fprintf(stderr, "  motion completion timeout: id=%u type=%u\n", motion_id, motion_type);
        return false;
    }

private:
    [[nodiscard]] static uart_frame_t BuildRequest(std::uint8_t sequence, std::uint8_t command,
                                                   std::span<const std::uint8_t> payload) {
        uart_frame_t request{};
        request.version = UART_PROTOCOL_VERSION;
        request.sequence = sequence;
        request.command = command;
        request.length = static_cast<std::uint8_t>(payload.size());
        if (!payload.empty()) {
            std::copy(payload.begin(), payload.end(), request.payload);
        }
        return request;
    }

    [[nodiscard]] static std::chrono::milliseconds Remaining(Clock::time_point deadline) {
        const auto now = Clock::now();
        if (now >= deadline) {
            return 0ms;
        }
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), 1ms);
    }

    [[nodiscard]] bool Send(const uart_frame_t& frame) {
        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length{};
        if (uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length) != UART_CODEC_OK) {
            std::fprintf(stderr, "encode failed for %s\n", CommandName(frame.command));
            return false;
        }
        const auto result = transport_.WriteAll(std::span<const std::uint8_t>(encoded.data(), encoded_length), 500ms);
        if (!result.Succeeded()) {
            std::fprintf(stderr, "write failed: status=%d error=%d\n", static_cast<int>(result.status),
                         result.error_code);
            return false;
        }
        PrintFrame("TX", frame);
        return true;
    }

    [[nodiscard]] bool ReadFrame(uart_frame_t* frame, std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        std::array<std::uint8_t, 1> byte{};
        while (Clock::now() < deadline) {
            const auto result = transport_.Read(byte, Remaining(deadline));
            if (result.status == UartIoStatus::kTimeout || result.status == UartIoStatus::kWouldBlock) {
                return false;
            }
            if (!result.Succeeded()) {
                std::fprintf(stderr, "read failed: status=%d error=%d\n", static_cast<int>(result.status),
                             result.error_code);
                return false;
            }
            const uart_parser_result_t parser_result = uart_parser_feed(&parser_, byte[0], frame);
            if (parser_result == UART_PARSER_FRAME_READY) {
                return true;
            }
            if (parser_result < UART_PARSER_NO_FRAME) {
                std::fprintf(stderr, "parser error: %d\n", static_cast<int>(parser_result));
            }
        }
        return false;
    }

    [[nodiscard]] bool WaitForResponse(std::uint8_t sequence, std::uint8_t original_command,
                                       std::chrono::milliseconds timeout, uart_frame_t* response) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            uart_frame_t frame{};
            if (!ReadFrame(&frame, Remaining(deadline))) {
                continue;
            }
            PrintFrame("RX", frame);
            if (frame.command == UART_CMD_RESPONSE && frame.sequence == sequence &&
                frame.length >= UART_RESPONSE_HEADER_SIZE &&
                frame.payload[UART_RESPONSE_COMMAND_INDEX] == original_command) {
                *response = frame;
                return true;
            }
        }
        return false;
    }

    UartTransport transport_{};
    uart_parser_t parser_{};
};

[[nodiscard]] bool ParseSequence(std::string_view text, std::uint8_t* sequence) {
    unsigned int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 255U) {
        return false;
    }
    *sequence = static_cast<std::uint8_t>(value);
    return true;
}

[[nodiscard]] bool DecodeStatus(const uart_frame_t& response, StatusSnapshot* status) {
    if (response.command != UART_CMD_RESPONSE || response.length != UART_GRIPPER_STATUS_PAYLOAD_SIZE ||
        response.payload[UART_RESPONSE_STATUS_INDEX] != UART_STATUS_SUCCESS ||
        response.payload[UART_RESPONSE_COMMAND_INDEX] != UART_CMD_GRIPPER_GET_STATUS ||
        response.payload[UART_RESPONSE_ERROR_INDEX] != UART_ERROR_NONE) {
        return false;
    }
    status->state = response.payload[UART_GRIPPER_STATUS_STATE_INDEX];
    status->motion_id = ReadU16(response.payload, UART_GRIPPER_STATUS_MOTION_ID_LOW_INDEX);
    status->base_angle = ReadU16(response.payload, UART_GRIPPER_STATUS_BASE_ANGLE_LOW_INDEX);
    status->shoulder_angle = ReadU16(response.payload, UART_GRIPPER_STATUS_SHOULDER_ANGLE_LOW_INDEX);
    status->elbow_angle = ReadU16(response.payload, UART_GRIPPER_STATUS_ELBOW_ANGLE_LOW_INDEX);
    status->gripper_position = response.payload[UART_GRIPPER_STATUS_POSITION_INDEX];
    status->homed = response.payload[UART_GRIPPER_STATUS_HOMED_INDEX];
    return true;
}

void PrintStatus(const StatusSnapshot& status) {
    std::printf("  state=%s motion_id=%u angles=[%.1f, %.1f, %.1f] gripper=%u%% homed=%u\n", StateName(status.state),
                status.motion_id, static_cast<double>(status.base_angle) / 10.0,
                static_cast<double>(status.shoulder_angle) / 10.0, static_cast<double>(status.elbow_angle) / 10.0,
                status.gripper_position, status.homed);
}

[[nodiscard]] bool RunHome(Roundtrip& roundtrip, std::uint8_t& sequence, std::uint16_t motion_id) {
    std::array<std::uint8_t, UART_GRIPPER_HOME_PAYLOAD_SIZE> payload{};
    WriteU16(payload.data(), UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, motion_id);

    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_HOME, payload, UART_STATUS_ACK)) {
        return false;
    }
    return roundtrip.WaitForMotion(motion_id, UART_GRIPPER_MOTION_HOME, kHomeTimeout);
}

[[nodiscard]] bool RunArmMotion(Roundtrip& roundtrip, std::uint8_t& sequence, std::uint16_t motion_id,
                                std::uint16_t base_angle, std::uint16_t shoulder_angle,
                                std::uint16_t elbow_angle, std::uint16_t duration_ms) {
    std::array<std::uint8_t, UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE> payload{};
    WriteU16(payload.data(), UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, motion_id);
    WriteU16(payload.data(), UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, base_angle);
    WriteU16(payload.data(), UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, shoulder_angle);
    WriteU16(payload.data(), UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, elbow_angle);
    WriteU16(payload.data(), UART_GRIPPER_MOVE_DURATION_LOW_INDEX, duration_ms);

    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_MOVE_ARM, payload, UART_STATUS_ACK)) {
        return false;
    }
    return roundtrip.WaitForMotion(motion_id, UART_GRIPPER_MOTION_ARM, kMotionTimeout);
}

[[nodiscard]] int RunBaseSweep(Roundtrip& roundtrip, std::uint8_t sequence, std::string_view device,
                               bool wide_sweep) {
    constexpr std::uint16_t kHomeMotionId = 100U;
    constexpr std::uint16_t kLowMotionId = 101U;
    constexpr std::uint16_t kHighMotionId = 102U;
    constexpr std::uint16_t kCenterMotionId = 103U;
    constexpr std::uint16_t kCenterAngle = 900U;
    const std::uint16_t low_angle = wide_sweep ? 450U : 600U;
    const std::uint16_t high_angle = wide_sweep ? 1350U : 1200U;
    const std::uint16_t edge_duration_ms = wide_sweep ? 2000U : 1500U;
    const std::uint16_t cross_duration_ms = wide_sweep ? 2500U : 2000U;

    std::printf("GRIPPER BASE %sCALIBRATION SWEEP\nDevice: %.*s\n", wide_sweep ? "WIDE " : "",
                static_cast<int>(device.size()), device.data());
    std::printf("Motion: 90.0 -> %.1f -> %.1f -> 90.0 degrees\n\n", static_cast<double>(low_angle) / 10.0,
                static_cast<double>(high_angle) / 10.0);

    std::printf("[1/4] HOME to 90.0 degrees\n");
    if (!RunHome(roundtrip, sequence, kHomeMotionId)) {
        return 1;
    }

    std::printf("[2/4] Move Base to %.1f degrees\n", static_cast<double>(low_angle) / 10.0);
    if (!RunArmMotion(roundtrip, sequence, kLowMotionId, low_angle, 900U, 900U, edge_duration_ms)) {
        return 1;
    }

    std::printf("[3/4] Move Base to %.1f degrees\n", static_cast<double>(high_angle) / 10.0);
    if (!RunArmMotion(roundtrip, sequence, kHighMotionId, high_angle, 900U, 900U, cross_duration_ms)) {
        return 1;
    }

    std::printf("[4/4] Return Base to 90.0 degrees\n");
    if (!RunArmMotion(roundtrip, sequence, kCenterMotionId, kCenterAngle, 900U, 900U, edge_duration_ms)) {
        return 1;
    }

    uart_frame_t response{};
    StatusSnapshot status{};
    if (!roundtrip.Transact(sequence, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_IDLE ||
        status.base_angle != kCenterAngle || status.shoulder_angle != 900U || status.elbow_angle != 900U ||
        status.homed != 1U) {
        return 1;
    }
    PrintStatus(status);
    std::printf("\nGRIPPER BASE %sCALIBRATION SWEEP: 4/4 PASS\n", wide_sweep ? "WIDE " : "");
    return 0;
}

[[nodiscard]] int RunShoulderSweep(Roundtrip& roundtrip, std::uint8_t sequence, std::string_view device) {
    constexpr std::uint16_t kHomeMotionId = 110U;
    constexpr std::uint16_t kLowMotionId = 111U;
    constexpr std::uint16_t kHighMotionId = 112U;
    constexpr std::uint16_t kCenterMotionId = 113U;
    constexpr std::uint16_t kLowAngle = 750U;
    constexpr std::uint16_t kHighAngle = 1050U;
    constexpr std::uint16_t kCenterAngle = 900U;

    std::printf("GRIPPER SHOULDER CALIBRATION SWEEP\nDevice: %.*s\n", static_cast<int>(device.size()),
                device.data());
    std::printf("Motion: 90.0 -> 75.0 -> 105.0 -> 90.0 degrees\n\n");

    std::printf("[1/4] HOME all joints to 90.0 degrees\n");
    if (!RunHome(roundtrip, sequence, kHomeMotionId)) {
        return 1;
    }

    std::printf("[2/4] Move Shoulder to 75.0 degrees\n");
    if (!RunArmMotion(roundtrip, sequence, kLowMotionId, 900U, kLowAngle, 900U, 1500U)) {
        return 1;
    }

    std::printf("[3/4] Move Shoulder to 105.0 degrees\n");
    if (!RunArmMotion(roundtrip, sequence, kHighMotionId, 900U, kHighAngle, 900U, 2000U)) {
        return 1;
    }

    std::printf("[4/4] Return Shoulder to 90.0 degrees\n");
    if (!RunArmMotion(roundtrip, sequence, kCenterMotionId, 900U, kCenterAngle, 900U, 1500U)) {
        return 1;
    }

    uart_frame_t response{};
    StatusSnapshot status{};
    if (!roundtrip.Transact(sequence, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_IDLE || status.base_angle != 900U ||
        status.shoulder_angle != kCenterAngle || status.elbow_angle != 900U || status.homed != 1U) {
        return 1;
    }
    PrintStatus(status);
    std::printf("\nGRIPPER SHOULDER CALIBRATION SWEEP: 4/4 PASS\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view device = (argc >= 2) ? argv[1] : "/dev/vedauart";
    std::uint8_t sequence = 10U;
    const std::string_view mode = (argc >= 4) ? argv[3] : "roundtrip";
    if ((argc >= 3 && !ParseSequence(argv[2], &sequence)) || argc > 4 ||
        (mode != "roundtrip" && mode != "base-sweep" && mode != "base-wide-sweep" &&
         mode != "shoulder-sweep")) {
        std::fprintf(stderr,
                     "usage: %s [device] [initial_sequence:0..255] "
                     "[roundtrip|base-sweep|base-wide-sweep|shoulder-sweep]\n",
                     argv[0]);
        return 2;
    }

    Roundtrip roundtrip;
    if (!roundtrip.Open(device)) {
        return 1;
    }

    if (mode == "base-sweep" || mode == "base-wide-sweep") {
        return RunBaseSweep(roundtrip, sequence, device, mode == "base-wide-sweep");
    }
    if (mode == "shoulder-sweep") {
        return RunShoulderSweep(roundtrip, sequence, device);
    }

    int passed = 0;
    std::printf("GRIPPER UART ROUND-TRIP TEST\nDevice: %.*s\n\n", static_cast<int>(device.size()), device.data());

    uart_frame_t response{};
    StatusSnapshot status{};
    std::printf("[1/20] Initial status\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status)) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_HOME_PAYLOAD_SIZE> home{};
    constexpr std::uint16_t kHomeMotionId = 1U;
    WriteU16(home.data(), UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, kHomeMotionId);
    const std::uint8_t home_sequence = sequence++;
    std::printf("[2/20] HOME accepted\n");
    if (!roundtrip.Transact(home_sequence, UART_CMD_GRIPPER_HOME, home, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[3/20] Duplicate HOME cached-response replay\n");
    if (!roundtrip.Transact(home_sequence, UART_CMD_GRIPPER_HOME, home, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[4/20] HOME completion event\n");
    if (!roundtrip.WaitForMotion(kHomeMotionId, UART_GRIPPER_MOTION_HOME, kHomeTimeout)) {
        return 1;
    }
    ++passed;

    std::printf("[5/20] Homed status\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_IDLE || status.homed != 1U) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE> move{};
    constexpr std::uint16_t kArmMotionId = 2U;
    WriteU16(move.data(), UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, kArmMotionId);
    WriteU16(move.data(), UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, 1000U);
    WriteU16(move.data(), UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 900U);
    WriteU16(move.data(), UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 800U);
    WriteU16(move.data(), UART_GRIPPER_MOVE_DURATION_LOW_INDEX, 500U);
    std::printf("[6/20] MOVE_ARM accepted\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_MOVE_ARM, move, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[7/20] MOVE_ARM completion event\n");
    if (!roundtrip.WaitForMotion(kArmMotionId, UART_GRIPPER_MOTION_ARM, kMotionTimeout)) {
        return 1;
    }
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_SET_GRIPPER_PAYLOAD_SIZE> grip{};
    constexpr std::uint16_t kGripMotionId = 3U;
    WriteU16(grip.data(), UART_GRIPPER_SET_MOTION_ID_LOW_INDEX, kGripMotionId);
    grip[UART_GRIPPER_SET_POSITION_INDEX] = 50U;
    WriteU16(grip.data(), UART_GRIPPER_SET_DURATION_LOW_INDEX, 500U);
    std::printf("[8/20] SET_GRIPPER accepted\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_SET_GRIPPER, grip, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[9/20] SET_GRIPPER completion event\n");
    if (!roundtrip.WaitForMotion(kGripMotionId, UART_GRIPPER_MOTION_GRIPPER, kMotionTimeout)) {
        return 1;
    }
    ++passed;

    std::printf("[10/20] Final normal-operation status\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_IDLE || status.homed != 1U ||
        status.base_angle != 1000U || status.shoulder_angle != 900U || status.elbow_angle != 800U ||
        status.gripper_position != 50U) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE> interrupted_move{};
    constexpr std::uint16_t kInterruptedMotionId = 4U;
    WriteU16(interrupted_move.data(), UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, kInterruptedMotionId);
    WriteU16(interrupted_move.data(), UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, 1200U);
    WriteU16(interrupted_move.data(), UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, 1000U);
    WriteU16(interrupted_move.data(), UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, 900U);
    WriteU16(interrupted_move.data(), UART_GRIPPER_MOVE_DURATION_LOW_INDEX, 2000U);
    std::printf("[11/20] Long MOVE_ARM accepted before E-Stop\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_MOVE_ARM, interrupted_move, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[12/20] EMERGENCY_STOP safety event\n");
    if (!roundtrip.SendWithoutResponse(sequence++, UART_CMD_EMERGENCY_STOP) ||
        !roundtrip.WaitForSafetyEvent(1U, UART_CMD_EMERGENCY_STOP, kSafetyEventTimeout)) {
        return 1;
    }
    std::this_thread::sleep_for(kSafetySettleDelay);
    ++passed;

    std::printf("[13/20] E-Stop status and interrupted motion cleared\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_EMERGENCY_STOP || status.homed != 0U ||
        status.motion_id != UART_GRIPPER_MOTION_ID_NONE) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE> blocked_move = move;
    constexpr std::uint16_t kBlockedMotionId = 5U;
    WriteU16(blocked_move.data(), UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, kBlockedMotionId);
    std::printf("[14/20] Motion rejected while E-Stop is latched\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_MOVE_ARM, blocked_move, UART_STATUS_ERROR, nullptr,
                            UART_ERROR_EMERGENCY_STOP)) {
        return 1;
    }
    ++passed;

    std::printf("[15/20] RESET_DEVICE safety release event\n");
    if (!roundtrip.SendWithoutResponse(sequence++, UART_CMD_RESET_DEVICE) ||
        !roundtrip.WaitForSafetyEvent(0U, UART_CMD_RESET_DEVICE, kSafetyEventTimeout)) {
        return 1;
    }
    std::this_thread::sleep_for(kSafetySettleDelay);
    ++passed;

    std::printf("[16/20] Released controller remains STOPPED and not homed\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_STOPPED || status.homed != 0U ||
        status.motion_id != UART_GRIPPER_MOTION_ID_NONE) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::printf("[17/20] Motion rejected until HOME is repeated\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_MOVE_ARM, blocked_move, UART_STATUS_ERROR, nullptr,
                            UART_GRIPPER_ERROR_NOT_HOMED)) {
        return 1;
    }
    ++passed;

    std::array<std::uint8_t, UART_GRIPPER_HOME_PAYLOAD_SIZE> recovery_home{};
    constexpr std::uint16_t kRecoveryHomeMotionId = 6U;
    WriteU16(recovery_home.data(), UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, kRecoveryHomeMotionId);
    std::printf("[18/20] Recovery HOME accepted\n");
    if (!roundtrip.Transact(sequence++, UART_CMD_GRIPPER_HOME, recovery_home, UART_STATUS_ACK)) {
        return 1;
    }
    ++passed;

    std::printf("[19/20] Recovery HOME completion event\n");
    if (!roundtrip.WaitForMotion(kRecoveryHomeMotionId, UART_GRIPPER_MOTION_HOME, kHomeTimeout)) {
        return 1;
    }
    ++passed;

    std::printf("[20/20] Recovered status\n");
    if (!roundtrip.Transact(sequence, UART_CMD_GRIPPER_GET_STATUS, {}, UART_STATUS_SUCCESS, &response) ||
        !DecodeStatus(response, &status) || status.state != UART_GRIPPER_STATE_IDLE || status.homed != 1U ||
        status.motion_id != UART_GRIPPER_MOTION_ID_NONE) {
        return 1;
    }
    PrintStatus(status);
    ++passed;

    std::printf("\nGRIPPER UART ROUND-TRIP SUMMARY: %d/20 PASS\n", passed);
    return (passed == 20) ? 0 : 1;
}
