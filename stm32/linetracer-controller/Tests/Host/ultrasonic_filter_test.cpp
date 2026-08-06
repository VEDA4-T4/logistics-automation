#include <cassert>
#include <cstdint>

#include "sensor_logic.h"

namespace {

void TestObstacleHysteresis() {
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    for (std::uint32_t sample = 1U; sample < SENSOR_OBSTACLE_ACTIVATE_SAMPLES; ++sample) {
        update = {};
        SensorLogic_UpdateUltrasonic(&context, 0U, 20U, 1U, sample * 60U, &update);
        assert((context.diagnostics.obstacle_mask & SENSOR_LOGIC_DIRECTION_FRONT) == 0U);
        assert((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) == 0U);
    }

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 20U, 1U, SENSOR_OBSTACLE_ACTIVATE_SAMPLES * 60U, &update);
    assert((context.diagnostics.obstacle_mask & SENSOR_LOGIC_DIRECTION_FRONT) != 0U);
    assert((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 20U, 1U, (SENSOR_OBSTACLE_ACTIVATE_SAMPLES * 60U) + 10U, &update);
    assert((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) == 0U);
    assert((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) == 0U);

    for (std::uint32_t sample = 1U; sample < SENSOR_OBSTACLE_CLEAR_SAMPLES; ++sample) {
        update = {};
        SensorLogic_UpdateUltrasonic(&context, 0U, 100U, 1U, 200U + (sample * 60U), &update);
        assert((context.diagnostics.obstacle_mask & SENSOR_LOGIC_DIRECTION_FRONT) != 0U);
        assert((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) == 0U);
    }

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 100U, 1U, 200U + (SENSOR_OBSTACLE_CLEAR_SAMPLES * 60U), &update);
    assert((context.diagnostics.obstacle_mask & SENSOR_LOGIC_DIRECTION_FRONT) == 0U);
    assert((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) != 0U);

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 100U, 1U, 210U + (SENSOR_OBSTACLE_CLEAR_SAMPLES * 60U), &update);
    assert((update.event_flags & APP_SENSOR_EVENT_OBSTACLE) == 0U);
    assert((update.safety_cleared_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) == 0U);
}

void TestAllDirectionsParticipateInSafetyPolicy() {
    constexpr auto kAllObstacles =
        static_cast<std::uint8_t>(SENSOR_LOGIC_DIRECTION_FRONT | SENSOR_LOGIC_DIRECTION_REAR |
                                  SENSOR_LOGIC_DIRECTION_LEFT | SENSOR_LOGIC_DIRECTION_RIGHT);
    constexpr auto kAllErrors =
        static_cast<std::uint32_t>(SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_REAR |
                                   SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT);

    assert(SensorLogic_GetEffectiveSafetyObstacleMask(kAllObstacles) == kAllObstacles);
    assert(SensorLogic_GetEffectiveSafetyObstacleMask(SENSOR_LOGIC_DIRECTION_REAR) == SENSOR_LOGIC_DIRECTION_REAR);
    assert(SensorLogic_GetEffectiveSafetyErrorFlags(kAllErrors) == kAllErrors);
}

void TestEachDirectionStopsAndInvalidReadingDoesNot() {
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    for (std::uint8_t index = 0U; index < SENSOR_LOGIC_ULTRASONIC_COUNT; ++index) {
        const auto direction = static_cast<std::uint8_t>(1U << index);

        update = {};
        SensorLogic_UpdateUltrasonic(&context, index, SENSOR_ULTRASONIC_STOP_DISTANCE_MM, 1U, 10U + index, &update);
        assert((context.diagnostics.obstacle_mask & direction) != 0U);

        update = {};
        SensorLogic_UpdateUltrasonic(&context, index, SENSOR_ULTRASONIC_CLEAR_DISTANCE_MM, 1U, 20U + index, &update);
        assert((context.diagnostics.obstacle_mask & direction) == 0U);
    }

    update = {};
    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, 100U, &update);
    assert(context.diagnostics.obstacle_mask == 0U);
    assert((update.safety_activated_flags & SENSOR_LOGIC_SAFETY_OBSTACLE) == 0U);
}

void TestDisabledMonitoringDoesNotCreateStaleFault() {
    sensor_logic_context_t context{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_SetUltrasonicMonitoringEnabled(&context, 0U, 0U);
    SensorLogic_CheckStaleness(&context, SENSOR_ULTRASONIC_STALE_MS + 1U);
    assert((context.diagnostics.error_flags &
            (SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_REAR |
             SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT)) == 0U);
}

void TestUltrasonicFailureDebounce() {
    sensor_logic_context_t context{};
    sensor_logic_update_t update{};

    SensorLogic_Init(&context, 0U);
    SensorLogic_MarkUltrasonicStarted(&context, 0U, 0U);
    for (std::uint32_t sample = 1U; sample < SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES; ++sample) {
        SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, sample * 60U, &update);
        assert((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) == 0U);
    }

    SensorLogic_UpdateUltrasonic(&context, 0U, 0U, 0U, SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES * 60U, &update);
    assert((context.diagnostics.error_flags & SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT) != 0U);
}

}  // namespace

int main() {
    TestObstacleHysteresis();
    TestAllDirectionsParticipateInSafetyPolicy();
    TestEachDirectionStopsAndInvalidReadingDoesNot();
    TestDisabledMonitoringDoesNotCreateStaleFault();
    TestUltrasonicFailureDebounce();
    return 0;
}
