#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "logistics/contracts/uart/conveyor_events.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_crc16.h"
#include "logistics/contracts/uart_parser.h"

namespace {

void TestCrc16KnownVector() {
    constexpr std::array<std::uint8_t, 9> kCheck{ '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    assert(uart_crc16_ccitt(kCheck.data(), kCheck.size()) == 0x29B1U);
}

void TestCommonValidationDoesNotTruncateWideValues() {
    static_assert(UART_CMD_GRIPPER_MIN == 0x20U);
    static_assert(UART_CMD_GRIPPER_MAX == 0x2FU);
    static_assert(UART_CMD_ROTATION_MIN == UART_CMD_GRIPPER_MIN);
    static_assert(UART_CMD_ROTATION_MAX == UART_CMD_GRIPPER_MAX);
    assert(UART_IS_VALID_COMMAND(UART_CMD_PING) != 0U);
    assert(UART_IS_VALID_COMMAND(0x101U) == 0U);
    assert(UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(UART_CMD_PING, 0U) != 0U);
    assert(UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(UART_CMD_PING, 256U) == 0U);
}

void TestInputPayloadValidation() {
    constexpr std::array<std::uint8_t, 1> kUnexpectedPayload{ 1U };
    constexpr std::array<std::uint8_t, 1> kSpeed100{ 100U };
    constexpr std::array<std::uint8_t, 1> kSpeed101{ 101U };
    constexpr std::array<std::uint8_t, 4> kInputSensorDetected{ UART_INPUT_SENSOR_ID_1, UART_SENSOR_DETECTED, 0x34U,
                                                                0x12U };
    constexpr std::array<std::uint8_t, 4> kInvalidInputSensor{ 2U, UART_SENSOR_DETECTED, 10U, 0U };
    constexpr std::array<std::uint8_t, 4> kInvalidInputSensorState{ UART_INPUT_SENSOR_ID_1, UART_SENSOR_FAULT + 1U, 10U,
                                                                    0U };

    assert(UART_IS_VALID_INPUT_COMMAND(UART_CMD_INPUT_CONVEYOR_START) != 0U);
    assert(UART_IS_VALID_INPUT_COMMAND(0x15U) == 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_INPUT_CONVEYOR_START, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_INPUT_CONVEYOR_START, kUnexpectedPayload.data(),
                                       kUnexpectedPayload.size()) == 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_INPUT_CONVEYOR_SET_SPEED, kSpeed100.data(), kSpeed100.size()) != 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_INPUT_CONVEYOR_SET_SPEED, kSpeed101.data(), kSpeed101.size()) == 0U);
    assert(UART_IS_VALID_INPUT_PAYLOAD(UART_CMD_INPUT_CONTROL_RESET, nullptr, 0U) != 0U);
    assert(uart_input_sensor_id_is_valid(UART_INPUT_SENSOR_ID_1) != 0U);
    assert(uart_input_sensor_id_is_valid(2U) == 0U);
    assert(uart_input_sensor_status_is_valid(kInputSensorDetected.data(), kInputSensorDetected.size()) != 0U);
    assert(uart_input_sensor_status_is_valid(kInvalidInputSensor.data(), kInvalidInputSensor.size()) == 0U);
    assert(uart_input_sensor_status_is_valid(kInvalidInputSensorState.data(), kInvalidInputSensorState.size()) == 0U);
    assert(uart_input_sensor_status_is_valid(nullptr, UART_SENSOR_STATUS_PAYLOAD_SIZE) == 0U);
    assert(kInputSensorDetected[UART_SENSOR_DISTANCE_CM_LOW_INDEX] == 0x34U);
    assert(kInputSensorDetected[UART_SENSOR_DISTANCE_CM_HIGH_INDEX] == 0x12U);
    assert((static_cast<std::uint16_t>(kInputSensorDetected[UART_SENSOR_DISTANCE_CM_LOW_INDEX]) |
            (static_cast<std::uint16_t>(kInputSensorDetected[UART_SENSOR_DISTANCE_CM_HIGH_INDEX]) << 8U)) == 0x1234U);
    assert(UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE == 5U);
}

void TestSortingPayloadValidation() {
    constexpr std::array<std::uint8_t, 3> kRouteDestination2{ 0x34U, 0x12U, UART_SORTING_DESTINATION_2 };
    constexpr std::array<std::uint8_t, 3> kInvalidDestination{ 0x34U, 0x12U, 4U };
    constexpr std::array<std::uint8_t, 3> kZeroCycleRoute{ 0U, 0U, UART_SORTING_DESTINATION_1 };
    constexpr std::array<std::uint8_t, 2> kCancelCycle{ 0x34U, 0x12U };
    constexpr std::array<std::uint8_t, 2> kZeroCycle{ 0U, 0U };
    constexpr std::array<std::uint8_t, 2> kReturnHomeCycle{ 0x34U, 0x12U };
    constexpr std::array<std::uint8_t, 1> kShortReturnHomeCycle{ 0x34U };
    constexpr std::array<std::uint8_t, 1> kSortingSpeed100{ 100U };
    constexpr std::array<std::uint8_t, 1> kSortingSpeed101{ 101U };
    constexpr std::array<std::uint8_t, 4> kSensorDetected{ UART_SORTING_SENSOR_ID_2, UART_SENSOR_DETECTED, 10U, 0U };
    constexpr std::array<std::uint8_t, 4> kCompleteEvent{ UART_SORTING_EVENT_CYCLE_COMPLETE, 0x34U, 0x12U,
                                                          UART_SORTING_DESTINATION_2 };

    assert(UART_IS_VALID_SORTING_COMMAND(UART_CMD_SORTING_ROUTE_ITEM) != 0U);
    assert(UART_IS_VALID_SORTING_COMMAND(UART_CMD_SORTING_RETURN_HOME) != 0U);
    assert(UART_IS_VALID_SORTING_COMMAND(UART_CMD_SORTING_MAX) == 0U);
    assert(UART_IS_VALID_SORTING_COMMAND(0x130U) == 0U);

    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_ROUTE_ITEM, kRouteDestination2.data(),
                                         kRouteDestination2.size()) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_ROUTE_ITEM, kInvalidDestination.data(),
                                         kInvalidDestination.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_ROUTE_ITEM, kZeroCycleRoute.data(), kZeroCycleRoute.size()) ==
           0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CANCEL, kCancelCycle.data(), kCancelCycle.size()) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CANCEL, nullptr, kCancelCycle.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CANCEL, kZeroCycle.data(), kZeroCycle.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RETURN_HOME, kReturnHomeCycle.data(),
                                         kReturnHomeCycle.size()) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RETURN_HOME, nullptr, kReturnHomeCycle.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RETURN_HOME, kShortReturnHomeCycle.data(),
                                         kShortReturnHomeCycle.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RETURN_HOME, kZeroCycle.data(), kZeroCycle.size()) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_GET_STATUS, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RESET, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_RESET, nullptr, 256U) == 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CONVEYOR_START, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CONVEYOR_STOP, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CONVEYOR_GET_STATUS, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CONVEYOR_SET_SPEED, kSortingSpeed100.data(),
                                         kSortingSpeed100.size()) != 0U);
    assert(UART_IS_VALID_SORTING_PAYLOAD(UART_CMD_SORTING_CONVEYOR_SET_SPEED, kSortingSpeed101.data(),
                                         kSortingSpeed101.size()) == 0U);

    assert(uart_sorting_route_cycle_id(kRouteDestination2.data()) == 0x1234U);
    assert(uart_sorting_cancel_cycle_id(kCancelCycle.data()) == 0x1234U);
    assert(uart_sorting_return_home_cycle_id(kReturnHomeCycle.data()) == 0x1234U);
    assert(uart_sorting_return_home_cycle_id(nullptr) == 0U);
    assert(uart_sorting_sensor_id_is_valid(UART_SORTING_SENSOR_ID_1) != 0U);
    assert(uart_sorting_sensor_id_is_valid(UART_SORTING_SENSOR_ID_2) != 0U);
    assert(uart_sorting_sensor_id_is_valid(UART_SORTING_SENSOR_ID_3) != 0U);
    assert(uart_sorting_sensor_id_is_valid(4U) == 0U);
    assert(uart_sorting_sensor_status_is_valid(kSensorDetected.data(), kSensorDetected.size()) != 0U);
    assert(UART_IS_VALID_SORTING_EVENT_PAYLOAD(kCompleteEvent.data(), kCompleteEvent.size()) != 0U);
    assert(uart_sorting_event_is_valid(0x01U) == 0U);
    assert(uart_sorting_event_is_valid(0x03U) == 0U);
    assert(uart_sorting_event_is_valid(0x04U) == 0U);
    assert(UART_SORTING_STATUS_PAYLOAD_SIZE == 7U);
    assert(UART_SORTING_CONVEYOR_STATUS_PAYLOAD_SIZE == 5U);
    assert(UART_SORTING_CYCLE_EVENT_PAYLOAD_SIZE == 4U);
}

void TestConveyorEventContracts() {
    std::array<std::uint8_t, APP_HEARTBEAT_PAYLOAD_SIZE> heartbeat{};
    heartbeat[UART_EVENT_ID_INDEX] = APP_EVENT_HEARTBEAT;
    heartbeat[APP_HEARTBEAT_STATE_INDEX] = UART_DEVICE_READY;
    heartbeat[APP_HEARTBEAT_ERROR_INDEX] = UART_ERROR_NONE;
    heartbeat[APP_HEARTBEAT_INPUT_SENSOR_INDEX] = UART_SENSOR_CLEAR;
    heartbeat[APP_HEARTBEAT_SORTING_SENSOR_INDEX] = UART_SENSOR_DETECTED;
    assert(UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(heartbeat.data(), heartbeat.size()) != 0U);
    assert(UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(heartbeat.data(), heartbeat.size() - 1U) == 0U);
    heartbeat[APP_HEARTBEAT_STATE_INDEX] = UART_DEVICE_EMERGENCY_STOP + 1U;
    assert(UART_IS_VALID_APP_HEARTBEAT_PAYLOAD(heartbeat.data(), heartbeat.size()) == 0U);
    heartbeat[APP_HEARTBEAT_STATE_INDEX] = UART_DEVICE_READY;

    std::array<std::uint8_t, APP_SAFETY_EVENT_PAYLOAD_SIZE> safety{};
    safety[UART_EVENT_ID_INDEX] = APP_EVENT_SAFETY;
    safety[APP_SAFETY_EVENT_KIND_INDEX] = SAFETY_EVENT_RESET_REJECTED;
    safety[APP_SAFETY_EVENT_CAUSE_INDEX] = SAFETY_CAUSE_ESTOP_INPUT_PI;
    safety[APP_SAFETY_EVENT_RESULT_INDEX] = SAFETY_RESET_INPUT_NOT_READY;
    assert(UART_IS_VALID_APP_SAFETY_PAYLOAD(safety.data(), safety.size()) != 0U);
    safety[APP_SAFETY_EVENT_KIND_INDEX] = SAFETY_EVENT_RESET_REJECTED + 1U;
    assert(UART_IS_VALID_APP_SAFETY_PAYLOAD(safety.data(), safety.size()) == 0U);
    safety[APP_SAFETY_EVENT_KIND_INDEX] = SAFETY_EVENT_RESET_REJECTED;

    std::array<std::uint8_t, APP_HEALTH_EVENT_PAYLOAD_SIZE> health{};
    health[UART_EVENT_ID_INDEX] = APP_EVENT_HEALTH;
    health[APP_HEALTH_EVENT_KIND_INDEX] = HEALTH_ISSUE_SENSOR_STALE;
    health[APP_HEALTH_EVENT_CAUSE_INDEX] = HEALTH_ISSUE_CAUSE_SORTING;
    health[APP_HEALTH_EVENT_SENSOR_ID_INDEX] = 3U;
    assert(UART_IS_VALID_APP_HEALTH_PAYLOAD(health.data(), health.size()) != 0U);
    health[APP_HEALTH_EVENT_SENSOR_ID_INDEX] = HEALTH_ISSUE_SENSOR_ID_NONE;
    assert(UART_IS_VALID_APP_HEALTH_PAYLOAD(health.data(), health.size()) == 0U);
    health[APP_HEALTH_EVENT_SENSOR_ID_INDEX] = 3U;

    uart_frame_t source{};
    source.version = UART_PROTOCOL_VERSION;
    source.sequence = 9U;
    source.command = UART_CMD_EVENT;
    source.length = static_cast<std::uint8_t>(health.size());
    std::memcpy(source.payload, health.data(), health.size());

    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t encoded_length{};
    assert(uart_encode_frame(&source, encoded.data(), encoded.size(), &encoded_length) == UART_CODEC_OK);
    uart_frame_t decoded{};
    assert(uart_decode_frame(encoded.data(), encoded_length, &decoded) == UART_CODEC_OK);
    assert(UART_IS_VALID_APP_HEALTH_PAYLOAD(decoded.payload, decoded.length) != 0U);
}

void TestLineTracerPayloadValidation() {
    constexpr std::array<std::uint8_t, 3> kAssignRouteB{ 0x34U, 0x12U, UART_LINETRACER_ROUTE_B };
    constexpr std::array<std::uint8_t, 3> kAssignWithoutJob{ 0x00U, 0x00U, UART_LINETRACER_ROUTE_B };
    constexpr std::array<std::uint8_t, 3> kAssignInvalidRoute{ 0x34U, 0x12U, 4U };
    constexpr std::array<std::uint8_t, 2> kStopJob{ 0x34U, 0x12U };
    constexpr std::array<std::uint8_t, 1> kPositionA{ UART_LINETRACER_POSITION_DEST_A };
    constexpr std::array<std::uint8_t, 1> kPositionC{ UART_LINETRACER_POSITION_DEST_C };
    constexpr std::array<std::uint8_t, 1> kPositionNone{ UART_LINETRACER_POSITION_NONE };
    constexpr std::array<std::uint8_t, 1> kInvalidPosition{ 4U };
    constexpr std::array<std::uint8_t, 4> kArrivedEvent{ UART_LINETRACER_EVENT_ARRIVED, 0x34U, 0x12U,
                                                         UART_LINETRACER_ROUTE_B };
    constexpr std::array<std::uint8_t, 5> kStateEvent{ UART_LINETRACER_EVENT_STATE_CHANGED, 0x34U, 0x12U,
                                                       UART_LINETRACER_ROUTE_B, UART_LINETRACER_STATE_FOLLOWING_LINE };
    constexpr std::array<std::uint8_t, 5> kFaultEvent{ UART_LINETRACER_EVENT_FAULT, 0x34U, 0x12U,
                                                       UART_LINETRACER_ROUTE_B, UART_ERROR_MOTOR };

    assert(UART_CMD_LINETRACER_START_ROUTE == 0x40U);
    assert(UART_CMD_LINETRACER_STOP == 0x41U);
    assert(UART_CMD_LINETRACER_GET_STATUS == 0x42U);
    assert(UART_CMD_LINETRACER_RESET == 0x43U);
    assert(UART_CMD_LINETRACER_SET_CURRENT_POSITION == 0x44U);
    assert(UART_CMD_LINETRACER_RESUME_DRIVE == 0x45U);
    assert(UART_CMD_LINETRACER_MANUAL_UNLOAD == 0x46U);
    assert(UART_CMD_LINETRACER_ASSIGN_ROUTE == UART_CMD_LINETRACER_START_ROUTE);
    assert(UART_IS_EMERGENCY_COMMAND(UART_CMD_EMERGENCY_STOP) != 0U);

    assert(UART_IS_VALID_LINETRACER_COMMAND(UART_CMD_LINETRACER_START_ROUTE) != 0U);
    assert(UART_IS_VALID_LINETRACER_COMMAND(UART_CMD_LINETRACER_MANUAL_UNLOAD) != 0U);
    assert(UART_IS_VALID_LINETRACER_COMMAND(0x47U) == 0U);
    assert(UART_IS_VALID_LINETRACER_COMMAND(0x140U) == 0U);

    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_START_ROUTE, kAssignRouteB.data(),
                                            kAssignRouteB.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_START_ROUTE, kAssignWithoutJob.data(),
                                            kAssignWithoutJob.size()) == 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_START_ROUTE, kAssignInvalidRoute.data(),
                                            kAssignInvalidRoute.size()) == 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_STOP, kStopJob.data(), kStopJob.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_STOP, nullptr, kStopJob.size()) == 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_GET_STATUS, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_RESET, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_RESUME_DRIVE, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_MANUAL_UNLOAD, nullptr, 0U) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_SET_CURRENT_POSITION, kPositionA.data(),
                                            kPositionA.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_SET_CURRENT_POSITION, kPositionC.data(),
                                            kPositionC.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_SET_CURRENT_POSITION, kPositionNone.data(),
                                            kPositionNone.size()) == 0U);
    assert(UART_IS_VALID_LINETRACER_PAYLOAD(UART_CMD_LINETRACER_SET_CURRENT_POSITION, kInvalidPosition.data(),
                                            kInvalidPosition.size()) == 0U);

    assert(uart_linetracer_start_job_id(kAssignRouteB.data()) == 0x1234U);
    assert(uart_linetracer_stop_job_id(kStopJob.data()) == 0x1234U);
    assert(uart_linetracer_position_is_valid(UART_LINETRACER_POSITION_DEST_A) != 0U);
    assert(uart_linetracer_position_is_valid(UART_LINETRACER_POSITION_NONE) == 0U);
    assert(uart_linetracer_status_position_is_valid(UART_LINETRACER_POSITION_NONE) != 0U);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(kArrivedEvent.data(), kArrivedEvent.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(kStateEvent.data(), kStateEvent.size()) != 0U);
    assert(UART_IS_VALID_LINETRACER_EVENT_PAYLOAD(kFaultEvent.data(), kFaultEvent.size()) != 0U);

    assert(UART_LINETRACER_START_PAYLOAD_SIZE == 3U);
    assert(UART_LINETRACER_STOP_PAYLOAD_SIZE == 2U);
    assert(UART_LINETRACER_SET_POSITION_PAYLOAD_SIZE == 1U);
    assert(UART_LINETRACER_RESUME_DRIVE_PAYLOAD_SIZE == 0U);
    assert(UART_LINETRACER_MANUAL_UNLOAD_PAYLOAD_SIZE == 0U);
    assert(UART_LINETRACER_STATUS_PAYLOAD_SIZE == 8U);
}

void TestCodecAndParserRoundTrip() {
    uart_frame_t source{};
    source.version = UART_PROTOCOL_VERSION;
    source.sequence = 7U;
    source.command = UART_CMD_INPUT_CONVEYOR_SET_SPEED;
    source.length = UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
    source.payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] = 70U;

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
    TestSortingPayloadValidation();
    TestConveyorEventContracts();
    TestLineTracerPayloadValidation();
    TestCodecAndParserRoundTrip();
    TestParserTimeout();
    return 0;
}
