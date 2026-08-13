#ifdef NDEBUG
#undef NDEBUG
#endif

#include "logistics/central_server/sensor_detection.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

using logistics::central_server::SensorDetectionConfig;
using logistics::central_server::SensorDetector;

constexpr const char* kDevice = "PI-SORTING-01";

[[nodiscard]] SensorDetectionConfig MakeConfig(int debounce = 3) {
    return SensorDetectionConfig{
        .enabled = true,
        .enter_threshold_cm = 10,
        .exit_threshold_cm = 12,
        .debounce_count = debounce,
    };
}

std::string Evaluate(SensorDetector& detector, int distance_cm, std::int32_t sensor_id = 1) {
    const auto result = detector.Evaluate(kDevice, sensor_id, "OK", distance_cm);
    assert(result.has_value());
    return *result;
}

void TestDisabledDetectionStampsNothing() {
    SensorDetector detector(SensorDetectionConfig{ .enabled = false });
    assert(!detector.Evaluate(kDevice, 1, "OK", 5).has_value());
}

void TestStartsClearAndNeedsDebounceToDetect() {
    SensorDetector detector(MakeConfig());

    // A single close reading is not enough - the debounce is what stops a lone
    // noisy sample from announcing a box that is not there.
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "DETECTED");
}

void TestHysteresisBandHoldsDetected() {
    SensorDetector detector(MakeConfig());
    for (int i = 0; i < 3; ++i) {
        Evaluate(detector, 5);
    }

    // 11cm is past the enter threshold (10) but still inside the exit
    // threshold (12), so a box parked on the line must stay DETECTED.
    for (int i = 0; i < 5; ++i) {
        assert(Evaluate(detector, 11) == "DETECTED");
    }

    // 13cm clears the exit threshold, and takes the full debounce to land.
    assert(Evaluate(detector, 13) == "DETECTED");
    assert(Evaluate(detector, 13) == "DETECTED");
    assert(Evaluate(detector, 13) == "CLEAR");
}

void TestInterruptedRunRestartsDebounce() {
    SensorDetector detector(MakeConfig());

    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 40) == "CLEAR");  // breaks the run
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "DETECTED");
}

void TestFaultYieldsUnknownAndResetsChannel() {
    SensorDetector detector(MakeConfig());
    for (int i = 0; i < 3; ++i) {
        Evaluate(detector, 5);
    }

    const auto faulted = detector.Evaluate(kDevice, 1, "FAULT", 0xFFFF);
    assert(faulted.has_value() && *faulted == "UNKNOWN");

    // The stale DETECTED must not survive the outage: detection restarts from
    // CLEAR and has to earn DETECTED again.
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "CLEAR");
    assert(Evaluate(detector, 5) == "DETECTED");
}

void TestUnknownDistanceIsTreatedAsUnmeasurable() {
    SensorDetector detector(MakeConfig());
    const auto result = detector.Evaluate(kDevice, 1, "OK", SensorDetector::kDistanceUnknown);
    assert(result.has_value() && *result == "UNKNOWN");
}

void TestChannelsAreIndependentPerDeviceAndSensor() {
    SensorDetector detector(MakeConfig(1));

    assert(Evaluate(detector, 5, 1) == "DETECTED");
    // Same sensor number on the same device but a different id stays separate...
    assert(Evaluate(detector, 40, 2) == "CLEAR");
    assert(Evaluate(detector, 5, 1) == "DETECTED");

    // ...and so does sensor 1 on another device, which would otherwise collide
    // since both nodes number their sensors from 1.
    const auto other = detector.Evaluate("PI-INPUT-01", 1, "OK", 40);
    assert(other.has_value() && *other == "CLEAR");
    assert(Evaluate(detector, 5, 1) == "DETECTED");
}

void TestThresholdsComeFromConfig() {
    // Retuning is a config edit, not a firmware change: a 30cm enter threshold
    // must make a 25cm reading DETECTED.
    SensorDetector detector(SensorDetectionConfig{
        .enabled = true,
        .enter_threshold_cm = 30,
        .exit_threshold_cm = 35,
        .debounce_count = 1,
    });

    assert(Evaluate(detector, 25) == "DETECTED");
    assert(Evaluate(detector, 33) == "DETECTED");  // inside the hysteresis band
    assert(Evaluate(detector, 40) == "CLEAR");
}

void TestConfigValidation() {
    assert(MakeConfig().IsValid());
    assert(!(SensorDetectionConfig{ .enabled = true, .enter_threshold_cm = 0 }).IsValid());
    // exit below enter would invert the hysteresis band.
    assert(!(SensorDetectionConfig{ .enabled = true, .enter_threshold_cm = 12, .exit_threshold_cm = 10 }).IsValid());
    assert(!(SensorDetectionConfig{ .enabled = true, .debounce_count = 0 }).IsValid());
    // enter == exit is degenerate but legal: no hysteresis, plain threshold.
    assert((SensorDetectionConfig{ .enabled = true, .enter_threshold_cm = 10, .exit_threshold_cm = 10 }).IsValid());
}

}  // namespace

int main() {
    TestDisabledDetectionStampsNothing();
    TestStartsClearAndNeedsDebounceToDetect();
    TestHysteresisBandHoldsDetected();
    TestInterruptedRunRestartsDebounce();
    TestFaultYieldsUnknownAndResetsChannel();
    TestUnknownDistanceIsTreatedAsUnmeasurable();
    TestChannelsAreIndependentPerDeviceAndSensor();
    TestThresholdsComeFromConfig();
    TestConfigValidation();
    std::cout << "sensor_detection_test passed\n";
    return 0;
}
