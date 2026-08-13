#include <cstdint>
#include <iostream>

extern "C" {
#include "sensor_logic.h"
}

namespace {

int g_failures = 0;

static_assert(APP_MARKER_NONE == 0);
static_assert(APP_MARKER_JUNCTION == 1);
static_assert(APP_MARKER_DEST_A == 2);
static_assert(APP_MARKER_DEST_B == 3);
static_assert(APP_MARKER_DEST_C == 4);
static_assert(APP_MARKER_INVALID == 5);

#define CHECK_TRUE(condition)                                                                    \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::cerr << __func__ << ":" << __LINE__ << " check failed: " << #condition << '\n'; \
            ++g_failures;                                                                        \
        }                                                                                        \
    } while (0)

sensor_logic_update_t UpdateLine(sensor_logic_context_t& context, std::uint8_t left, std::uint8_t right,
                                 std::uint32_t now_ms) {
    sensor_logic_update_t update{};
    SensorLogic_UpdateLine(&context, left, 1U, right, now_ms, &update);
    return update;
}

sensor_logic_update_t UpdateLineWithCenter(sensor_logic_context_t& context, std::uint8_t left, std::uint8_t center,
                                           std::uint8_t right, std::uint32_t now_ms) {
    sensor_logic_update_t update{};
    SensorLogic_UpdateLine(&context, left, center, right, now_ms, &update);
    return update;
}

sensor_logic_update_t UpdateFsr(sensor_logic_context_t& context, std::uint16_t raw, std::uint32_t now_ms) {
    sensor_logic_update_t update{};
    SensorLogic_UpdateFsr(&context, raw, now_ms, &update);
    return update;
}

void InitializeCentered(sensor_logic_context_t& context, std::uint32_t start_ms) {
    (void)UpdateLine(context, 0U, 0U, start_ms);
    (void)UpdateLine(context, 0U, 0U, start_ms + 10U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 20U);
}

std::uint32_t EmitMarkerStripe(sensor_logic_context_t& context, std::uint32_t start_ms) {
    (void)UpdateLine(context, 1U, 1U, start_ms);
    (void)UpdateLine(context, 1U, 1U, start_ms + 10U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 20U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 60U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 70U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 80U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 130U);
    return start_ms + 80U;
}

sensor_logic_update_t EmitMarkerGroup(sensor_logic_context_t& context, std::uint8_t stripe_count,
                                      std::uint32_t first_stripe_at_ms, std::uint32_t& last_stripe_at_ms) {
    auto next_stripe_at_ms = first_stripe_at_ms;

    for (std::uint8_t index = 0U; index < stripe_count; ++index) {
        last_stripe_at_ms = EmitMarkerStripe(context, next_stripe_at_ms);
        next_stripe_at_ms = last_stripe_at_ms + 60U;
    }

    auto update = UpdateLine(context, 0U, 0U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS - 1U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    return UpdateLine(context, 0U, 0U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS);
}

void TestMarkerMessageContract() {
    app_sensor_snapshot_t snapshot{};
    sensor_logic_context_t context{};

    CHECK_TRUE(snapshot.marker_code == APP_MARKER_NONE);
    CHECK_TRUE(snapshot.marker_count == 0U);
    CHECK_TRUE(snapshot.marker_detected_at_ms == 0U);
    CHECK_TRUE(snapshot.marker_active == 0U);

    SensorLogic_Init(&context, 10U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_NONE);
    CHECK_TRUE(context.snapshot.marker_count == 0U);
    CHECK_TRUE(context.snapshot.marker_detected_at_ms == 0U);
    CHECK_TRUE(context.snapshot.marker_active == 0U);

    snapshot.marker_code = APP_MARKER_DEST_C;
    snapshot.marker_count = 4U;
    snapshot.marker_detected_at_ms = 1234U;

    CHECK_TRUE(snapshot.marker_code == APP_MARKER_DEST_C);
    CHECK_TRUE(snapshot.marker_count == 4U);
    CHECK_TRUE(snapshot.marker_detected_at_ms == 1234U);
}

void TestLineNormalizationAndDebounce() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    CHECK_TRUE(UpdateLine(context, 0U, 0U, 0U).event_flags == 0U);
    CHECK_TRUE(UpdateLine(context, 0U, 0U, 10U).event_flags == 0U);
    auto update = UpdateLine(context, 0U, 0U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);

    CHECK_TRUE(UpdateLine(context, 1U, 0U, 30U).event_flags == 0U);
    CHECK_TRUE(UpdateLine(context, 0U, 0U, 40U).event_flags == 0U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);

    CHECK_TRUE(UpdateLine(context, 1U, 0U, 50U).event_flags == 0U);
    CHECK_TRUE(UpdateLine(context, 1U, 0U, 60U).event_flags == 0U);
    update = UpdateLine(context, 1U, 0U, 70U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_LEFT_ONLY);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);
    CHECK_TRUE(UpdateLine(context, 1U, 0U, 80U).event_flags == 0U);

    (void)UpdateLine(context, 0U, 1U, 90U);
    (void)UpdateLine(context, 0U, 1U, 100U);
    update = UpdateLine(context, 0U, 1U, 110U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_RIGHT_ONLY);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);

    (void)UpdateLine(context, 1U, 1U, 120U);
    (void)UpdateLine(context, 1U, 1U, 130U);
    update = UpdateLine(context, 1U, 1U, 140U);
    CHECK_TRUE(context.snapshot.marker_active != 0U);
    CHECK_TRUE(context.diagnostics.marker_active != 0U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_RIGHT_ONLY);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) == 0U);

    (void)UpdateLine(context, 0U, 0U, 150U);
    (void)UpdateLine(context, 0U, 0U, 160U);
    update = UpdateLine(context, 0U, 0U, 170U);
    CHECK_TRUE(context.snapshot.marker_active == 0U);
    CHECK_TRUE(context.diagnostics.marker_active == 0U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);
}

void TestOuterSensorsDetectMarkerWhenCenterMisses() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 0U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 10U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 20U);

    (void)UpdateLineWithCenter(context, 1U, 0U, 1U, 30U);
    (void)UpdateLineWithCenter(context, 1U, 0U, 1U, 40U);
    auto update = UpdateLineWithCenter(context, 1U, 0U, 1U, 50U);
    CHECK_TRUE(update.event_flags == 0U);
    CHECK_TRUE(context.snapshot.marker_active != 0U);

    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 90U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 100U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 110U);
    update = UpdateLineWithCenter(context, 0U, 0U, 0U, 110U + SENSOR_MARKER_GROUP_TIMEOUT_MS);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_JUNCTION);
    CHECK_TRUE(context.snapshot.marker_count == 1U);
}

void TestAnalogLineSamples() {
    sensor_logic_context_t context{};
    constexpr uint16_t kLeftMidpointRaw = (SENSOR_LINE_LEFT_WHITE_RAW + SENSOR_LINE_LEFT_BLACK_RAW) / 2U;

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, kLeftMidpointRaw, SENSOR_LINE_RIGHT_BLACK_RAW);

    CHECK_TRUE(context.snapshot.line_left_raw == kLeftMidpointRaw);
    CHECK_TRUE(context.snapshot.line_right_raw == SENSOR_LINE_RIGHT_BLACK_RAW);
    CHECK_TRUE(context.line_left_filtered == 500U);
    CHECK_TRUE(context.line_right_filtered == 1000U);
    CHECK_TRUE(context.snapshot.line_error == -500);
    CHECK_TRUE(context.line_left_black == 0U);
    CHECK_TRUE(context.line_right_black != 0U);

    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_BLACK_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    CHECK_TRUE(context.line_left_filtered == 625U);
    CHECK_TRUE(context.line_right_filtered == 750U);
    CHECK_TRUE(context.snapshot.line_error == -125);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, 0U, 4095U);
    CHECK_TRUE(context.line_left_filtered == 0U);
    CHECK_TRUE(context.line_right_filtered == SENSOR_LINE_NORMALIZED_MAX);
    CHECK_TRUE(context.snapshot.line_error == -1000);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, 4095U, 0U);
    CHECK_TRUE(context.line_left_filtered == SENSOR_LINE_NORMALIZED_MAX);
    CHECK_TRUE(context.line_right_filtered == 0U);
    CHECK_TRUE(context.snapshot.line_error == 1000);
}

void TestAnalogBlackHysteresis() {
    sensor_logic_context_t context{};
    constexpr std::uint16_t kLeftMidRaw =
        SENSOR_LINE_LEFT_WHITE_RAW +
        ((SENSOR_LINE_LEFT_BLACK_RAW - SENSOR_LINE_LEFT_WHITE_RAW) * 550U) / SENSOR_LINE_NORMALIZED_MAX;

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_BLACK_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    CHECK_TRUE(context.line_left_black != 0U);
    CHECK_TRUE(context.line_right_black == 0U);

    for (std::uint8_t index = 0U; index < 16U; ++index) {
        SensorLogic_UpdateLineAnalogRaw(&context, kLeftMidRaw, SENSOR_LINE_RIGHT_WHITE_RAW);
    }
    CHECK_TRUE(context.line_left_filtered > SENSOR_LINE_BLACK_EXIT_THRESHOLD);
    CHECK_TRUE(context.line_left_filtered < SENSOR_LINE_BLACK_ENTER_THRESHOLD);
    CHECK_TRUE(context.line_left_black != 0U);

    for (std::uint8_t index = 0U; index < 8U; ++index) {
        SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_WHITE_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    }
    CHECK_TRUE(context.line_left_filtered <= SENSOR_LINE_BLACK_EXIT_THRESHOLD);
    CHECK_TRUE(context.line_left_black == 0U);
}

void TestCenterDigitalInputOwnsBlackState() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);

    SensorLogic_UpdateLineCenter(&context, 0U, SENSOR_LINE_CENTER_BLACK_RAW);
    CHECK_TRUE(context.snapshot.line_center_raw == SENSOR_LINE_CENTER_BLACK_RAW);
    CHECK_TRUE(context.line_center_black == 0U);
    CHECK_TRUE(context.snapshot.line_center == 0U);

    SensorLogic_UpdateLineCenter(&context, 1U, SENSOR_LINE_CENTER_WHITE_RAW);
    CHECK_TRUE(context.snapshot.line_center_raw == SENSOR_LINE_CENTER_WHITE_RAW);
    CHECK_TRUE(context.line_center_black != 0U);
    CHECK_TRUE(context.snapshot.line_center != 0U);
}

void TestThreeSensorTrackingState() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 0U);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 10U);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);

    (void)UpdateLineWithCenter(context, 1U, 1U, 0U, 30U);
    (void)UpdateLineWithCenter(context, 1U, 1U, 0U, 40U);
    (void)UpdateLineWithCenter(context, 1U, 1U, 0U, 50U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_LEFT_ONLY);

    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 60U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 70U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 80U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);

    (void)UpdateLineWithCenter(context, 1U, 1U, 1U, 90U);
    (void)UpdateLineWithCenter(context, 1U, 1U, 1U, 100U);
    (void)UpdateLineWithCenter(context, 1U, 1U, 1U, 110U);
    CHECK_TRUE(context.snapshot.marker_active != 0U);
}

void TestAnalogLineErrorDeadband() {
    sensor_logic_context_t context{};
    constexpr std::uint16_t kLeftInsideDeadband =
        SENSOR_LINE_LEFT_WHITE_RAW +
        ((SENSOR_LINE_LEFT_BLACK_RAW - SENSOR_LINE_LEFT_WHITE_RAW) * SENSOR_LINE_ERROR_DEADBAND) /
            SENSOR_LINE_NORMALIZED_MAX;
    constexpr std::uint16_t kLeftOutsideDeadband =
        SENSOR_LINE_LEFT_WHITE_RAW +
        ((SENSOR_LINE_LEFT_BLACK_RAW - SENSOR_LINE_LEFT_WHITE_RAW) * (SENSOR_LINE_ERROR_DEADBAND + 100U)) /
            SENSOR_LINE_NORMALIZED_MAX;

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, kLeftInsideDeadband, SENSOR_LINE_RIGHT_WHITE_RAW);
    CHECK_TRUE(context.snapshot.line_error == 0);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, kLeftOutsideDeadband, SENSOR_LINE_RIGHT_WHITE_RAW);
    CHECK_TRUE(context.snapshot.line_error > (int16_t)SENSOR_LINE_ERROR_DEADBAND);
}

void TestDigitalLineStateOwnsPidDirection() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_WHITE_RAW, SENSOR_LINE_RIGHT_BLACK_RAW);
    (void)UpdateLineWithCenter(context, 1U, 0U, 0U, 0U);
    (void)UpdateLineWithCenter(context, 1U, 0U, 0U, 10U);
    (void)UpdateLineWithCenter(context, 1U, 0U, 0U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_LEFT_ONLY);
    CHECK_TRUE(context.snapshot.line_error >= (int16_t)SENSOR_LINE_DO_PID_MIN_ERROR);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_BLACK_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    (void)UpdateLineWithCenter(context, 0U, 0U, 1U, 0U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 1U, 10U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 1U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_RIGHT_ONLY);
    CHECK_TRUE(context.snapshot.line_error <= -(int16_t)SENSOR_LINE_DO_PID_MIN_ERROR);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_BLACK_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 0U);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 10U);
    (void)UpdateLineWithCenter(context, 0U, 1U, 0U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);
    CHECK_TRUE(context.snapshot.line_error > (int16_t)SENSOR_LINE_ERROR_DEADBAND);

    SensorLogic_Init(&context, 0U);
    SensorLogic_UpdateLineAnalogRaw(&context, SENSOR_LINE_LEFT_BLACK_RAW, SENSOR_LINE_RIGHT_WHITE_RAW);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 0U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 10U);
    (void)UpdateLineWithCenter(context, 0U, 0U, 0U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);
    CHECK_TRUE(context.snapshot.line_error == 0);
}

void TestMarkerGroupClassification() {
    struct MarkerCase {
        std::uint8_t count;
        app_marker_code_t expected_code;
    };

    constexpr MarkerCase kCases[] = {
        { 1U, APP_MARKER_JUNCTION }, { 2U, APP_MARKER_DEST_A },  { 3U, APP_MARKER_DEST_B },
        { 4U, APP_MARKER_DEST_C },   { 5U, APP_MARKER_INVALID },
    };

    for (const auto& marker_case : kCases) {
        sensor_logic_context_t context{};
        std::uint32_t last_stripe_at_ms = 0U;

        SensorLogic_Init(&context, 0U);
        InitializeCentered(context, 0U);
        const auto update = EmitMarkerGroup(context, marker_case.count, 30U, last_stripe_at_ms);

        CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
        CHECK_TRUE(context.snapshot.marker_code == marker_case.expected_code);
        CHECK_TRUE(context.snapshot.marker_count == marker_case.count);
        CHECK_TRUE(context.snapshot.marker_detected_at_ms == last_stripe_at_ms);
        CHECK_TRUE(context.diagnostics.marker_sequence == 1U);
        CHECK_TRUE(context.latest_marker_event.code == marker_case.expected_code);
        CHECK_TRUE(context.latest_marker_event.count == marker_case.count);
        CHECK_TRUE(context.latest_marker_event.type == ((marker_case.expected_code == APP_MARKER_INVALID)
                                                            ? SENSOR_MARKER_EVENT_INVALID_COUNT
                                                            : SENSOR_MARKER_EVENT_DETECTED));
        CHECK_TRUE((UpdateLine(context, 0U, 0U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS + 10U).event_flags &
                    APP_SENSOR_EVENT_MARKER) == 0U);
    }
}

void TestInvalidMarkerWidthAndNoise() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    InitializeCentered(context, 0U);
    (void)UpdateLine(context, 1U, 1U, 30U);
    (void)UpdateLine(context, 1U, 1U, 31U);
    (void)UpdateLine(context, 1U, 1U, 32U);
    (void)UpdateLine(context, 0U, 0U, 33U);
    (void)UpdateLine(context, 0U, 0U, 34U);
    auto update = UpdateLine(context, 0U, 0U, 35U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    CHECK_TRUE(context.marker_group_count == 0U);

    SensorLogic_Init(&context, 0U);
    InitializeCentered(context, 0U);
    (void)UpdateLine(context, 1U, 1U, 30U);
    (void)UpdateLine(context, 1U, 1U, 40U);
    (void)UpdateLine(context, 1U, 1U, 50U);
    (void)UpdateLine(context, 0U, 0U, SENSOR_MARKER_MAX_BLACK_MS + 60U);
    (void)UpdateLine(context, 0U, 0U, SENSOR_MARKER_MAX_BLACK_MS + 70U);
    update = UpdateLine(context, 0U, 0U, SENSOR_MARKER_MAX_BLACK_MS + 80U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_INVALID);
    CHECK_TRUE(context.latest_marker_event.type == SENSOR_MARKER_EVENT_INVALID_WIDTH);
}

void TestFsrStabilityAndHysteresis() {
    sensor_logic_context_t context{};
    std::uint32_t baseline_ready_count = 0U;
    std::uint32_t load_on_count = 0U;
    std::uint32_t load_off_count = 0U;

    SensorLogic_Init(&context, 0U);
    SensorLogic_StartFsrBaselineCapture(&context, SENSOR_FSR_BASELINE_FOR_LOAD_ON);
    for (std::uint32_t now = 0U; now < (SENSOR_FSR_BASELINE_SAMPLES * 10U); now += 10U) {
        const auto update = UpdateFsr(context, 1700U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_FSR_BASELINE_READY) != 0U) {
            ++baseline_ready_count;
        }
    }
    CHECK_TRUE(baseline_ready_count == 1U);
    CHECK_TRUE(context.fsr_baseline_valid != 0U);
    CHECK_TRUE(context.diagnostics.fsr_empty_baseline == 1700U);

    for (std::uint32_t now = 160U; now <= 500U; now += 10U) {
        auto update = UpdateFsr(context, ((now / 10U) % 2U == 0U) ? 1850U : 1900U, now);
        CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LOAD_ON) == 0U);
    }
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_EMPTY);

    for (std::uint32_t now = 510U; now <= 870U; now += 10U) {
        auto update = UpdateFsr(context, 2000U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_ON) != 0U) {
            ++load_on_count;
        }
    }
    CHECK_TRUE(load_on_count == 1U);
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    SensorLogic_StartFsrBaselineCapture(&context, SENSOR_FSR_BASELINE_FOR_LOAD_OFF);
    baseline_ready_count = 0U;
    for (std::uint32_t now = 880U; now < 880U + (SENSOR_FSR_BASELINE_SAMPLES * 10U); now += 10U) {
        const auto update = UpdateFsr(context, 2000U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_FSR_BASELINE_READY) != 0U) {
            ++baseline_ready_count;
        }
    }
    CHECK_TRUE(baseline_ready_count == 1U);

    for (std::uint32_t now = 1120U; now <= 1290U; now += 10U) {
        auto update = UpdateFsr(context, 1850U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) != 0U) {
            ++load_off_count;
        }
    }
    CHECK_TRUE(load_off_count == 0U);
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    for (std::uint32_t now = 1300U; now <= 1650U; now += 10U) {
        auto update = UpdateFsr(context, 1700U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) != 0U) {
            ++load_off_count;
        }
    }
    CHECK_TRUE(load_off_count == 1U);
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_EMPTY);
}

void TestOverloadAndObstacleHysteresis() {
    sensor_logic_context_t context{};
    std::uint32_t overload_count = 0U;

    SensorLogic_Init(&context, 0U);
    for (std::uint32_t now = 0U; now <= 350U; now += 10U) {
        auto update = UpdateFsr(context, 4095U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_OVERLOAD) != 0U) {
            ++overload_count;
            CHECK_TRUE((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OVERLOAD) != 0U);
        }
    }
    CHECK_TRUE(overload_count == 1U);
    CHECK_TRUE(context.diagnostics.overload_active == 1U);

    sensor_logic_update_t update{};
    for (std::uint32_t sample = 1U; sample < SENSOR_OBSTACLE_ACTIVATE_SAMPLES; ++sample) {
        update = {};
        SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U, 400U + (sample * 10U), &update);
        CHECK_TRUE(update.event_flags == 0U);
        CHECK_TRUE(context.diagnostics.obstacle_mask == 0U);
    }

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U,
                                 400U + (SENSOR_OBSTACLE_ACTIVATE_SAMPLES * 10U), &update);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) != 0U);
    CHECK_TRUE((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_OFF_MM - 1U, 1U, 500U, &update);
    CHECK_TRUE(update.event_flags == 0U);
    CHECK_TRUE(context.diagnostics.obstacle_mask != 0U);

    for (std::uint32_t sample = 1U; sample < SENSOR_OBSTACLE_CLEAR_SAMPLES; ++sample) {
        update = {};
        SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_OFF_MM, 1U, 510U + (sample * 10U), &update);
        CHECK_TRUE(update.event_flags == 0U);
        CHECK_TRUE(context.diagnostics.obstacle_mask != 0U);
    }

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_OFF_MM, 1U, 510U + (SENSOR_OBSTACLE_CLEAR_SAMPLES * 10U),
                                 &update);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) != 0U);
    CHECK_TRUE((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);
}

void TestUltrasonicSuspendClearsObstacleAndStaleness() {
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U, 10U, &update);
    CHECK_TRUE((context.diagnostics.obstacle_mask & SENSOR_LOGIC_DIRECTION_FRONT) != 0U);

    update = {};
    SensorLogic_SuspendUltrasonic(&context, 20U, &update);
    CHECK_TRUE(context.diagnostics.obstacle_mask == 0U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) != 0U);
    CHECK_TRUE((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);

    SensorLogic_CheckStaleness(&context, SENSOR_ULTRASONIC_STALE_MS + 100U);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
}

void TestPendingEventLatch() {
    sensor_event_latch_t latch{};

    SensorEventLatch_Init(&latch);
    CHECK_TRUE(SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_MARKER) == APP_SENSOR_EVENT_MARKER);
    CHECK_TRUE(SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_NONE) == APP_SENSOR_EVENT_MARKER);

    (void)SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_LOAD_ON);
    SensorEventLatch_Acknowledge(&latch, APP_SENSOR_EVENT_MARKER);
    CHECK_TRUE(latch.pending_flags == APP_SENSOR_EVENT_LOAD_ON);
    SensorEventLatch_Acknowledge(&latch, APP_SENSOR_EVENT_LOAD_ON);
    CHECK_TRUE(latch.pending_flags == APP_SENSOR_EVENT_NONE);
}

void TestSensorErrorsAndStaleness() {
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, 10U, &update);
    CHECK_TRUE((context.diagnostics.valid_flags & SENSOR_LOGIC_VALID_ULTRASONIC_FRONT) != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 20U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    CHECK_TRUE((context.diagnostics.valid_flags & SENSOR_LOGIC_VALID_ULTRASONIC_FRONT) != 0U);

    SensorLogic_CheckStaleness(&context, 10U + SENSOR_ULTRASONIC_STALE_MS - 1U);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    SensorLogic_CheckStaleness(&context, 10U + SENSOR_ULTRASONIC_STALE_MS);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);
    CHECK_TRUE((context.diagnostics.valid_flags & SENSOR_LOGIC_VALID_ULTRASONIC_FRONT) == 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, SENSOR_ULTRASONIC_STALE_MS + 20U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, SENSOR_ULTRASONIC_STALE_MS + 30U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, SENSOR_ULTRASONIC_STALE_MS + 40U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, SENSOR_ULTRASONIC_STALE_MS + 50U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, SENSOR_ULTRASONIC_STALE_MS + 60U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    CHECK_TRUE((context.diagnostics.valid_flags & SENSOR_LOGIC_VALID_ULTRASONIC_FRONT) != 0U);

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U, 10U, &update);
    update = {};
    for (std::uint32_t sample = 1U; sample < SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES; ++sample) {
        SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 10U + (sample * 10U), &update);
    }
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 10U + (SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES * 10U),
                                 &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);

    SensorLogic_Init(&context, 0U);
    SensorLogic_CheckStaleness(&context, SENSOR_FSR_ADC_TIMEOUT_MS);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_FSR_TIMEOUT) != 0U);
    CHECK_TRUE(context.snapshot.fsr_valid == 0U);
    update = {};
    SensorLogic_UpdateFsr(&context, 1000U, SENSOR_FSR_ADC_TIMEOUT_MS + 1U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_FSR_TIMEOUT) == 0U);
    CHECK_TRUE(context.snapshot.fsr_valid != 0U);
    SensorLogic_MarkFsrError(&context, SENSOR_LOGIC_ERROR_FSR_ADC, SENSOR_FSR_ADC_TIMEOUT_MS + 2U);
    CHECK_TRUE(context.snapshot.fsr_valid == 0U);
}

void TestRouteTestSafetyFiltering() {
    constexpr auto kFrontError = static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT);
    constexpr auto kRearError = static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_ULTRASONIC_REAR);
    constexpr auto kLeftError = static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT);
    constexpr auto kRightError = static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT);
    constexpr auto kFsrError = static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_FSR_ADC);
    constexpr auto kFrontObstacle = static_cast<std::uint8_t>(SENSOR_LOGIC_DIRECTION_FRONT);
    constexpr auto kRearObstacle = static_cast<std::uint8_t>(SENSOR_LOGIC_DIRECTION_REAR);
    constexpr auto kLeftObstacle = static_cast<std::uint8_t>(SENSOR_LOGIC_DIRECTION_LEFT);
    constexpr auto kRightObstacle = static_cast<std::uint8_t>(SENSOR_LOGIC_DIRECTION_RIGHT);
    constexpr auto kAllUltrasonicErrors = kFrontError | kRearError | kLeftError | kRightError;
    constexpr auto kAllUltrasonicObstacles =
        static_cast<std::uint8_t>(kFrontObstacle | kRearObstacle | kLeftObstacle | kRightObstacle);
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    for (std::uint32_t sample = 1U; sample <= SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES; ++sample) {
        SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, sample * 10U, &update);
    }
    CHECK_TRUE((context.diagnostics.error_flags & kFrontError) != 0U);

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 1U, 0U);
    for (std::uint32_t sample = 1U; sample <= SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES; ++sample) {
        SensorLogic_UpdateUltrasonic(&context, 1U, 0U, 0U, sample * 10U, &update);
    }
#if SENSOR_ULTRASONIC_REAR_ENABLED
    CHECK_TRUE((context.diagnostics.error_flags & kRearError) != 0U);
#else
    CHECK_TRUE((context.diagnostics.error_flags & kRearError) == 0U);
#endif

    SensorLogic_Init(&context, 0U);
    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U, 10U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U, 20U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, SENSOR_OBSTACLE_ON_MM, 1U, 30U, &update);
    CHECK_TRUE((context.diagnostics.obstacle_mask & kFrontObstacle) != 0U);

#if LINETRACER_ROUTE_TEST_MODE
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyErrorFlags(kAllUltrasonicErrors) == 0U);
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyErrorFlags(kAllUltrasonicErrors | kFsrError) ==
               ((kAllUltrasonicErrors | kFsrError) & ~SENSOR_ROUTE_TEST_IGNORED_ERROR_FLAGS));
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyObstacleMask(kAllUltrasonicObstacles) == 0U);
#else
    constexpr auto kEnabledUltrasonicObstacles =
        static_cast<std::uint8_t>(kFrontObstacle | kLeftObstacle | kRightObstacle);
#if SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT
    constexpr auto kEnabledSafetyErrors = kFrontError | kLeftError | kRightError;
#else
    constexpr std::uint32_t kEnabledSafetyErrors = 0U;
#endif
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyErrorFlags(kAllUltrasonicErrors) == kEnabledSafetyErrors);
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyErrorFlags(kAllUltrasonicErrors | kFsrError) ==
               (kEnabledSafetyErrors | kFsrError));
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyObstacleMask(kAllUltrasonicObstacles) == kEnabledUltrasonicObstacles);
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyErrorFlags(kRearError | kLeftError | kRightError) ==
               (kEnabledSafetyErrors & (kLeftError | kRightError)));
    CHECK_TRUE(SensorLogic_GetEffectiveSafetyObstacleMask(kRearObstacle) == 0U);
#endif
}

}  // namespace

int main() {
    TestMarkerMessageContract();
    TestLineNormalizationAndDebounce();
    TestOuterSensorsDetectMarkerWhenCenterMisses();
    TestAnalogLineSamples();
    TestAnalogBlackHysteresis();
    TestCenterDigitalInputOwnsBlackState();
    TestThreeSensorTrackingState();
    TestAnalogLineErrorDeadband();
    TestDigitalLineStateOwnsPidDirection();
    TestMarkerGroupClassification();
    TestInvalidMarkerWidthAndNoise();
    TestFsrStabilityAndHysteresis();
    TestOverloadAndObstacleHysteresis();
    TestUltrasonicSuspendClearsObstacleAndStaleness();
    TestPendingEventLatch();
    TestSensorErrorsAndStaleness();
    TestRouteTestSafetyFiltering();

    if (g_failures != 0) {
        std::cerr << g_failures << " sensor logic test(s) failed\n";
        return 1;
    }

    std::cout << "All sensor logic tests passed\n";
    return 0;
}
