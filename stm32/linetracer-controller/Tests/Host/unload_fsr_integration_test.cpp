#include <cassert>
#include <cstdint>

extern "C" {
#include "sensor_logic.h"
}

namespace {

sensor_logic_update_t UpdateFsr(sensor_logic_context_t& context, std::uint16_t raw, std::uint32_t now_ms) {
    sensor_logic_update_t update{};
    SensorLogic_UpdateFsr(&context, raw, now_ms, &update);
    return update;
}

std::uint32_t CaptureBaseline(sensor_logic_context_t& context, sensor_fsr_baseline_mode_t mode, std::uint16_t raw,
                              std::uint32_t started_at_ms) {
    std::uint32_t ready_count = 0U;

    SensorLogic_StartFsrBaselineCapture(&context, mode);
    for (std::uint32_t index = 0U; index < SENSOR_FSR_BASELINE_SAMPLES; ++index) {
        const auto update = UpdateFsr(context, raw, started_at_ms + (index * 10U));
        if ((update.event_flags & APP_SENSOR_EVENT_FSR_BASELINE_READY) != 0U) {
            ++ready_count;
        }
    }

    assert(ready_count == 1U);
    assert(context.fsr_baseline_valid != 0U);
    assert(context.snapshot.fsr_valid != 0U);
    return started_at_ms + (SENSOR_FSR_BASELINE_SAMPLES * 10U);
}

void TestAdaptiveLoadRemovalRequiresStableDelta() {
    sensor_logic_context_t context{};
    std::uint32_t now_ms;
    std::uint32_t load_on_events = 0U;
    std::uint32_t load_off_events = 0U;

    SensorLogic_Init(&context, 0U);
    now_ms = CaptureBaseline(context, SENSOR_FSR_BASELINE_FOR_LOAD_ON, 1700U, 0U);

    for (; now_ms <= 900U; now_ms += 10U) {
        const auto update = UpdateFsr(context, 2000U, now_ms);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_ON) != 0U) {
            ++load_on_events;
        }
    }
    assert(load_on_events == 1U);
    assert(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    now_ms = CaptureBaseline(context, SENSOR_FSR_BASELINE_FOR_LOAD_OFF, 2000U, now_ms);

    const auto unstable_until_ms = now_ms + APP_TIMING_FSR_STABLE_MS - 10U;
    for (; now_ms <= unstable_until_ms; now_ms += 10U) {
        const auto update = UpdateFsr(context, 1700U, now_ms);
        assert((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) == 0U);
        assert(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);
    }

    /* Allow the moving-average filter to settle before the 300 ms state timer can expire. */
    for (; now_ms <= unstable_until_ms + 200U; now_ms += 10U) {
        const auto update = UpdateFsr(context, 1700U, now_ms);
        if ((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) != 0U) {
            ++load_off_events;
        }
    }
    assert(load_off_events == 1U);
    assert(context.snapshot.load_state == UART_LINETRACER_LOAD_EMPTY);
}

void TestDestinationNoiseDoesNotClearLoad() {
    sensor_logic_context_t context{};
    std::uint32_t now_ms;

    SensorLogic_Init(&context, 0U);
    now_ms = CaptureBaseline(context, SENSOR_FSR_BASELINE_FOR_LOAD_ON, 1700U, 0U);
    for (; now_ms <= 900U; now_ms += 10U) {
        (void)UpdateFsr(context, 2000U, now_ms);
    }
    assert(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);

    now_ms = CaptureBaseline(context, SENSOR_FSR_BASELINE_FOR_LOAD_OFF, 2000U, now_ms);
    for (std::uint32_t index = 0U; index < 80U; ++index, now_ms += 10U) {
        const auto noisy_raw = static_cast<std::uint16_t>(((index % 2U) == 0U) ? 1800U : 2000U);
        const auto update = UpdateFsr(context, noisy_raw, now_ms);
        assert((update.event_flags & APP_SENSOR_EVENT_LOAD_OFF) == 0U);
    }
    assert(context.snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);
}

}  // namespace

int main() {
    TestAdaptiveLoadRemovalRequiresStableDelta();
    TestDestinationNoiseDoesNotClearLoad();
    return 0;
}
