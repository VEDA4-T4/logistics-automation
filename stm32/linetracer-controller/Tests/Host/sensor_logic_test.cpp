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
    SensorLogic_UpdateLine(&context, left, right, now_ms, &update);
    return update;
}

sensor_logic_update_t UpdateFsr(sensor_logic_context_t& context, std::uint16_t raw, std::uint32_t now_ms) {
    sensor_logic_update_t update{};
    SensorLogic_UpdateFsr(&context, raw, now_ms, &update);
    return update;
}

void InitializeCentered(sensor_logic_context_t& context, std::uint32_t start_ms) {
    (void)UpdateLine(context, 1U, 1U, start_ms);
    (void)UpdateLine(context, 1U, 1U, start_ms + 10U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 20U);
}

std::uint32_t EmitMarkerStripe(sensor_logic_context_t& context, std::uint32_t start_ms) {
    (void)UpdateLine(context, 0U, 0U, start_ms);
    (void)UpdateLine(context, 0U, 0U, start_ms + 10U);
    (void)UpdateLine(context, 0U, 0U, start_ms + 20U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 60U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 70U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 80U);
    (void)UpdateLine(context, 1U, 1U, start_ms + 130U);
    return start_ms + 80U;
}

sensor_logic_update_t EmitMarkerGroup(sensor_logic_context_t& context, std::uint8_t stripe_count,
                                      std::uint32_t first_stripe_at_ms, std::uint32_t& last_stripe_at_ms) {
    auto next_stripe_at_ms = first_stripe_at_ms;

    for (std::uint8_t index = 0U; index < stripe_count; ++index) {
        last_stripe_at_ms = EmitMarkerStripe(context, next_stripe_at_ms);
        next_stripe_at_ms = last_stripe_at_ms + 60U;
    }

    auto update = UpdateLine(context, 1U, 1U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS - 1U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    return UpdateLine(context, 1U, 1U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS);
}

void TestMarkerMessageContract() {
    app_sensor_snapshot_t snapshot{};
    sensor_logic_context_t context{};

    CHECK_TRUE(snapshot.marker_code == APP_MARKER_NONE);
    CHECK_TRUE(snapshot.marker_count == 0U);
    CHECK_TRUE(snapshot.marker_detected_at_ms == 0U);

    SensorLogic_Init(&context, 10U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_NONE);
    CHECK_TRUE(context.snapshot.marker_count == 0U);
    CHECK_TRUE(context.snapshot.marker_detected_at_ms == 0U);

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
    CHECK_TRUE(UpdateLine(context, 1U, 1U, 0U).event_flags == 0U);
    CHECK_TRUE(UpdateLine(context, 1U, 1U, 10U).event_flags == 0U);
    auto update = UpdateLine(context, 1U, 1U, 20U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_CENTERED);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);

    CHECK_TRUE(UpdateLine(context, 1U, 0U, 30U).event_flags == 0U);
    CHECK_TRUE(UpdateLine(context, 1U, 1U, 40U).event_flags == 0U);
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

    (void)UpdateLine(context, 0U, 0U, 120U);
    (void)UpdateLine(context, 0U, 0U, 130U);
    update = UpdateLine(context, 0U, 0U, 140U);
    CHECK_TRUE(context.snapshot.line_state == LINETRACER_LINE_WHITE_GAP);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_CHANGED) != 0U);
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
        CHECK_TRUE((UpdateLine(context, 1U, 1U, last_stripe_at_ms + SENSOR_MARKER_GROUP_TIMEOUT_MS + 10U).event_flags &
                    APP_SENSOR_EVENT_MARKER) == 0U);
    }
}

void TestInvalidMarkerEntryAndLineLost() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    (void)UpdateLine(context, 1U, 1U, 0U);
    (void)UpdateLine(context, 1U, 1U, 10U);
    (void)UpdateLine(context, 1U, 1U, 20U);
    (void)UpdateLine(context, 1U, 0U, 30U);
    (void)UpdateLine(context, 1U, 0U, 40U);
    (void)UpdateLine(context, 1U, 0U, 50U);
    (void)UpdateLine(context, 0U, 0U, 60U);
    (void)UpdateLine(context, 0U, 0U, 70U);
    (void)UpdateLine(context, 0U, 0U, 80U);
    (void)UpdateLine(context, 1U, 1U, 100U);
    (void)UpdateLine(context, 1U, 1U, 110U);
    auto update = UpdateLine(context, 1U, 1U, 120U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_INVALID);
    CHECK_TRUE(context.snapshot.marker_count == 1U);
    CHECK_TRUE(context.latest_marker_event.type == SENSOR_MARKER_EVENT_INVALID_TRANSITION);

    SensorLogic_Init(&context, 0U);
    InitializeCentered(context, 0U);
    (void)UpdateLine(context, 0U, 0U, 30U);
    (void)UpdateLine(context, 0U, 0U, 40U);
    (void)UpdateLine(context, 0U, 0U, 50U);
    (void)UpdateLine(context, 1U, 1U, 370U);
    (void)UpdateLine(context, 1U, 1U, 380U);
    update = UpdateLine(context, 1U, 1U, 390U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_INVALID);
    CHECK_TRUE(context.latest_marker_event.type == SENSOR_MARKER_EVENT_INVALID_WIDTH);

    SensorLogic_Init(&context, 0U);
    (void)UpdateLine(context, 1U, 1U, 0U);
    (void)UpdateLine(context, 1U, 1U, 10U);
    (void)UpdateLine(context, 1U, 1U, 20U);
    (void)UpdateLine(context, 0U, 0U, 30U);
    (void)UpdateLine(context, 0U, 0U, 40U);
    (void)UpdateLine(context, 0U, 0U, 50U);

    update = UpdateLine(context, 0U, 0U, 550U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LINE_LOST) != 0U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    CHECK_TRUE((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_LINE_LOST) != 0U);
    CHECK_TRUE((UpdateLine(context, 0U, 0U, 560U).event_flags & APP_SENSOR_EVENT_LINE_LOST) == 0U);

    (void)UpdateLine(context, 1U, 1U, 570U);
    (void)UpdateLine(context, 1U, 1U, 580U);
    update = UpdateLine(context, 1U, 1U, 590U);
    CHECK_TRUE((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_LINE_LOST) != 0U);

    (void)UpdateLine(context, 1U, 1U, 600U);
    (void)UpdateLine(context, 1U, 1U, 650U);
    (void)UpdateLine(context, 0U, 0U, 660U);
    (void)UpdateLine(context, 0U, 0U, 670U);
    (void)UpdateLine(context, 0U, 0U, 680U);
    (void)UpdateLine(context, 1U, 1U, 700U);
    (void)UpdateLine(context, 1U, 1U, 710U);
    update = UpdateLine(context, 1U, 1U, 720U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    update = UpdateLine(context, 1U, 1U, 969U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);
    update = UpdateLine(context, 1U, 1U, 970U);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.snapshot.marker_code == APP_MARKER_JUNCTION);
}

void TestFsrStabilityAndHysteresis() {
    sensor_logic_context_t context{};
    std::uint32_t load_on_count = 0U;
    std::uint32_t load_off_count = 0U;

    SensorLogic_Init(&context, 0U);
    for (std::uint32_t now = 0U; now <= 500U; now += 10U) {
        auto update = UpdateFsr(context, ((now / 10U) % 2U == 0U) ? 2000U : 2300U, now);
        CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_LOAD_ON) == 0U);
    }
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_EMPTY);

    SensorLogic_Init(&context, 0U);
    for (std::uint32_t now = 0U; now <= 350U; now += 10U) {
        auto update = UpdateFsr(context, 3000U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_ON) != 0U) {
            ++load_on_count;
        }
    }
    CHECK_TRUE(load_on_count == 1U);
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    for (std::uint32_t now = 360U; now <= 550U; now += 10U) {
        auto update = UpdateFsr(context, 0U, now);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) != 0U) {
            ++load_off_count;
        }
    }
    CHECK_TRUE(load_off_count == 0U);
    CHECK_TRUE(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    for (std::uint32_t now = 560U; now <= 1100U; now += 10U) {
        auto update = UpdateFsr(context, 0U, now);
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
    SensorLogic_UpdateUltrasonic(&context, 0U, 100U, 1U, 400U, &update);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) != 0U);
    CHECK_TRUE((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 180U, 1U, 410U, &update);
    CHECK_TRUE(update.event_flags == 0U);
    CHECK_TRUE(context.diagnostics.obstacle_mask != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 230U, 1U, 420U, &update);
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) != 0U);
    CHECK_TRUE((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);
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
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 20U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 30U, &update);
    CHECK_TRUE((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 40U, &update);
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

}  // namespace

int main() {
    TestMarkerMessageContract();
    TestLineNormalizationAndDebounce();
    TestMarkerGroupClassification();
    TestInvalidMarkerEntryAndLineLost();
    TestFsrStabilityAndHysteresis();
    TestOverloadAndObstacleHysteresis();
    TestPendingEventLatch();
    TestSensorErrorsAndStaleness();

    if (g_failures != 0) {
        std::cerr << g_failures << " sensor logic test(s) failed\n";
        return 1;
    }

    std::cout << "All sensor logic tests passed\n";
    return 0;
}
