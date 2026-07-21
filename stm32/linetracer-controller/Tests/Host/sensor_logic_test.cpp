#include <cstdint>
#include <iostream>

extern "C" {
#include "sensor_logic.h"
}

namespace {

int g_failures = 0;

#define CHECK_TRUE(condition)                                                        \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::cerr << __func__ << ":" << __LINE__ << " check failed: "          \
                      << #condition << '\n';                                         \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

sensor_logic_update_t UpdateLine(sensor_logic_context_t &context,
                                 std::uint8_t left,
                                 std::uint8_t right,
                                 std::uint32_t now_ms)
{
    sensor_logic_update_t update{};
    SensorLogic_UpdateLine(&context, left, right, now_ms, &update);
    return update;
}

sensor_logic_update_t UpdateFsr(sensor_logic_context_t &context,
                                std::uint16_t raw,
                                std::uint32_t now_ms)
{
    sensor_logic_update_t update{};
    SensorLogic_UpdateFsr(&context, raw, now_ms, &update);
    return update;
}

void TestLineNormalizationAndDebounce()
{
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

void TestMarkerIsOneShot()
{
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    (void)UpdateLine(context, 1U, 1U, 0U);
    (void)UpdateLine(context, 1U, 1U, 10U);
    (void)UpdateLine(context, 1U, 1U, 20U);

    (void)UpdateLine(context, 0U, 0U, 30U);
    (void)UpdateLine(context, 0U, 0U, 40U);
    (void)UpdateLine(context, 0U, 0U, 50U);
    (void)UpdateLine(context, 0U, 0U, 60U);
    (void)UpdateLine(context, 1U, 1U, 70U);
    (void)UpdateLine(context, 1U, 1U, 80U);
    auto update = UpdateLine(context, 1U, 1U, 90U);

    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
    CHECK_TRUE(context.diagnostics.marker_sequence == 1U);
    CHECK_TRUE((UpdateLine(context, 1U, 1U, 100U).event_flags &
                APP_SENSOR_EVENT_MARKER) == 0U);
    CHECK_TRUE((UpdateLine(context, 1U, 1U, 150U).event_flags &
                APP_SENSOR_EVENT_MARKER) == 0U);
}

void TestInvalidMarkerEntryAndLineLost()
{
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
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) == 0U);

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
    CHECK_TRUE((UpdateLine(context, 0U, 0U, 560U).event_flags &
                APP_SENSOR_EVENT_LINE_LOST) == 0U);

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
    CHECK_TRUE((update.event_flags & APP_SENSOR_EVENT_MARKER) != 0U);
}

void TestFsrStabilityAndHysteresis()
{
    sensor_logic_context_t context{};
    std::uint32_t load_on_count = 0U;
    std::uint32_t load_off_count = 0U;

    SensorLogic_Init(&context, 0U);
    for (std::uint32_t now = 0U; now <= 500U; now += 10U) {
        auto update = UpdateFsr(context,
                                ((now / 10U) % 2U == 0U) ? 2000U : 2300U,
                                now);
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

void TestOverloadAndObstacleHysteresis()
{
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

void TestPendingEventLatch()
{
    sensor_event_latch_t latch{};

    SensorEventLatch_Init(&latch);
    CHECK_TRUE(SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_MARKER) ==
               APP_SENSOR_EVENT_MARKER);
    CHECK_TRUE(SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_NONE) ==
               APP_SENSOR_EVENT_MARKER);

    (void)SensorEventLatch_Pend(&latch, APP_SENSOR_EVENT_LOAD_ON);
    SensorEventLatch_Acknowledge(&latch, APP_SENSOR_EVENT_MARKER);
    CHECK_TRUE(latch.pending_flags == APP_SENSOR_EVENT_LOAD_ON);
    SensorEventLatch_Acknowledge(&latch, APP_SENSOR_EVENT_LOAD_ON);
    CHECK_TRUE(latch.pending_flags == APP_SENSOR_EVENT_NONE);
}

void TestSensorErrorsAndStaleness()
{
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    SensorLogic_CheckStaleness(&context, SENSOR_ULTRASONIC_STALE_MS - 1U);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    SensorLogic_CheckStaleness(&context, SENSOR_ULTRASONIC_STALE_MS);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);

    SensorLogic_UpdateUltrasonic(&context, 0U, 500U, 1U,
                                 SENSOR_ULTRASONIC_STALE_MS + 10U, &update);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 800U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 810U, &update);
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 820U, &update);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);

    SensorLogic_Init(&context, 0U);
    SensorLogic_CheckStaleness(&context, SENSOR_FSR_ADC_TIMEOUT_MS);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_FSR_TIMEOUT) != 0U);
    update = {};
    SensorLogic_UpdateFsr(&context, 1000U,
                          SENSOR_FSR_ADC_TIMEOUT_MS + 1U, &update);
    CHECK_TRUE((context.diagnostics.error_flags &
                SENSOR_LOGIC_ERROR_FSR_TIMEOUT) == 0U);
}

}  // namespace

int main()
{
    TestLineNormalizationAndDebounce();
    TestMarkerIsOneShot();
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
