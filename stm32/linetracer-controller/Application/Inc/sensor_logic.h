#ifndef SENSOR_LOGIC_H
#define SENSOR_LOGIC_H

#include <stdint.h>

#include "app_messages.h"
#include "sensor_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_LOGIC_ULTRASONIC_COUNT 4U

typedef enum {
    SENSOR_LOGIC_VALID_NONE = 0U,
    SENSOR_LOGIC_VALID_LINE = (1U << 0U),
    SENSOR_LOGIC_VALID_FSR = (1U << 1U),
    SENSOR_LOGIC_VALID_ULTRASONIC_FRONT = (1U << 2U),
    SENSOR_LOGIC_VALID_ULTRASONIC_REAR = (1U << 3U),
    SENSOR_LOGIC_VALID_ULTRASONIC_LEFT = (1U << 4U),
    SENSOR_LOGIC_VALID_ULTRASONIC_RIGHT = (1U << 5U)
} sensor_logic_valid_flags_t;

typedef enum {
    SENSOR_LOGIC_ERROR_NONE = 0U,
    SENSOR_LOGIC_ERROR_FSR_ADC = (1U << 0U),
    SENSOR_LOGIC_ERROR_FSR_TIMEOUT = (1U << 1U),
    SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT = (1U << 2U),
    SENSOR_LOGIC_ERROR_ULTRASONIC_REAR = (1U << 3U),
    SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT = (1U << 4U),
    SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT = (1U << 5U)
} sensor_logic_error_flags_t;

typedef enum {
    SENSOR_LOGIC_DIRECTION_NONE = 0U,
    SENSOR_LOGIC_DIRECTION_FRONT = (1U << 0U),
    SENSOR_LOGIC_DIRECTION_REAR = (1U << 1U),
    SENSOR_LOGIC_DIRECTION_LEFT = (1U << 2U),
    SENSOR_LOGIC_DIRECTION_RIGHT = (1U << 3U)
} sensor_logic_direction_flags_t;

typedef enum {
    SENSOR_LOGIC_SAFETY_NONE = 0U,
    SENSOR_LOGIC_SAFETY_LINE_LOST = (1U << 0U),
    SENSOR_LOGIC_SAFETY_OBSTACLE = (1U << 1U),
    SENSOR_LOGIC_SAFETY_OVERLOAD = (1U << 2U)
} sensor_logic_safety_flags_t;

typedef enum { SENSOR_MARKER_IDLE = 0, SENSOR_MARKER_BLACK_CANDIDATE, SENSOR_MARKER_CONFIRMED } sensor_marker_state_t;

typedef enum {
    SENSOR_MARKER_EVENT_NONE = 0,
    SENSOR_MARKER_EVENT_DETECTED,
    SENSOR_MARKER_EVENT_INVALID_WIDTH,
    SENSOR_MARKER_EVENT_INVALID_TRANSITION,
    SENSOR_MARKER_EVENT_INVALID_COUNT
} sensor_marker_event_type_t;

typedef struct {
    uint32_t event_flags;
    uint32_t safety_activated_flags;
    uint32_t safety_cleared_flags;
} sensor_logic_update_t;

typedef struct {
    uint32_t valid_flags;
    uint32_t error_flags;
    uint32_t line_changed_at_ms;
    uint32_t marker_detected_at_ms;
    uint32_t marker_sequence;
    uint32_t load_changed_at_ms;
    uint32_t obstacle_changed_at_ms;
    uint32_t error_changed_at_ms;
    uint16_t fsr_filtered;
    uint16_t fsr_empty_baseline;
    uint16_t marker_duration_ms;
    uint8_t obstacle_mask;
    uint8_t overload_active;
    uint8_t marker_active;
} sensor_logic_diagnostics_t;

typedef struct {
    sensor_marker_event_type_t type;
    uint32_t sequence;
    uint32_t detected_at_ms;
    uint16_t gap_duration_ms;
    linetracer_line_state_t entry_state;
    linetracer_line_state_t exit_state;
    app_marker_code_t code;
    uint8_t count;
} sensor_marker_event_t;

typedef struct {
    uint32_t pending_flags;
} sensor_event_latch_t;

typedef struct {
    uint8_t stable;
    uint8_t candidate;
    uint8_t count;
    uint8_t initialized;
} sensor_debounce_filter_t;

typedef struct {
    uint16_t samples[SENSOR_FSR_FILTER_SAMPLES];
    uint32_t sum;
    uint8_t count;
    uint8_t next_index;
} sensor_fsr_filter_t;

typedef enum { SENSOR_FSR_BASELINE_FOR_LOAD_ON = 0, SENSOR_FSR_BASELINE_FOR_LOAD_OFF } sensor_fsr_baseline_mode_t;

typedef struct {
    sensor_debounce_filter_t line_left_filter;
    sensor_debounce_filter_t line_center_filter;
    sensor_debounce_filter_t line_right_filter;
    sensor_fsr_filter_t fsr_filter;
    uint16_t line_left_filtered;
    uint16_t line_right_filtered;
    uint16_t line_center_filtered;
    app_sensor_snapshot_t snapshot;
    sensor_logic_diagnostics_t diagnostics;
    sensor_marker_event_t latest_marker_event;
    uint32_t marker_black_since_ms;
    uint32_t marker_rearm_since_ms;
    uint32_t marker_group_last_stripe_at_ms;
    uint32_t fsr_candidate_since_ms;
    uint32_t fsr_baseline_sum;
    uint32_t overload_candidate_since_ms;
    uint32_t last_fsr_sample_ms;
    uint32_t ultrasonic_last_success_ms[SENSOR_LOGIC_ULTRASONIC_COUNT];
    linetracer_line_state_t marker_entry_state;
    sensor_marker_state_t marker_state;
    uint8_t ultrasonic_failure_count[SENSOR_LOGIC_ULTRASONIC_COUNT];
    uint8_t ultrasonic_recovery_count[SENSOR_LOGIC_ULTRASONIC_COUNT];
    uint8_t ultrasonic_obstacle_on_count[SENSOR_LOGIC_ULTRASONIC_COUNT];
    uint8_t ultrasonic_obstacle_off_count[SENSOR_LOGIC_ULTRASONIC_COUNT];
    uint8_t ultrasonic_started_mask;
    uint8_t marker_event_valid;
    uint8_t marker_rearm_active;
    uint8_t marker_group_count;
    uint8_t marker_group_active;
    uint8_t fsr_candidate_loaded;
    uint8_t fsr_baseline_capture_active;
    uint8_t fsr_baseline_valid;
    uint8_t fsr_baseline_sample_count;
    sensor_fsr_baseline_mode_t fsr_baseline_mode;
    uint8_t overload_candidate_active;
    uint8_t line_analog_initialized;
    uint8_t line_center_analog_initialized;
    uint8_t line_left_black;
    uint8_t line_right_black;
    uint8_t line_center_black;
} sensor_logic_context_t;

void SensorLogic_Init(sensor_logic_context_t* context, uint32_t now_ms);
void SensorLogic_UpdateLine(sensor_logic_context_t* context, uint8_t line_left, uint8_t line_center, uint8_t line_right,
                            uint32_t now_ms, sensor_logic_update_t* update);
void SensorLogic_UpdateLineAnalogRaw(sensor_logic_context_t* context, uint16_t line_left_raw, uint16_t line_right_raw);
void SensorLogic_UpdateLineCenter(sensor_logic_context_t* context, uint8_t line_center, uint16_t line_center_raw);
void SensorLogic_UpdateFsr(sensor_logic_context_t* context, uint16_t raw_value, uint32_t now_ms,
                           sensor_logic_update_t* update);
void SensorLogic_StartFsrBaselineCapture(sensor_logic_context_t* context, sensor_fsr_baseline_mode_t mode);
void SensorLogic_MarkFsrError(sensor_logic_context_t* context, uint32_t error_flag, uint32_t now_ms);
void SensorLogic_MarkAllUltrasonicUnavailable(sensor_logic_context_t* context, uint32_t now_ms);
void SensorLogic_MarkUltrasonicStarted(sensor_logic_context_t* context, uint8_t sensor_index, uint32_t now_ms);
void SensorLogic_UpdateUltrasonic(sensor_logic_context_t* context, uint8_t sensor_index, uint16_t distance_mm,
                                  uint8_t valid, uint32_t now_ms, sensor_logic_update_t* update);
void SensorLogic_CheckStaleness(sensor_logic_context_t* context, uint32_t now_ms);
uint32_t SensorLogic_GetEffectiveSafetyErrorFlags(uint32_t raw_error_flags);
uint8_t SensorLogic_GetEffectiveSafetyObstacleMask(uint8_t raw_obstacle_mask);

const app_sensor_snapshot_t* SensorLogic_GetSnapshot(const sensor_logic_context_t* context);
const sensor_logic_diagnostics_t* SensorLogic_GetDiagnostics(const sensor_logic_context_t* context);
uint8_t SensorLogic_GetLatestMarker(const sensor_logic_context_t* context, sensor_marker_event_t* event);

void SensorEventLatch_Init(sensor_event_latch_t* latch);
uint32_t SensorEventLatch_Pend(sensor_event_latch_t* latch, uint32_t event_flags);
void SensorEventLatch_Acknowledge(sensor_event_latch_t* latch, uint32_t event_flags);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_LOGIC_H */
