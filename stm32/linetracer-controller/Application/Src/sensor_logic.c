#include "sensor_logic.h"

#include <stddef.h>
#include <string.h>

static const uint32_t s_ultrasonic_valid_flags[SENSOR_LOGIC_ULTRASONIC_COUNT] = {
    SENSOR_LOGIC_VALID_ULTRASONIC_FRONT,
    SENSOR_LOGIC_VALID_ULTRASONIC_REAR,
    SENSOR_LOGIC_VALID_ULTRASONIC_LEFT,
    SENSOR_LOGIC_VALID_ULTRASONIC_RIGHT,
};

static const uint32_t s_ultrasonic_error_flags[SENSOR_LOGIC_ULTRASONIC_COUNT] = {
    SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT,
    SENSOR_LOGIC_ERROR_ULTRASONIC_REAR,
    SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT,
    SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT,
};

static const uint8_t s_ultrasonic_direction_flags[SENSOR_LOGIC_ULTRASONIC_COUNT] = {
    SENSOR_LOGIC_DIRECTION_FRONT,
    SENSOR_LOGIC_DIRECTION_REAR,
    SENSOR_LOGIC_DIRECTION_LEFT,
    SENSOR_LOGIC_DIRECTION_RIGHT,
};

#define SENSOR_LOGIC_ALL_ULTRASONIC_VALID_FLAGS                                                                      \
    (SENSOR_LOGIC_VALID_ULTRASONIC_FRONT | SENSOR_LOGIC_VALID_ULTRASONIC_REAR | SENSOR_LOGIC_VALID_ULTRASONIC_LEFT | \
     SENSOR_LOGIC_VALID_ULTRASONIC_RIGHT)
#define SENSOR_LOGIC_ALL_ULTRASONIC_ERROR_FLAGS                                                                      \
    (SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_REAR | SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | \
     SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT)
#define SENSOR_LOGIC_ENABLED_ULTRASONIC_ERROR_FLAGS                                                                   \
    (SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT | \
     (SENSOR_ULTRASONIC_REAR_ENABLED ? SENSOR_LOGIC_ERROR_ULTRASONIC_REAR : 0U))

static uint8_t SensorLogic_UltrasonicIsEnabled(uint8_t sensor_index) {
    if (sensor_index >= SENSOR_LOGIC_ULTRASONIC_COUNT) {
        return 0U;
    }

#if !SENSOR_ULTRASONIC_REAR_ENABLED
    if (sensor_index == 1U) {
        return 0U;
    }
#endif

    return 1U;
}

static uint8_t SensorLogic_TimeElapsed(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms) {
    return ((uint32_t)(now_ms - since_ms) >= duration_ms) ? 1U : 0U;
}

static uint8_t SensorLogic_DebounceUpdate(sensor_debounce_filter_t* filter, uint8_t input) {
    input = (input != 0U) ? 1U : 0U;

    if ((filter->count == 0U) || (filter->candidate != input)) {
        filter->candidate = input;
        filter->count = 1U;
    } else if (filter->count < APP_TIMING_LINE_DEBOUNCE_SAMPLES) {
        filter->count++;
    }

    if (filter->count < APP_TIMING_LINE_DEBOUNCE_SAMPLES) {
        return 0U;
    }

    if ((filter->initialized == 0U) || (filter->stable != filter->candidate)) {
        filter->stable = filter->candidate;
        filter->initialized = 1U;
        return 1U;
    }

    return 0U;
}

static linetracer_line_state_t SensorLogic_CalculateTrackingState(uint8_t left_black, uint8_t center_black,
                                                                  uint8_t right_black) {
    if ((left_black != 0U) && (right_black == 0U)) {
        return LINETRACER_LINE_LEFT_ONLY;
    }
    if ((left_black == 0U) && (right_black != 0U)) {
        return LINETRACER_LINE_RIGHT_ONLY;
    }
    if ((left_black == 0U) && (center_black == 0U) && (right_black == 0U)) {
        return LINETRACER_LINE_WHITE_GAP;
    }
    return LINETRACER_LINE_CENTERED;
}

static void SensorLogic_RememberLineError(sensor_logic_context_t* context, int16_t line_error, uint32_t now_ms) {
    if ((context == NULL) || (line_error == 0)) {
        return;
    }

    context->line_last_valid_error = line_error;
    context->line_last_valid_at_ms = now_ms;
    context->line_last_valid_error_valid = 1U;
}

static int16_t SensorLogic_ApplyDigitalPidDirection(sensor_logic_context_t* context, int16_t analog_error,
                                                    linetracer_line_state_t line_state, uint8_t center_black,
                                                    uint32_t now_ms) {
    int32_t magnitude = analog_error;
    int16_t directed_error;

    if (context == NULL) {
        return 0;
    }

    if (magnitude < 0) {
        magnitude = -magnitude;
    }
    if (magnitude < (int32_t)SENSOR_LINE_DO_PID_MIN_ERROR) {
        magnitude = (int32_t)SENSOR_LINE_DO_PID_MIN_ERROR;
    }

    switch (line_state) {
        case LINETRACER_LINE_LEFT_ONLY:
            /* Black is on the left: slow the physical left wheel and steer left. */
            directed_error = (int16_t)magnitude;
            SensorLogic_RememberLineError(context, directed_error, now_ms);
            return directed_error;

        case LINETRACER_LINE_RIGHT_ONLY:
            /* Black is on the right: slow the physical right wheel and steer right. */
            directed_error = (int16_t)-magnitude;
            SensorLogic_RememberLineError(context, directed_error, now_ms);
            return directed_error;

        case LINETRACER_LINE_CENTERED:
            /* 010 is centred; three-sensor AO still provides small continuous correction before DO changes. */
            if ((center_black != 0U) || (context->line_analog_signal_valid != 0U)) {
                SensorLogic_RememberLineError(context, analog_error, now_ms);
                return analog_error;
            }
            return 0;

        case LINETRACER_LINE_WHITE_GAP:
            /* A valid AO centroid overrides a simultaneous DO miss. */
            if (context->line_analog_signal_valid != 0U) {
                SensorLogic_RememberLineError(context, analog_error, now_ms);
                return analog_error;
            }

            /* Bridge only a short physical gap, using the latest trustworthy direction. */
            if ((context->line_last_valid_error_valid != 0U) &&
                (SensorLogic_TimeElapsed(now_ms, context->line_last_valid_at_ms, SENSOR_LINE_LOST_RECOVERY_MS) == 0U)) {
                magnitude = context->line_last_valid_error;
                if (magnitude < 0) {
                    magnitude = -magnitude;
                }
                if (magnitude < (int32_t)SENSOR_LINE_DO_PID_MIN_ERROR) {
                    magnitude = (int32_t)SENSOR_LINE_DO_PID_MIN_ERROR;
                }
                return (context->line_last_valid_error > 0) ? (int16_t)magnitude : (int16_t)-magnitude;
            }
            return 0;

        case LINETRACER_LINE_UNKNOWN:
        default:
            return 0;
    }
}

static uint16_t SensorLogic_NormalizeLineRaw(uint16_t raw_value, uint16_t white_raw, uint16_t black_raw) {
    uint32_t normalized;

    if (raw_value <= white_raw) {
        return 0U;
    }
    if (raw_value >= black_raw) {
        return (uint16_t)SENSOR_LINE_NORMALIZED_MAX;
    }

    normalized = ((uint32_t)(raw_value - white_raw) * SENSOR_LINE_NORMALIZED_MAX) / (uint32_t)(black_raw - white_raw);
    return (uint16_t)normalized;
}

static uint16_t SensorLogic_FilterLineNormalized(uint16_t previous, uint16_t current) {
    return (uint16_t)((((uint32_t)previous * SENSOR_LINE_FILTER_PREVIOUS_WEIGHT) + current) /
                      SENSOR_LINE_FILTER_DIVISOR);
}

static uint8_t SensorLogic_UpdateBlackHysteresis(uint16_t normalized, uint8_t previous_black) {
    if (normalized >= SENSOR_LINE_BLACK_ENTER_THRESHOLD) {
        return 1U;
    }
    if (normalized <= SENSOR_LINE_BLACK_EXIT_THRESHOLD) {
        return 0U;
    }
    return previous_black;
}

static void SensorLogic_SetErrorFlags(sensor_logic_context_t* context, uint32_t error_flags, uint32_t now_ms) {
    if (context->diagnostics.error_flags != error_flags) {
        context->diagnostics.error_flags = error_flags;
        context->diagnostics.error_changed_at_ms = now_ms;
    }
}

static app_marker_code_t SensorLogic_MarkerCodeFromCount(uint8_t count) {
    switch (count) {
        case 1U:
        case 2U:
        case 3U:
        case 4U:
            /* Route phase owns marker meaning; every valid physical marker uses the same code. */
            return APP_MARKER_JUNCTION;
        default:
            return APP_MARKER_INVALID;
    }
}

static void SensorLogic_ResetMarkerGroup(sensor_logic_context_t* context) {
    context->marker_group_active = 0U;
    context->marker_group_count = 0U;
    context->marker_group_last_stripe_at_ms = 0U;
}

static void SensorLogic_PublishMarker(sensor_logic_context_t* context, sensor_marker_event_type_t type,
                                      app_marker_code_t code, uint8_t count, linetracer_line_state_t exit_state,
                                      uint32_t duration_ms, uint32_t detected_at_ms, sensor_logic_update_t* update) {
    context->diagnostics.marker_detected_at_ms = detected_at_ms;
    context->diagnostics.marker_duration_ms = (duration_ms <= UINT16_MAX) ? (uint16_t)duration_ms : UINT16_MAX;
    context->diagnostics.marker_sequence++;

    context->snapshot.marker_code = code;
    context->snapshot.marker_count = count;
    context->snapshot.marker_detected_at_ms = detected_at_ms;
    update->event_flags |= APP_SENSOR_EVENT_MARKER;

    context->latest_marker_event.type = type;
    context->latest_marker_event.sequence = context->diagnostics.marker_sequence;
    context->latest_marker_event.detected_at_ms = detected_at_ms;
    context->latest_marker_event.gap_duration_ms = context->diagnostics.marker_duration_ms;
    context->latest_marker_event.entry_state = context->marker_entry_state;
    context->latest_marker_event.exit_state = exit_state;
    context->latest_marker_event.code = code;
    context->latest_marker_event.count = count;
    context->marker_event_valid = 1U;
}

static void SensorLogic_AccumulateMarkerStripe(sensor_logic_context_t* context, uint32_t duration_ms, uint32_t now_ms) {
    if (context->marker_group_count < UINT8_MAX) {
        context->marker_group_count++;
    }

    context->marker_group_active = 1U;
    context->marker_group_last_stripe_at_ms = now_ms;
    context->diagnostics.marker_duration_ms = (duration_ms <= UINT16_MAX) ? (uint16_t)duration_ms : UINT16_MAX;
}

static void SensorLogic_PublishInvalidMarker(sensor_logic_context_t* context, sensor_marker_event_type_t type,
                                             linetracer_line_state_t exit_state, uint32_t duration_ms, uint32_t now_ms,
                                             sensor_logic_update_t* update) {
    uint8_t observed_count = context->marker_group_count;

    if (observed_count < UINT8_MAX) {
        observed_count++;
    }

    SensorLogic_PublishMarker(context, type, APP_MARKER_INVALID, observed_count, exit_state, duration_ms, now_ms,
                              update);
    SensorLogic_ResetMarkerGroup(context);
}

static void SensorLogic_FinalizeMarkerGroup(sensor_logic_context_t* context, uint32_t now_ms,
                                            sensor_logic_update_t* update) {
    app_marker_code_t code;
    sensor_marker_event_type_t type;

    if ((context->marker_group_active == 0U) || (context->snapshot.marker_active != 0U) ||
        (SensorLogic_TimeElapsed(now_ms, context->marker_group_last_stripe_at_ms, SENSOR_MARKER_GROUP_TIMEOUT_MS) ==
         0U)) {
        return;
    }

    code = SensorLogic_MarkerCodeFromCount(context->marker_group_count);
    type = (code == APP_MARKER_INVALID) ? SENSOR_MARKER_EVENT_INVALID_COUNT : SENSOR_MARKER_EVENT_DETECTED;

    SensorLogic_PublishMarker(context, type, code, context->marker_group_count, LINETRACER_LINE_CENTERED,
                              context->diagnostics.marker_duration_ms, context->marker_group_last_stripe_at_ms, update);
    SensorLogic_ResetMarkerGroup(context);
}

static void SensorLogic_UpdateMarkerRearm(sensor_logic_context_t* context, uint32_t now_ms) {
    if ((context->marker_state != SENSOR_MARKER_CONFIRMED) || (context->snapshot.marker_active != 0U) ||
        (context->marker_rearm_active == 0U)) {
        return;
    }

    if (SensorLogic_TimeElapsed(now_ms, context->marker_rearm_since_ms, SENSOR_MARKER_REARM_MS) != 0U) {
        context->marker_state = SENSOR_MARKER_IDLE;
        context->marker_rearm_active = 0U;
    }
}

static uint16_t SensorLogic_FilterFsr(sensor_fsr_filter_t* filter, uint16_t sample) {
    if (filter->count < SENSOR_FSR_FILTER_SAMPLES) {
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
        filter->count++;
    } else {
        filter->sum -= filter->samples[filter->next_index];
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
    }

    filter->next_index = (uint8_t)((filter->next_index + 1U) % SENSOR_FSR_FILTER_SAMPLES);
    return (uint16_t)(filter->sum / filter->count);
}

static uint16_t* SensorLogic_UltrasonicDistanceField(app_sensor_snapshot_t* snapshot, uint8_t sensor_index) {
    switch (sensor_index) {
        case 0U:
            return &snapshot->ultrasonic_front_mm;
        case 1U:
            return &snapshot->ultrasonic_rear_mm;
        case 2U:
            return &snapshot->ultrasonic_left_mm;
        case 3U:
            return &snapshot->ultrasonic_right_mm;
        default:
            return NULL;
    }
}

void SensorLogic_Init(sensor_logic_context_t* context, uint32_t now_ms) {
    uint8_t index;

    if (context == NULL) {
        return;
    }

    (void)memset(context, 0, sizeof(*context));
    context->snapshot.sampled_at_ms = now_ms;
    context->snapshot.line_state = LINETRACER_LINE_UNKNOWN;
    context->snapshot.load_state = UART_LINETRACER_LOAD_EMPTY;
    context->snapshot.ultrasonic_front_mm = UINT16_MAX;
    context->snapshot.ultrasonic_rear_mm = UINT16_MAX;
    context->snapshot.ultrasonic_left_mm = UINT16_MAX;
    context->snapshot.ultrasonic_right_mm = UINT16_MAX;
    context->marker_entry_state = LINETRACER_LINE_UNKNOWN;
    context->marker_state = SENSOR_MARKER_IDLE;
    context->last_fsr_sample_ms = now_ms;
    context->fsr_candidate_since_ms = now_ms;
    context->overload_candidate_since_ms = now_ms;

    for (index = 0U; index < SENSOR_LOGIC_ULTRASONIC_COUNT; index++) {
        context->ultrasonic_last_success_ms[index] = now_ms;
    }
}

void SensorLogic_ResetLineTrackingHistory(sensor_logic_context_t* context) {
    if (context == NULL) {
        return;
    }

    /* Keep debounced DO and marker state, but discard steering history from the completed pivot. */
    context->line_analog_error = 0;
    context->line_last_valid_error = 0;
    context->line_last_valid_at_ms = 0U;
    context->line_analog_initialized = 0U;
    context->line_analog_signal_valid = 0U;
    context->line_last_valid_error_valid = 0U;
    context->snapshot.line_error = 0;
}

void SensorLogic_StartFsrBaselineCapture(sensor_logic_context_t* context, sensor_fsr_baseline_mode_t mode) {
    if (context == NULL) {
        return;
    }

    context->fsr_baseline_capture_active = 1U;
    context->fsr_baseline_valid = 0U;
    context->fsr_baseline_sample_count = 0U;
    context->fsr_baseline_sum = 0U;
    context->diagnostics.fsr_empty_baseline = 0U;
    context->fsr_baseline_mode = mode;
    context->fsr_candidate_loaded = (context->snapshot.load_state == UART_LINETRACER_LOAD_PRESENT) ? 1U : 0U;
}

void SensorLogic_UpdateLine(sensor_logic_context_t* context, uint8_t line_left, uint8_t line_center, uint8_t line_right,
                            uint32_t now_ms, sensor_logic_update_t* update) {
    uint8_t left_changed;
    uint8_t center_changed;
    uint8_t right_changed;
    uint8_t both_outer_black;
    linetracer_line_state_t previous_state;
    linetracer_line_state_t next_state;
    uint32_t black_duration_ms;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    SensorLogic_FinalizeMarkerGroup(context, now_ms, update);

    left_changed = SensorLogic_DebounceUpdate(&context->line_left_filter, line_left);
    center_changed = SensorLogic_DebounceUpdate(&context->line_center_filter, line_center);
    right_changed = SensorLogic_DebounceUpdate(&context->line_right_filter, line_right);

    if ((context->line_left_filter.initialized == 0U) || (context->line_center_filter.initialized == 0U) ||
        (context->line_right_filter.initialized == 0U)) {
        return;
    }

    context->diagnostics.valid_flags |= SENSOR_LOGIC_VALID_LINE;
    context->snapshot.line_left = context->line_left_filter.stable;
    context->snapshot.line_center = context->line_center_filter.stable;
    context->snapshot.line_right = context->line_right_filter.stable;
    /*
     * The two outer sensors are the tracking authority and are aligned across
     * the vehicle. A transverse
     * marker covers both outer sensors even when the
     * center sensor's DO threshold reacts late or misses the
     * short stripe.
     */
    both_outer_black = ((context->snapshot.line_left != 0U) && (context->snapshot.line_right != 0U)) ? 1U : 0U;

    if (both_outer_black != 0U) {
        if (context->marker_state == SENSOR_MARKER_IDLE) {
            context->marker_state = SENSOR_MARKER_BLACK_CANDIDATE;
            context->marker_entry_state = context->snapshot.line_state;
            context->marker_black_since_ms = now_ms;
            context->snapshot.marker_active = 1U;
            context->diagnostics.marker_active = 1U;
            context->marker_rearm_active = 0U;
        }
        return;
    }

    if (context->marker_state == SENSOR_MARKER_BLACK_CANDIDATE) {
        black_duration_ms = (uint32_t)(now_ms - context->marker_black_since_ms);
        context->snapshot.marker_active = 0U;
        context->snapshot.marker_cleared_at_ms = now_ms;
        context->diagnostics.marker_active = 0U;
        update->event_flags |= APP_SENSOR_EVENT_MARKER_CLEARED;

        if ((black_duration_ms >= SENSOR_MARKER_MIN_BLACK_MS) && (black_duration_ms <= SENSOR_MARKER_MAX_BLACK_MS)) {
            SensorLogic_AccumulateMarkerStripe(context, black_duration_ms, now_ms);
        } else if (black_duration_ms > SENSOR_MARKER_MAX_BLACK_MS) {
            SensorLogic_PublishInvalidMarker(
                context, SENSOR_MARKER_EVENT_INVALID_WIDTH,
                SensorLogic_CalculateTrackingState(context->snapshot.line_left, context->snapshot.line_center,
                                                   context->snapshot.line_right),
                black_duration_ms, now_ms, update);
        }

        context->marker_state = SENSOR_MARKER_CONFIRMED;
        context->marker_rearm_active = 1U;
        context->marker_rearm_since_ms = now_ms;
    }

    previous_state = context->snapshot.line_state;
    next_state = SensorLogic_CalculateTrackingState(context->snapshot.line_left, context->snapshot.line_center,
                                                    context->snapshot.line_right);
    context->snapshot.line_error = SensorLogic_ApplyDigitalPidDirection(context, context->line_analog_error, next_state,
                                                                        context->snapshot.line_center, now_ms);
    if (((left_changed != 0U) || (center_changed != 0U) || (right_changed != 0U)) && (next_state != previous_state)) {
        context->snapshot.line_state = next_state;
        context->diagnostics.line_changed_at_ms = now_ms;
        update->event_flags |= APP_SENSOR_EVENT_LINE_CHANGED;
    }

    SensorLogic_UpdateMarkerRearm(context, now_ms);
}

void SensorLogic_UpdateLineAnalogRawWithCenter(sensor_logic_context_t* context, uint16_t line_left_raw,
                                               uint16_t line_center_raw, uint16_t line_right_raw) {
    uint16_t line_left_normalized;
    uint16_t line_center_normalized;
    uint16_t line_right_normalized;
    uint32_t line_signal;
    int32_t line_numerator;
    int32_t line_error;

    if (context == NULL) {
        return;
    }

    context->snapshot.line_left_raw = line_left_raw;
    context->snapshot.line_center_raw = line_center_raw;
    context->snapshot.line_right_raw = line_right_raw;

    line_left_normalized =
        SensorLogic_NormalizeLineRaw(line_left_raw, SENSOR_LINE_LEFT_WHITE_RAW, SENSOR_LINE_LEFT_BLACK_RAW);
    line_center_normalized =
        SensorLogic_NormalizeLineRaw(line_center_raw, SENSOR_LINE_CENTER_WHITE_RAW, SENSOR_LINE_CENTER_BLACK_RAW);
    line_right_normalized =
        SensorLogic_NormalizeLineRaw(line_right_raw, SENSOR_LINE_RIGHT_WHITE_RAW, SENSOR_LINE_RIGHT_BLACK_RAW);

    if (context->line_analog_initialized == 0U) {
        context->line_left_filtered = line_left_normalized;
        context->line_center_filtered = line_center_normalized;
        context->line_right_filtered = line_right_normalized;
        context->line_analog_initialized = 1U;
    } else {
        context->line_left_filtered =
            SensorLogic_FilterLineNormalized(context->line_left_filtered, line_left_normalized);
        context->line_center_filtered =
            SensorLogic_FilterLineNormalized(context->line_center_filtered, line_center_normalized);
        context->line_right_filtered =
            SensorLogic_FilterLineNormalized(context->line_right_filtered, line_right_normalized);
    }

    context->line_left_black = SensorLogic_UpdateBlackHysteresis(context->line_left_filtered, context->line_left_black);
    context->line_right_black =
        SensorLogic_UpdateBlackHysteresis(context->line_right_filtered, context->line_right_black);

    line_signal = (uint32_t)context->line_left_filtered + (uint32_t)context->line_center_filtered +
                  (uint32_t)context->line_right_filtered;
    if (line_signal >= SENSOR_LINE_ANALOG_MIN_SIGNAL) {
        line_numerator = ((int32_t)context->line_left_filtered - (int32_t)context->line_right_filtered) *
                         (int32_t)SENSOR_LINE_NORMALIZED_MAX;
        line_error = line_numerator / (int32_t)line_signal;
        context->line_analog_signal_valid = 1U;
    } else {
        line_error = 0;
        context->line_analog_signal_valid = 0U;
    }

    if (line_error > (int32_t)SENSOR_LINE_NORMALIZED_MAX) {
        line_error = (int32_t)SENSOR_LINE_NORMALIZED_MAX;
    } else if (line_error < -(int32_t)SENSOR_LINE_NORMALIZED_MAX) {
        line_error = -(int32_t)SENSOR_LINE_NORMALIZED_MAX;
    }

    /*
     * Ignore a small calibrated sensor mismatch; otherwise PID would
     * continuously bias one wheel while the
     * vehicle is already centred.
     */
    if (line_error <= (int32_t)SENSOR_LINE_ERROR_DEADBAND && line_error >= -(int32_t)SENSOR_LINE_ERROR_DEADBAND) {
        line_error = 0;
    }

    context->line_analog_error = (int16_t)line_error;
    context->snapshot.line_error = context->line_analog_error;
}

void SensorLogic_UpdateLineAnalogRaw(sensor_logic_context_t* context, uint16_t line_left_raw, uint16_t line_right_raw) {
    SensorLogic_UpdateLineAnalogRawWithCenter(context, line_left_raw, SENSOR_LINE_CENTER_WHITE_RAW, line_right_raw);
}

void SensorLogic_UpdateLineCenter(sensor_logic_context_t* context, uint8_t line_center, uint16_t line_center_raw) {
    if (context == NULL) {
        return;
    }

    /* PB8 remains the center DO authority; the ADC value participates in the weighted PID position. */
    context->snapshot.line_center_raw = line_center_raw;
    context->line_center_black = (line_center != 0U) ? 1U : 0U;
    context->snapshot.line_center = context->line_center_black;
}

void SensorLogic_UpdateFsr(sensor_logic_context_t* context, uint16_t raw_value, uint32_t now_ms,
                           sensor_logic_update_t* update) {
    uint8_t currently_loaded;
    uint8_t desired_loaded;
    uint8_t load_detection_available;
    uint8_t currently_overloaded;
    uint8_t desired_overloaded;
    uint32_t error_flags;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    context->snapshot.fsr_raw = raw_value;
    context->snapshot.fsr_valid = 1U;
    context->diagnostics.fsr_filtered = SensorLogic_FilterFsr(&context->fsr_filter, raw_value);
    context->diagnostics.valid_flags |= SENSOR_LOGIC_VALID_FSR;
    error_flags = context->diagnostics.error_flags & ~(SENSOR_LOGIC_ERROR_FSR_ADC | SENSOR_LOGIC_ERROR_FSR_TIMEOUT);
    SensorLogic_SetErrorFlags(context, error_flags, now_ms);
    context->last_fsr_sample_ms = now_ms;

    if (context->fsr_baseline_capture_active != 0U) {
        context->fsr_baseline_sum += context->diagnostics.fsr_filtered;
        if (context->fsr_baseline_sample_count < SENSOR_FSR_BASELINE_SAMPLES) {
            context->fsr_baseline_sample_count++;
        }

        if (context->fsr_baseline_sample_count >= SENSOR_FSR_BASELINE_SAMPLES) {
            context->diagnostics.fsr_empty_baseline =
                (uint16_t)(context->fsr_baseline_sum / SENSOR_FSR_BASELINE_SAMPLES);
            context->fsr_baseline_capture_active = 0U;
            context->fsr_baseline_valid = 1U;
            context->fsr_candidate_since_ms = now_ms;
            update->event_flags |= APP_SENSOR_EVENT_FSR_BASELINE_READY;
        }
    }

    load_detection_available =
        (context->fsr_baseline_capture_active == 0U && context->fsr_baseline_valid != 0U) ? 1U : 0U;

    if (load_detection_available != 0U) {
        currently_loaded = (context->snapshot.load_state == UART_LINETRACER_LOAD_PRESENT) ? 1U : 0U;
        desired_loaded = currently_loaded;

        if (context->fsr_baseline_mode == SENSOR_FSR_BASELINE_FOR_LOAD_ON) {
            if ((currently_loaded == 0U) &&
                (context->diagnostics.fsr_filtered >= context->diagnostics.fsr_empty_baseline) &&
                ((uint16_t)(context->diagnostics.fsr_filtered - context->diagnostics.fsr_empty_baseline) >=
                 SENSOR_FSR_LOAD_ON_DELTA)) {
                desired_loaded = 1U;
            }
        } else if ((currently_loaded != 0U) &&
                   (context->diagnostics.fsr_empty_baseline >= context->diagnostics.fsr_filtered) &&
                   ((uint16_t)(context->diagnostics.fsr_empty_baseline - context->diagnostics.fsr_filtered) >=
                    SENSOR_FSR_LOAD_OFF_DELTA)) {
            desired_loaded = 0U;
        }

        if (desired_loaded == currently_loaded) {
            context->fsr_candidate_loaded = currently_loaded;
            context->fsr_candidate_since_ms = now_ms;
        } else if (desired_loaded != context->fsr_candidate_loaded) {
            context->fsr_candidate_loaded = desired_loaded;
            context->fsr_candidate_since_ms = now_ms;
        } else if (SensorLogic_TimeElapsed(now_ms, context->fsr_candidate_since_ms, APP_TIMING_FSR_STABLE_MS) != 0U) {
            context->snapshot.load_state =
                (desired_loaded != 0U) ? UART_LINETRACER_LOAD_PRESENT : UART_LINETRACER_LOAD_EMPTY;
            context->diagnostics.load_changed_at_ms = now_ms;
            update->event_flags |= (desired_loaded != 0U) ? APP_SENSOR_EVENT_LOAD_ON : APP_SENSOR_EVENT_LOAD_OFF;
            context->fsr_candidate_since_ms = now_ms;
        }
    }

    currently_overloaded = context->diagnostics.overload_active;
    desired_overloaded = currently_overloaded;

    if ((currently_overloaded == 0U) && (context->diagnostics.fsr_filtered >= SENSOR_FSR_OVERLOAD_THRESHOLD)) {
        desired_overloaded = 1U;
    } else if ((currently_overloaded != 0U) &&
               (context->diagnostics.fsr_filtered <= SENSOR_FSR_OVERLOAD_CLEAR_THRESHOLD)) {
        desired_overloaded = 0U;
    }

    if (desired_overloaded == currently_overloaded) {
        context->overload_candidate_active = currently_overloaded;
        context->overload_candidate_since_ms = now_ms;
    } else if (desired_overloaded != context->overload_candidate_active) {
        context->overload_candidate_active = desired_overloaded;
        context->overload_candidate_since_ms = now_ms;
    } else if (SensorLogic_TimeElapsed(now_ms, context->overload_candidate_since_ms, APP_TIMING_FSR_STABLE_MS) != 0U) {
        context->diagnostics.overload_active = desired_overloaded;
        if (desired_overloaded != 0U) {
            update->event_flags |= APP_SENSOR_EVENT_OVERLOAD;
            update->safety_activated_flags |= SENSOR_LOGIC_SAFETY_OVERLOAD;
        } else {
            update->safety_cleared_flags |= SENSOR_LOGIC_SAFETY_OVERLOAD;
        }
        context->overload_candidate_since_ms = now_ms;
    }
}

void SensorLogic_MarkFsrError(sensor_logic_context_t* context, uint32_t error_flag, uint32_t now_ms) {
    uint32_t allowed_flags = SENSOR_LOGIC_ERROR_FSR_ADC | SENSOR_LOGIC_ERROR_FSR_TIMEOUT;

    if (context == NULL) {
        return;
    }

    context->diagnostics.valid_flags &= ~SENSOR_LOGIC_VALID_FSR;
    context->snapshot.fsr_valid = 0U;
    SensorLogic_SetErrorFlags(context, context->diagnostics.error_flags | (error_flag & allowed_flags), now_ms);
}

void SensorLogic_MarkAllUltrasonicUnavailable(sensor_logic_context_t* context, uint32_t now_ms) {
    if (context == NULL) {
        return;
    }

    context->diagnostics.valid_flags &= ~SENSOR_LOGIC_ALL_ULTRASONIC_VALID_FLAGS;
    SensorLogic_SetErrorFlags(context,
                              (context->diagnostics.error_flags & ~SENSOR_LOGIC_ALL_ULTRASONIC_ERROR_FLAGS) |
                                  SENSOR_LOGIC_ENABLED_ULTRASONIC_ERROR_FLAGS,
                              now_ms);
#if !SENSOR_ULTRASONIC_REAR_ENABLED
    context->diagnostics.obstacle_mask &= (uint8_t)~SENSOR_LOGIC_DIRECTION_REAR;
#endif
}

void SensorLogic_MarkUltrasonicStarted(sensor_logic_context_t* context, uint8_t sensor_index, uint32_t now_ms) {
    uint8_t direction_flag;

    if ((context == NULL) || (SensorLogic_UltrasonicIsEnabled(sensor_index) == 0U)) {
        return;
    }

    direction_flag = s_ultrasonic_direction_flags[sensor_index];
    if ((context->ultrasonic_started_mask & direction_flag) == 0U) {
        context->ultrasonic_last_success_ms[sensor_index] = now_ms;
    }
    context->ultrasonic_started_mask |= direction_flag;
}

void SensorLogic_SuspendUltrasonic(sensor_logic_context_t* context, uint32_t now_ms, sensor_logic_update_t* update) {
    const uint32_t valid_mask = SENSOR_LOGIC_VALID_ULTRASONIC_FRONT | SENSOR_LOGIC_VALID_ULTRASONIC_REAR |
                                SENSOR_LOGIC_VALID_ULTRASONIC_LEFT | SENSOR_LOGIC_VALID_ULTRASONIC_RIGHT;
    const uint32_t error_mask = SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | SENSOR_LOGIC_ERROR_ULTRASONIC_REAR |
                                SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT;
    uint8_t previous_obstacle_mask;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    previous_obstacle_mask = context->diagnostics.obstacle_mask;
    context->ultrasonic_started_mask = 0U;
    (void)memset(context->ultrasonic_failure_count, 0, sizeof(context->ultrasonic_failure_count));
    (void)memset(context->ultrasonic_recovery_count, 0, sizeof(context->ultrasonic_recovery_count));
    (void)memset(context->ultrasonic_obstacle_on_count, 0, sizeof(context->ultrasonic_obstacle_on_count));
    (void)memset(context->ultrasonic_obstacle_off_count, 0, sizeof(context->ultrasonic_obstacle_off_count));
    context->diagnostics.valid_flags &= ~valid_mask;
    SensorLogic_SetErrorFlags(context, context->diagnostics.error_flags & ~error_mask, now_ms);
    context->diagnostics.obstacle_mask = 0U;
    context->snapshot.ultrasonic_front_mm = 0U;
    context->snapshot.ultrasonic_rear_mm = 0U;
    context->snapshot.ultrasonic_left_mm = 0U;
    context->snapshot.ultrasonic_right_mm = 0U;

    if (previous_obstacle_mask != 0U) {
        context->diagnostics.obstacle_changed_at_ms = now_ms;
        update->event_flags |= APP_SENSOR_EVENT_OBSTACLE;
        update->safety_cleared_flags |= SENSOR_LOGIC_SAFETY_OBSTACLE;
    }
}

void SensorLogic_UpdateUltrasonic(sensor_logic_context_t* context, uint8_t sensor_index, uint16_t distance_mm,
                                  uint8_t valid, uint32_t now_ms, sensor_logic_update_t* update) {
    uint16_t* distance_field;
    uint8_t previous_mask;
    uint8_t direction_flag;
    uint32_t error_flags;

    if ((context == NULL) || (update == NULL) || (SensorLogic_UltrasonicIsEnabled(sensor_index) == 0U)) {
        return;
    }

    direction_flag = s_ultrasonic_direction_flags[sensor_index];
    distance_field = SensorLogic_UltrasonicDistanceField(&context->snapshot, sensor_index);
    if ((valid == 0U) || (distance_field == NULL) || (distance_mm < SENSOR_ULTRASONIC_MIN_MM) ||
        (distance_mm > SENSOR_ULTRASONIC_MAX_MM)) {
        context->ultrasonic_obstacle_on_count[sensor_index] = 0U;
        context->ultrasonic_obstacle_off_count[sensor_index] = 0U;
        context->ultrasonic_recovery_count[sensor_index] = 0U;
        if (context->ultrasonic_failure_count[sensor_index] < UINT8_MAX) {
            context->ultrasonic_failure_count[sensor_index]++;
        }
        if ((context->ultrasonic_failure_count[sensor_index] >= SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES) ||
            (SensorLogic_TimeElapsed(now_ms, context->ultrasonic_last_success_ms[sensor_index],
                                     SENSOR_ULTRASONIC_STALE_MS) != 0U)) {
            context->diagnostics.valid_flags &= ~s_ultrasonic_valid_flags[sensor_index];
            SensorLogic_SetErrorFlags(
                context, context->diagnostics.error_flags | s_ultrasonic_error_flags[sensor_index], now_ms);
        }
        return;
    }

    *distance_field = distance_mm;
    context->ultrasonic_failure_count[sensor_index] = 0U;
    context->ultrasonic_last_success_ms[sensor_index] = now_ms;
    if ((context->diagnostics.error_flags & s_ultrasonic_error_flags[sensor_index]) != 0U) {
        if (context->ultrasonic_recovery_count[sensor_index] < UINT8_MAX) {
            context->ultrasonic_recovery_count[sensor_index]++;
        }
        if (context->ultrasonic_recovery_count[sensor_index] >= SENSOR_ULTRASONIC_RECOVERY_SUCCESSES) {
            context->ultrasonic_recovery_count[sensor_index] = 0U;
            context->diagnostics.valid_flags |= s_ultrasonic_valid_flags[sensor_index];
            error_flags = context->diagnostics.error_flags & ~s_ultrasonic_error_flags[sensor_index];
            SensorLogic_SetErrorFlags(context, error_flags, now_ms);
        }
    } else {
        context->ultrasonic_recovery_count[sensor_index] = 0U;
        context->diagnostics.valid_flags |= s_ultrasonic_valid_flags[sensor_index];
    }

    previous_mask = context->diagnostics.obstacle_mask;
    if ((previous_mask & direction_flag) == 0U) {
        context->ultrasonic_obstacle_off_count[sensor_index] = 0U;
        if (distance_mm <= SENSOR_OBSTACLE_ON_MM) {
            if (context->ultrasonic_obstacle_on_count[sensor_index] < UINT8_MAX) {
                context->ultrasonic_obstacle_on_count[sensor_index]++;
            }
            if (context->ultrasonic_obstacle_on_count[sensor_index] >= SENSOR_OBSTACLE_ACTIVATE_SAMPLES) {
                context->ultrasonic_obstacle_on_count[sensor_index] = 0U;
                context->diagnostics.obstacle_mask |= direction_flag;
            }
        } else {
            context->ultrasonic_obstacle_on_count[sensor_index] = 0U;
        }
    } else {
        context->ultrasonic_obstacle_on_count[sensor_index] = 0U;
        if (distance_mm >= SENSOR_OBSTACLE_OFF_MM) {
            if (context->ultrasonic_obstacle_off_count[sensor_index] < UINT8_MAX) {
                context->ultrasonic_obstacle_off_count[sensor_index]++;
            }
            if (context->ultrasonic_obstacle_off_count[sensor_index] >= SENSOR_OBSTACLE_CLEAR_SAMPLES) {
                context->ultrasonic_obstacle_off_count[sensor_index] = 0U;
                context->diagnostics.obstacle_mask &= (uint8_t)~direction_flag;
            }
        } else {
            context->ultrasonic_obstacle_off_count[sensor_index] = 0U;
        }
    }

    if (context->diagnostics.obstacle_mask != previous_mask) {
        context->diagnostics.obstacle_changed_at_ms = now_ms;
        update->event_flags |= APP_SENSOR_EVENT_OBSTACLE;
        if ((previous_mask == 0U) && (context->diagnostics.obstacle_mask != 0U)) {
            update->safety_activated_flags |= SENSOR_LOGIC_SAFETY_OBSTACLE;
        } else if ((previous_mask != 0U) && (context->diagnostics.obstacle_mask == 0U)) {
            update->safety_cleared_flags |= SENSOR_LOGIC_SAFETY_OBSTACLE;
        }
    }
}

void SensorLogic_CheckStaleness(sensor_logic_context_t* context, uint32_t now_ms) {
    uint8_t index;
    uint32_t error_flags;

    if (context == NULL) {
        return;
    }

    error_flags = context->diagnostics.error_flags;
    if (SensorLogic_TimeElapsed(now_ms, context->last_fsr_sample_ms, SENSOR_FSR_ADC_TIMEOUT_MS) != 0U) {
        context->diagnostics.valid_flags &= ~SENSOR_LOGIC_VALID_FSR;
        context->snapshot.fsr_valid = 0U;
        error_flags |= SENSOR_LOGIC_ERROR_FSR_TIMEOUT;
    }

    for (index = 0U; index < SENSOR_LOGIC_ULTRASONIC_COUNT; index++) {
        if ((context->ultrasonic_started_mask & s_ultrasonic_direction_flags[index]) == 0U) {
            continue;
        }
        if (SensorLogic_TimeElapsed(now_ms, context->ultrasonic_last_success_ms[index], SENSOR_ULTRASONIC_STALE_MS) !=
            0U) {
            context->ultrasonic_failure_count[index] = SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES;
            context->ultrasonic_recovery_count[index] = 0U;
            context->diagnostics.valid_flags &= ~s_ultrasonic_valid_flags[index];
            error_flags |= s_ultrasonic_error_flags[index];
        }
    }

    SensorLogic_SetErrorFlags(context, error_flags, now_ms);
}

uint32_t SensorLogic_GetEffectiveSafetyErrorFlags(uint32_t raw_error_flags) {
    uint32_t effective_flags = raw_error_flags & ~((uint32_t)SENSOR_ROUTE_TEST_IGNORED_ERROR_FLAGS);

#if !SENSOR_ULTRASONIC_REAR_ENABLED
    effective_flags &= ~((uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_REAR);
#endif

#if !SENSOR_ULTRASONIC_TIMEOUT_SAFETY_FAULT
    effective_flags &= ~((uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT | (uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_REAR |
                         (uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT | (uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT);
#endif

#if SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY
    effective_flags &= ~((uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_REAR | (uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT |
                         (uint32_t)SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT);
#endif

    return effective_flags;
}

uint8_t SensorLogic_GetEffectiveSafetyObstacleMask(uint8_t raw_obstacle_mask) {
    uint8_t effective_mask = raw_obstacle_mask & (uint8_t)~((uint8_t)SENSOR_ROUTE_TEST_IGNORED_OBSTACLE_MASK);

#if !SENSOR_ULTRASONIC_REAR_ENABLED
    effective_mask &= (uint8_t)~SENSOR_LOGIC_DIRECTION_REAR;
#endif

#if SENSOR_ULTRASONIC_FRONT_SAFETY_ONLY
    effective_mask &= (uint8_t)SENSOR_LOGIC_DIRECTION_FRONT;
#endif

    return effective_mask;
}

const app_sensor_snapshot_t* SensorLogic_GetSnapshot(const sensor_logic_context_t* context) {
    return (context != NULL) ? &context->snapshot : NULL;
}

const sensor_logic_diagnostics_t* SensorLogic_GetDiagnostics(const sensor_logic_context_t* context) {
    return (context != NULL) ? &context->diagnostics : NULL;
}

uint8_t SensorLogic_GetLatestMarker(const sensor_logic_context_t* context, sensor_marker_event_t* event) {
    if ((context == NULL) || (event == NULL) || (context->marker_event_valid == 0U)) {
        return 0U;
    }

    *event = context->latest_marker_event;
    return 1U;
}

void SensorEventLatch_Init(sensor_event_latch_t* latch) {
    if (latch != NULL) {
        latch->pending_flags = APP_SENSOR_EVENT_NONE;
    }
}

uint32_t SensorEventLatch_Pend(sensor_event_latch_t* latch, uint32_t event_flags) {
    if (latch == NULL) {
        return APP_SENSOR_EVENT_NONE;
    }

    latch->pending_flags |= event_flags;
    return latch->pending_flags;
}

void SensorEventLatch_Acknowledge(sensor_event_latch_t* latch, uint32_t event_flags) {
    if (latch != NULL) {
        latch->pending_flags &= ~event_flags;
    }
}
