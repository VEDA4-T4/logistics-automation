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

static uint8_t SensorLogic_TimeElapsed(uint32_t now_ms,
                                       uint32_t since_ms,
                                       uint32_t duration_ms)
{
    return ((uint32_t)(now_ms - since_ms) >= duration_ms) ? 1U : 0U;
}

static uint8_t SensorLogic_DebounceUpdate(sensor_debounce_filter_t *filter,
                                          uint8_t input)
{
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

static linetracer_line_state_t SensorLogic_CalculateLineState(uint8_t left,
                                                              uint8_t right)
{
    if ((left != 0U) && (right != 0U)) {
        return LINETRACER_LINE_CENTERED;
    }
    if ((left != 0U) && (right == 0U)) {
        return LINETRACER_LINE_LEFT_ONLY;
    }
    if ((left == 0U) && (right != 0U)) {
        return LINETRACER_LINE_RIGHT_ONLY;
    }
    return LINETRACER_LINE_WHITE_GAP;
}

static void SensorLogic_SetErrorFlags(sensor_logic_context_t *context,
                                      uint32_t error_flags,
                                      uint32_t now_ms)
{
    if (context->diagnostics.error_flags != error_flags) {
        context->diagnostics.error_flags = error_flags;
        context->diagnostics.error_changed_at_ms = now_ms;
    }
}

static app_marker_code_t SensorLogic_MarkerCodeFromCount(uint8_t count)
{
    switch (count) {
        case 1U:
            return APP_MARKER_JUNCTION;
        case 2U:
            return APP_MARKER_DEST_A;
        case 3U:
            return APP_MARKER_DEST_B;
        case 4U:
            return APP_MARKER_DEST_C;
        default:
            return APP_MARKER_INVALID;
    }
}

static void SensorLogic_ResetMarkerGroup(sensor_logic_context_t *context)
{
    context->marker_group_active = 0U;
    context->marker_group_count = 0U;
    context->marker_group_last_stripe_at_ms = 0U;
}

static void SensorLogic_PublishMarker(sensor_logic_context_t *context,
                                      sensor_marker_event_type_t type,
                                      app_marker_code_t code,
                                      uint8_t count,
                                      linetracer_line_state_t exit_state,
                                      uint32_t duration_ms,
                                      uint32_t detected_at_ms,
                                      sensor_logic_update_t *update)
{
    context->diagnostics.marker_detected_at_ms = detected_at_ms;
    context->diagnostics.marker_duration_ms =
        (duration_ms <= UINT16_MAX) ? (uint16_t)duration_ms : UINT16_MAX;
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

static void SensorLogic_AccumulateMarkerStripe(sensor_logic_context_t *context,
                                               uint32_t duration_ms,
                                               uint32_t now_ms)
{
    if (context->marker_group_count < UINT8_MAX) {
        context->marker_group_count++;
    }

    context->marker_group_active = 1U;
    context->marker_group_last_stripe_at_ms = now_ms;
    context->diagnostics.marker_duration_ms =
        (duration_ms <= UINT16_MAX) ? (uint16_t)duration_ms : UINT16_MAX;
}

static void SensorLogic_PublishInvalidMarker(sensor_logic_context_t *context,
                                             sensor_marker_event_type_t type,
                                             linetracer_line_state_t exit_state,
                                             uint32_t duration_ms,
                                             uint32_t now_ms,
                                             sensor_logic_update_t *update)
{
    uint8_t observed_count = context->marker_group_count;

    if (observed_count < UINT8_MAX) {
        observed_count++;
    }

    SensorLogic_PublishMarker(context,
                              type,
                              APP_MARKER_INVALID,
                              observed_count,
                              exit_state,
                              duration_ms,
                              now_ms,
                              update);
    SensorLogic_ResetMarkerGroup(context);
}

static void SensorLogic_FinalizeMarkerGroup(sensor_logic_context_t *context,
                                            uint32_t now_ms,
                                            sensor_logic_update_t *update)
{
    app_marker_code_t code;
    sensor_marker_event_type_t type;

    if ((context->marker_group_active == 0U) ||
        (context->line_white_active != 0U) ||
        (SensorLogic_TimeElapsed(now_ms,
                                 context->marker_group_last_stripe_at_ms,
                                 SENSOR_MARKER_GROUP_TIMEOUT_MS) == 0U)) {
        return;
    }

    code = SensorLogic_MarkerCodeFromCount(context->marker_group_count);
    type = (code == APP_MARKER_INVALID)
               ? SENSOR_MARKER_EVENT_INVALID_COUNT
               : SENSOR_MARKER_EVENT_DETECTED;

    SensorLogic_PublishMarker(context,
                              type,
                              code,
                              context->marker_group_count,
                              LINETRACER_LINE_CENTERED,
                              context->diagnostics.marker_duration_ms,
                              context->marker_group_last_stripe_at_ms,
                              update);
    SensorLogic_ResetMarkerGroup(context);
}

static void SensorLogic_UpdateMarkerRearm(sensor_logic_context_t *context,
                                          uint32_t now_ms)
{
    if ((context->marker_state != SENSOR_MARKER_CONFIRMED) ||
        (context->line_white_active != 0U)) {
        return;
    }

    if (context->snapshot.line_state != LINETRACER_LINE_CENTERED) {
        context->marker_rearm_centered = 0U;
        context->marker_state = SENSOR_MARKER_IDLE;
        return;
    }

    if (context->marker_rearm_centered == 0U) {
        context->marker_rearm_centered = 1U;
        context->marker_rearm_since_ms = now_ms;
        return;
    }

    if (SensorLogic_TimeElapsed(now_ms,
                                context->marker_rearm_since_ms,
                                SENSOR_MARKER_REARM_MS) != 0U) {
        context->marker_state = SENSOR_MARKER_IDLE;
        context->marker_rearm_centered = 0U;
    }
}

static uint16_t SensorLogic_FilterFsr(sensor_fsr_filter_t *filter,
                                      uint16_t sample)
{
    if (filter->count < SENSOR_FSR_FILTER_SAMPLES) {
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
        filter->count++;
    } else {
        filter->sum -= filter->samples[filter->next_index];
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
    }

    filter->next_index =
        (uint8_t)((filter->next_index + 1U) % SENSOR_FSR_FILTER_SAMPLES);
    return (uint16_t)(filter->sum / filter->count);
}

static uint16_t *SensorLogic_UltrasonicDistanceField(app_sensor_snapshot_t *snapshot,
                                                      uint8_t sensor_index)
{
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

void SensorLogic_Init(sensor_logic_context_t *context, uint32_t now_ms)
{
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

void SensorLogic_UpdateLine(sensor_logic_context_t *context,
                            uint8_t line_left,
                            uint8_t line_right,
                            uint32_t now_ms,
                            sensor_logic_update_t *update)
{
    uint8_t left_changed;
    uint8_t right_changed;
    linetracer_line_state_t previous_state;
    linetracer_line_state_t next_state;
    uint32_t white_duration_ms;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    SensorLogic_FinalizeMarkerGroup(context, now_ms, update);

    left_changed = SensorLogic_DebounceUpdate(&context->line_left_filter, line_left);
    right_changed = SensorLogic_DebounceUpdate(&context->line_right_filter, line_right);

    if ((context->line_left_filter.initialized == 0U) ||
        (context->line_right_filter.initialized == 0U)) {
        return;
    }

    context->diagnostics.valid_flags |= SENSOR_LOGIC_VALID_LINE;
    context->snapshot.line_left = context->line_left_filter.stable;
    context->snapshot.line_right = context->line_right_filter.stable;

    if ((left_changed != 0U) || (right_changed != 0U)) {
        previous_state = context->snapshot.line_state;
        next_state = SensorLogic_CalculateLineState(context->snapshot.line_left,
                                                    context->snapshot.line_right);

        if (next_state != previous_state) {
            context->snapshot.line_state = next_state;
            context->diagnostics.line_changed_at_ms = now_ms;
            update->event_flags |= APP_SENSOR_EVENT_LINE_CHANGED;

            if (next_state == LINETRACER_LINE_WHITE_GAP) {
                context->line_white_active = 1U;
                context->line_white_since_ms = now_ms;
                context->marker_entry_state = previous_state;
                context->marker_rearm_centered = 0U;
                context->marker_state =
                    (previous_state == LINETRACER_LINE_CENTERED)
                        ? SENSOR_MARKER_GAP_CANDIDATE
                        : SENSOR_MARKER_IDLE;
            } else if (context->line_white_active != 0U) {
                white_duration_ms = (uint32_t)(now_ms - context->line_white_since_ms);

                if (context->line_lost_active != 0U) {
                    context->line_lost_active = 0U;
                    update->safety_cleared_flags |= SENSOR_LOGIC_SAFETY_LINE_LOST;
                } else if ((context->marker_state == SENSOR_MARKER_GAP_CANDIDATE) &&
                           (next_state == LINETRACER_LINE_CENTERED) &&
                           (white_duration_ms >= SENSOR_MARKER_MIN_GAP_MS) &&
                           (white_duration_ms <= SENSOR_MARKER_MAX_GAP_MS)) {
                    SensorLogic_AccumulateMarkerStripe(context,
                                                       white_duration_ms,
                                                       now_ms);
                } else if (white_duration_ms >= SENSOR_MARKER_MIN_GAP_MS) {
                    SensorLogic_PublishInvalidMarker(
                        context,
                        (white_duration_ms > SENSOR_MARKER_MAX_GAP_MS)
                            ? SENSOR_MARKER_EVENT_INVALID_WIDTH
                            : SENSOR_MARKER_EVENT_INVALID_TRANSITION,
                        next_state,
                        white_duration_ms,
                        now_ms,
                        update);
                }

                context->line_white_active = 0U;
                context->marker_state = SENSOR_MARKER_CONFIRMED;
                context->marker_rearm_centered =
                    (next_state == LINETRACER_LINE_CENTERED) ? 1U : 0U;
                context->marker_rearm_since_ms = now_ms;
            }
        }
    }

    SensorLogic_UpdateMarkerRearm(context, now_ms);

    if ((context->line_white_active != 0U) &&
        (context->line_lost_active == 0U) &&
        (SensorLogic_TimeElapsed(now_ms,
                                 context->line_white_since_ms,
                                 SENSOR_LINE_LOST_TIMEOUT_MS) != 0U)) {
        context->line_lost_active = 1U;
        context->marker_state = SENSOR_MARKER_LINE_LOST;
        SensorLogic_ResetMarkerGroup(context);
        update->event_flags |= APP_SENSOR_EVENT_LINE_LOST;
        update->safety_activated_flags |= SENSOR_LOGIC_SAFETY_LINE_LOST;
    }
}

void SensorLogic_UpdateFsr(sensor_logic_context_t *context,
                           uint16_t raw_value,
                           uint32_t now_ms,
                           sensor_logic_update_t *update)
{
    uint8_t currently_loaded;
    uint8_t desired_loaded;
    uint8_t currently_overloaded;
    uint8_t desired_overloaded;
    uint32_t error_flags;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    context->snapshot.fsr_raw = raw_value;
    context->diagnostics.fsr_filtered = SensorLogic_FilterFsr(&context->fsr_filter, raw_value);
    context->diagnostics.valid_flags |= SENSOR_LOGIC_VALID_FSR;
    error_flags = context->diagnostics.error_flags &
                  ~(SENSOR_LOGIC_ERROR_FSR_ADC | SENSOR_LOGIC_ERROR_FSR_TIMEOUT);
    SensorLogic_SetErrorFlags(context, error_flags, now_ms);
    context->last_fsr_sample_ms = now_ms;

    currently_loaded =
        (context->snapshot.load_state == UART_LINETRACER_LOAD_PRESENT) ? 1U : 0U;
    desired_loaded = currently_loaded;

    if ((currently_loaded == 0U) &&
        (context->diagnostics.fsr_filtered >= SENSOR_FSR_LOAD_ON_THRESHOLD)) {
        desired_loaded = 1U;
    } else if ((currently_loaded != 0U) &&
               (context->diagnostics.fsr_filtered <= SENSOR_FSR_LOAD_OFF_THRESHOLD)) {
        desired_loaded = 0U;
    }

    if (desired_loaded == currently_loaded) {
        context->fsr_candidate_loaded = currently_loaded;
        context->fsr_candidate_since_ms = now_ms;
    } else if (desired_loaded != context->fsr_candidate_loaded) {
        context->fsr_candidate_loaded = desired_loaded;
        context->fsr_candidate_since_ms = now_ms;
    } else if (SensorLogic_TimeElapsed(now_ms,
                                       context->fsr_candidate_since_ms,
                                       APP_TIMING_FSR_STABLE_MS) != 0U) {
        context->snapshot.load_state =
            (desired_loaded != 0U)
                ? UART_LINETRACER_LOAD_PRESENT
                : UART_LINETRACER_LOAD_EMPTY;
        context->diagnostics.load_changed_at_ms = now_ms;
        update->event_flags |=
            (desired_loaded != 0U) ? APP_SENSOR_EVENT_LOAD_ON : APP_SENSOR_EVENT_LOAD_OFF;
        context->fsr_candidate_since_ms = now_ms;
    }

    currently_overloaded = context->diagnostics.overload_active;
    desired_overloaded = currently_overloaded;

    if ((currently_overloaded == 0U) &&
        (context->diagnostics.fsr_filtered >= SENSOR_FSR_OVERLOAD_THRESHOLD)) {
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
    } else if (SensorLogic_TimeElapsed(now_ms,
                                       context->overload_candidate_since_ms,
                                       APP_TIMING_FSR_STABLE_MS) != 0U) {
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

void SensorLogic_MarkFsrError(sensor_logic_context_t *context,
                              uint32_t error_flag,
                              uint32_t now_ms)
{
    uint32_t allowed_flags = SENSOR_LOGIC_ERROR_FSR_ADC | SENSOR_LOGIC_ERROR_FSR_TIMEOUT;

    if (context == NULL) {
        return;
    }

    context->diagnostics.valid_flags &= ~SENSOR_LOGIC_VALID_FSR;
    SensorLogic_SetErrorFlags(context,
                              context->diagnostics.error_flags | (error_flag & allowed_flags),
                              now_ms);
}

void SensorLogic_MarkAllUltrasonicUnavailable(sensor_logic_context_t *context,
                                               uint32_t now_ms)
{
    uint32_t all_valid;
    uint32_t all_errors;

    if (context == NULL) {
        return;
    }

    all_valid = SENSOR_LOGIC_VALID_ULTRASONIC_FRONT |
                SENSOR_LOGIC_VALID_ULTRASONIC_REAR |
                SENSOR_LOGIC_VALID_ULTRASONIC_LEFT |
                SENSOR_LOGIC_VALID_ULTRASONIC_RIGHT;
    all_errors = SENSOR_LOGIC_ERROR_ULTRASONIC_FRONT |
                 SENSOR_LOGIC_ERROR_ULTRASONIC_REAR |
                 SENSOR_LOGIC_ERROR_ULTRASONIC_LEFT |
                 SENSOR_LOGIC_ERROR_ULTRASONIC_RIGHT;
    context->diagnostics.valid_flags &= ~all_valid;
    SensorLogic_SetErrorFlags(context,
                              context->diagnostics.error_flags | all_errors,
                              now_ms);
}

void SensorLogic_MarkUltrasonicStarted(sensor_logic_context_t *context,
                                       uint8_t sensor_index,
                                       uint32_t now_ms)
{
    uint8_t direction_flag;

    if ((context == NULL) || (sensor_index >= SENSOR_LOGIC_ULTRASONIC_COUNT)) {
        return;
    }

    direction_flag = s_ultrasonic_direction_flags[sensor_index];
    if ((context->ultrasonic_started_mask & direction_flag) == 0U) {
        context->ultrasonic_last_success_ms[sensor_index] = now_ms;
    }
    context->ultrasonic_started_mask |= direction_flag;
}

void SensorLogic_UpdateUltrasonic(sensor_logic_context_t *context,
                                  uint8_t sensor_index,
                                  uint16_t distance_mm,
                                  uint8_t valid,
                                  uint32_t now_ms,
                                  sensor_logic_update_t *update)
{
    uint16_t *distance_field;
    uint8_t previous_mask;
    uint8_t direction_flag;
    uint32_t error_flags;

    if ((context == NULL) || (update == NULL) ||
        (sensor_index >= SENSOR_LOGIC_ULTRASONIC_COUNT)) {
        return;
    }

    direction_flag = s_ultrasonic_direction_flags[sensor_index];
    distance_field = SensorLogic_UltrasonicDistanceField(&context->snapshot, sensor_index);
    if ((valid == 0U) || (distance_field == NULL) ||
        (distance_mm < SENSOR_ULTRASONIC_MIN_MM) ||
        (distance_mm > SENSOR_ULTRASONIC_MAX_MM)) {
        context->diagnostics.valid_flags &= ~s_ultrasonic_valid_flags[sensor_index];
        if (context->ultrasonic_failure_count[sensor_index] < UINT8_MAX) {
            context->ultrasonic_failure_count[sensor_index]++;
        }
        if (context->ultrasonic_failure_count[sensor_index] >=
            SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES) {
            SensorLogic_SetErrorFlags(
                context,
                context->diagnostics.error_flags | s_ultrasonic_error_flags[sensor_index],
                now_ms);
        }
        return;
    }

    *distance_field = distance_mm;
    context->ultrasonic_failure_count[sensor_index] = 0U;
    context->ultrasonic_last_success_ms[sensor_index] = now_ms;
    context->diagnostics.valid_flags |= s_ultrasonic_valid_flags[sensor_index];
    error_flags = context->diagnostics.error_flags & ~s_ultrasonic_error_flags[sensor_index];
    SensorLogic_SetErrorFlags(context, error_flags, now_ms);

    previous_mask = context->diagnostics.obstacle_mask;
    if (((previous_mask & direction_flag) == 0U) &&
        (distance_mm <= SENSOR_OBSTACLE_ON_MM)) {
        context->diagnostics.obstacle_mask |= direction_flag;
    } else if (((previous_mask & direction_flag) != 0U) &&
               (distance_mm >= SENSOR_OBSTACLE_OFF_MM)) {
        context->diagnostics.obstacle_mask &= (uint8_t)~direction_flag;
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

void SensorLogic_CheckStaleness(sensor_logic_context_t *context,
                                uint32_t now_ms)
{
    uint8_t index;
    uint32_t error_flags;

    if (context == NULL) {
        return;
    }

    error_flags = context->diagnostics.error_flags;
    if (SensorLogic_TimeElapsed(now_ms,
                                context->last_fsr_sample_ms,
                                SENSOR_FSR_ADC_TIMEOUT_MS) != 0U) {
        context->diagnostics.valid_flags &= ~SENSOR_LOGIC_VALID_FSR;
        error_flags |= SENSOR_LOGIC_ERROR_FSR_TIMEOUT;
    }

    for (index = 0U; index < SENSOR_LOGIC_ULTRASONIC_COUNT; index++) {
        if ((context->ultrasonic_started_mask & s_ultrasonic_direction_flags[index]) == 0U) {
            continue;
        }
        if (SensorLogic_TimeElapsed(now_ms,
                                    context->ultrasonic_last_success_ms[index],
                                    SENSOR_ULTRASONIC_STALE_MS) != 0U) {
            context->diagnostics.valid_flags &= ~s_ultrasonic_valid_flags[index];
            error_flags |= s_ultrasonic_error_flags[index];
        }
    }

    SensorLogic_SetErrorFlags(context, error_flags, now_ms);
}

const app_sensor_snapshot_t *SensorLogic_GetSnapshot(const sensor_logic_context_t *context)
{
    return (context != NULL) ? &context->snapshot : NULL;
}

const sensor_logic_diagnostics_t *SensorLogic_GetDiagnostics(const sensor_logic_context_t *context)
{
    return (context != NULL) ? &context->diagnostics : NULL;
}

uint8_t SensorLogic_GetLatestMarker(const sensor_logic_context_t *context,
                                    sensor_marker_event_t *event)
{
    if ((context == NULL) || (event == NULL) || (context->marker_event_valid == 0U)) {
        return 0U;
    }

    *event = context->latest_marker_event;
    return 1U;
}

void SensorEventLatch_Init(sensor_event_latch_t *latch)
{
    if (latch != NULL) {
        latch->pending_flags = APP_SENSOR_EVENT_NONE;
    }
}

uint32_t SensorEventLatch_Pend(sensor_event_latch_t *latch,
                               uint32_t event_flags)
{
    if (latch == NULL) {
        return APP_SENSOR_EVENT_NONE;
    }

    latch->pending_flags |= event_flags;
    return latch->pending_flags;
}

void SensorEventLatch_Acknowledge(sensor_event_latch_t *latch,
                                  uint32_t event_flags)
{
    if (latch != NULL) {
        latch->pending_flags &= ~event_flags;
    }
}
