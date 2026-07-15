#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_crc16.h"
#include "logistics/contracts/uart_parser.h"

namespace {

void TestCrc16KnownVector() {
    constexpr std::array<std::uint8_t, 9> kCheck{ '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    assert(uart_crc16_ccitt(kCheck.data(), kCheck.size()) == 0x29B1U);
}

void TestCommonValidationDoesNotTruncateWideValues() {
    assert(UART_IS_VALID_COMMAND(UART_CMD_PING) != 0U);
    assert(UART_IS_VALID_COMMAND(0x101U) == 0U);
    assert(UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(UART_CMD_PING, 0U) != 0U);
    assert(UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(UART_CMD_PING, 256U) == 0U);
}

void TestInputPayloadValidation() {
    constexpr std::array<std::uint8_t, 1> kConveyor1{ UART_CONVEYOR_ID_1 };
    constexpr std::array<std::uint8_t, 1> kInvalidConveyor{ 3U };
    constexpr std::array<std::uint8_t, 2> kSpeed100{ UART_CONVEYOR_ID_2, 100U };
    constexpr std::array<std::uint8_t, 2> kSpeed101{ UART_CONVEYOR_ID_2, 101U };
    constexpr std::array<std::uint8_t, 5> kVisionResult{ 0x34U, 0x12U, UART_VISION_DETECTED, 2U, 90U };

    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_CONVEYOR_START, kConveyor1.data(), kConveyor1.size()) != 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_CONVEYOR_START, kInvalidConveyor.data(), kInvalidConveyor.size()) ==
           0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_CONVEYOR_SET_SPEED, kSpeed100.data(), kSpeed100.size()) != 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_CONVEYOR_SET_SPEED, kSpeed101.data(), kSpeed101.size()) == 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_CONTROL_RESET, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_VISION_RESULT, kVisionResult.data(), kVisionResult.size()) != 0U);
    assert(uart_vision_cycle_id(kVisionResult.data()) == 0x1234U);
    assert(UART_CONVEYOR_STATUS_PAYLOAD_SIZE == 6U);
}

void TestCodecAndParserRoundTrip() {
    uart_frame_t source{};
    source.version = UART_PROTOCOL_VERSION;
    source.sequence = 7U;
    source.command = UART_CMD_CONVEYOR_SET_SPEED;
    source.length = UART_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
    source.payload[UART_CONVEYOR_SPEED_ID_INDEX] = UART_CONVEYOR_ID_1;
    source.payload[UART_CONVEYOR_SPEED_VALUE_INDEX] = 70U;

    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length = 0U;
    assert(uart_encode_frame(&source, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);

    uart_frame_t decoded{};
    assert(uart_decode_frame(encoded.data(), encoded_length, &decoded) == UART_CODEC_OK);
    assert(decoded.sequence == source.sequence);
    assert(decoded.command == source.command);
    assert(decoded.length == source.length);
    assert(std::memcmp(decoded.payload, source.payload, source.length) == 0);

    uart_parser_t parser{};
    uart_parser_init(&parser);
    for (std::size_t index = 0U; index < encoded_length; ++index) {
        const auto result = uart_parser_feed(&parser, encoded[index], &decoded);
        assert(result == (index + 1U == encoded_length ? UART_PARSER_FRAME_READY : UART_PARSER_NO_FRAME));
    }

    encoded[UART_FRAME_HEADER_SIZE] ^= 0x01U;
    assert(uart_decode_frame(encoded.data(), encoded_length, &decoded) == UART_CODEC_CRC_MISMATCH);
}

void TestParserTimeout() {
    uart_parser_t parser{};
    uart_frame_t frame{};
    uart_parser_init(&parser);
    assert(uart_parser_feed(&parser, UART_SOF, &frame) == UART_PARSER_NO_FRAME);
    assert(uart_parser_tick(&parser, UART_PARSER_TIMEOUT_MS) == UART_PARSER_TIMEOUT);
    assert(parser.state == UART_PARSER_WAIT_SOF);
}

}  // namespace

int main() {
    TestCrc16KnownVector();
    TestCommonValidationDoesNotTruncateWideValues();
    TestInputPayloadValidation();
    TestCodecAndParserRoundTrip();
    TestParserTimeout();
    return 0;
}
