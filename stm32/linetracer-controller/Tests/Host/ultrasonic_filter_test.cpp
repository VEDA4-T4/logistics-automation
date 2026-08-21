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
}

void TestAllDirectionSafetyPolicy() {
    constexpr auto kAllObstacles = static_cast<std::uint8_t>(
        SENSOR_LOGIC_DIRECTION_FRONT | SENSOR_LOGIC_DIRECTION_LEFT | SENSOR_LOGIC_DIRECTION_RIGHT);
    constexpr auto kAllUltrasonicErrors = static_cast<std::uint32_t>(
        SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT);
    constexpr auto kEnabledObstacles = static_cast<std::uint8_t>(
        SENSOR_LOGIC_DIRECTION_FRONT | SENSOR_LOGIC_DIRECTION_LEFT | SENSOR_LOGIC_DIRECTION_RIGHT);

    assert(SensorLogic_GetEffectiveSafetyObstacleMask(kAllObstacles) == kEnabledObstacles);
    assert((SensorLogic_GetEffectiveSafetyErrorFlags(kAllUltrasonicErrors) & kAllUltrasonicErrors) == 0U);
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
    TestAllDirectionSafetyPolicy();
    TestUltrasonicFailureDebounce();
    return 0;
}
