#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logistics/central_server/process_state_machine.hpp"

namespace logistics::contracts::mqtt {
struct MqttMessage;
}

namespace logistics::central_server {

// ---------------------------------------------------------------------------
// Ultrasonic box-presence detection
// ---------------------------------------------------------------------------
//
// The STM32 used to decide DETECTED/CLEAR itself, with the thresholds baked
// into sensor_filter.h as #defines - retuning them meant editing firmware and
// reflashing the board. The controller now reports only the measured distance
// and whether the reading is trustworthy, and this judgement lives here so the
// thresholds are ordinary server configuration ([sensor_detection] in
// server.ini).
//
// The hysteresis + debounce behaviour is a deliberate port of the firmware
// filter, not a simplification of it: a single threshold makes DETECTED/CLEAR
// chatter whenever a box parks near the limit, which is exactly what the two
// separate enter/exit thresholds were introduced to stop.

struct SensorDetectionConfig final {
    bool enabled{ true };

    // A reading at or below enter_threshold_cm starts a box; the box is only
    // released once the reading climbs above exit_threshold_cm. The gap
    // between the two is the hysteresis band.
    int enter_threshold_cm{ 10 };
    int exit_threshold_cm{ 12 };

    // How many consecutive readings must agree before the state flips.
    int debounce_count{ 3 };

    [[nodiscard]] bool IsValid() const noexcept {
        return enter_threshold_cm > 0 && exit_threshold_cm >= enter_threshold_cm && debounce_count >= 1;
    }
};

// Detection status strings, matching IsValidDetectionStatus in mqtt_codec.hpp.
inline constexpr std::string_view kDetectionClear = "CLEAR";
inline constexpr std::string_view kDetectionDetected = "DETECTED";
inline constexpr std::string_view kDetectionUnknown = "UNKNOWN";

class SensorDetector final {
public:
    explicit SensorDetector(SensorDetectionConfig config) noexcept;

    // Returns the detection status for one SENSOR_STATUS reading, or nullopt
    // when detection is disabled (the field is then left off the message so
    // consumers can tell "not configured" from "cannot tell").
    //
    // measurement_status is the device-reported health ("OK"/"FAULT").  A
    // faulty sensor, or a distance of kDistanceUnknown, yields UNKNOWN and
    // resets that channel's debounce - a stale DETECTED must not survive the
    // sensor going blind.
    [[nodiscard]] std::optional<std::string> Evaluate(std::string_view device_id, std::int32_t sensor_id,
                                                      std::string_view measurement_status, std::int32_t distance_cm);

    [[nodiscard]] const SensorDetectionConfig& Config() const noexcept {
        return config_;
    }

    void Reset() noexcept;

    // Mirrors UART_SENSOR_DISTANCE_UNKNOWN: the controller could not measure.
    static constexpr std::int32_t kDistanceUnknown = 0xFFFF;

private:
    struct ChannelState final {
        bool detected{ false };
        bool candidate{ false };
        int matches{ 0 };
        bool primed{ false };
    };

    SensorDetectionConfig config_;
    std::map<std::string, ChannelState> channels_;
};

class InputDetectionGate final {
public:
    explicit InputDetectionGate(std::string input_device_id, std::int32_t sensor_id = 1);

    // Physical stopping is independent from whether the process can create a
    // work item. This closes the partial-START case where the conveyor is
    // moving but the server already returned to STOPPED after another node
    // timed out.
    [[nodiscard]] bool ShouldStopConveyor(const contracts::mqtt::MqttMessage& message);

    // Consumes one physical DETECTED interval. A stopped process defers the
    // interval so the next reading after START can create the work; an already
    // occupied input station consumes it to prevent a second work for the same
    // box. A prior CLEAR is not required because CLEAR telemetry can be missed
    // while the process starts.
    [[nodiscard]] bool ShouldCreateWork(const contracts::mqtt::MqttMessage& message, bool process_accepts_work,
                                        bool input_station_occupied);
    void RequireClear() noexcept;
    void Retry() noexcept;
    void RetryStop() noexcept;

private:
    std::string input_device_id_;
    std::int32_t sensor_id_;
    bool consumed_{ false };
    bool stop_consumed_{ false };
};

class SortingDetectionGate final {
public:
    explicit SortingDetectionGate(std::string sorting_device_id);

    // Matches sensor N to destination N and consumes one DETECTED interval.
    // The interval is deferred while the process is stopped or no matching
    // sorting work exists, so START can resume the same physical box.
    [[nodiscard]] std::optional<std::string> ShouldStop(const contracts::mqtt::MqttMessage& message,
                                                        bool process_running,
                                                        const std::vector<WorkProcessSnapshot>& active_works);
    void Retry() noexcept;

private:
    std::string sorting_device_id_;
    std::optional<std::int32_t> consumed_sensor_id_;
};

}  // namespace logistics::central_server
