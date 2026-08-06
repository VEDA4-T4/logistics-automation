#include "safety_task.h"

#include <stddef.h>
#include <string.h>

#include "app_messages.h"
#include "app_queues.h"
#include "app_timing.h"
#include "cmsis_os2.h"
#include "control_task.h"
#include "main.h"
#include "motor_control.h"
#include "safety_policy.h"
#include "sensor_task.h"

extern osThreadId_t SafetyTaskHandle;

#define SAFETY_HAZARD_ESTOP (1UL << 0U)
#define SAFETY_HAZARD_LINE_LOST (1UL << 1U)
#define SAFETY_HAZARD_OBSTACLE (1UL << 2U)
#define SAFETY_HAZARD_LOAD_LOST (1UL << 3U)
#define SAFETY_HAZARD_OVERLOAD (1UL << 4U)
#define SAFETY_HAZARD_TURN_TIMEOUT (1UL << 5U)
#define SAFETY_HAZARD_MARKER_SEQUENCE (1UL << 6U)
#define SAFETY_HAZARD_COMM_TIMEOUT (1UL << 7U)
#define SAFETY_HAZARD_SENSOR_FAULT (1UL << 8U)
#define SAFETY_HAZARD_HEALTH_FAULT (1UL << 9U)
#define SAFETY_HAZARD_OBSTACLE_FRONT (1UL << 10U)
#define SAFETY_HAZARD_OBSTACLE_REAR (1UL << 11U)
#define SAFETY_HAZARD_OBSTACLE_LEFT (1UL << 12U)
#define SAFETY_HAZARD_OBSTACLE_RIGHT (1UL << 13U)
#define SAFETY_HAZARD_OBSTACLE_DIRECTIONS                                                       \
    (SAFETY_HAZARD_OBSTACLE_FRONT | SAFETY_HAZARD_OBSTACLE_REAR | SAFETY_HAZARD_OBSTACLE_LEFT | \
     SAFETY_HAZARD_OBSTACLE_RIGHT)

typedef struct {
    uint32_t active_hazard_mask;
    uint32_t latched_hazard_mask;
    linetracer_stop_reason_t latched_reason;
    uint32_t latched_at_ms;
    uint8_t error_code;
    uint8_t latched;
} safety_task_context_t;

/*
 * These file-local values intentionally remain visible in a Debug build so
 * that the SensorTask -> SafetyTask -> ControlTask contract can be inspected
 * from CubeIDE Expressions without requiring a temporary UART protocol.
 */
static safety_task_context_t s_safety_context;
static volatile app_control_safety_event_t s_latest_control_event;
static volatile uint8_t s_latest_control_event_valid;
static volatile uint32_t s_control_event_count;
static volatile uint32_t s_control_event_drop_count;
static volatile uint32_t s_control_event_deferred_count;
static volatile uint32_t s_duplicate_event_count;
static volatile uint32_t s_invalid_event_count;
static volatile uint32_t s_emergency_stop_interrupt_count;
static uint8_t s_emergency_stop_input_reported;
static uint8_t s_line_lost_sensor_active;
static uint8_t s_centered_stable_valid;
static uint8_t s_line_auto_recovery_failed;
static uint32_t s_centered_since_ms;
static uint32_t s_line_recovery_started_at_ms;
static app_control_safety_event_t s_control_event_outbox[APP_CONTROL_SAFETY_QUEUE_DEPTH];
static uint32_t s_control_event_outbox_head;
static uint32_t s_control_event_outbox_count;

static void SafetyTask_ProcessEvent(const app_safety_event_t* event);

static void SafetyTask_PublishHealthEvent(app_health_event_type_t type, uint32_t detail, uint32_t now_ms) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_SAFETY;
    (void)AppQueues_TryPutHealth(&event);
}

static uint32_t SafetyTask_EventToHazardMask(app_safety_event_type_t type) {
    switch (type) {
        case APP_SAFETY_EVENT_EMERGENCY_STOP:
            return SAFETY_HAZARD_ESTOP;

        case APP_SAFETY_EVENT_LINE_LOST:
            return SAFETY_HAZARD_LINE_LOST;

        case APP_SAFETY_EVENT_OBSTACLE:
            return SAFETY_HAZARD_OBSTACLE;

        case APP_SAFETY_EVENT_LOAD_LOST:
            return SAFETY_HAZARD_LOAD_LOST;

        case APP_SAFETY_EVENT_OVERLOAD:
            return SAFETY_HAZARD_OVERLOAD;

        case APP_SAFETY_EVENT_TURN_TIMEOUT:
            return SAFETY_HAZARD_TURN_TIMEOUT;

        case APP_SAFETY_EVENT_MARKER_SEQUENCE:
            return SAFETY_HAZARD_MARKER_SEQUENCE;

        case APP_SAFETY_EVENT_COMM_TIMEOUT:
            return SAFETY_HAZARD_COMM_TIMEOUT;

        case APP_SAFETY_EVENT_SENSOR_FAULT:
            return SAFETY_HAZARD_SENSOR_FAULT;

        case APP_SAFETY_EVENT_HEALTH_FAULT:
            return SAFETY_HAZARD_HEALTH_FAULT;

        case APP_SAFETY_EVENT_NONE:
        case APP_SAFETY_EVENT_RESET_REQUEST:
        case APP_SAFETY_EVENT_RECOVERY_REQUEST:
        default:
            return 0U;
    }
}

static linetracer_stop_reason_t SafetyTask_EventToReason(app_safety_event_type_t type) {
    switch (type) {
        case APP_SAFETY_EVENT_EMERGENCY_STOP:
            return LINETRACER_STOP_REASON_EMERGENCY;

        case APP_SAFETY_EVENT_LINE_LOST:
            return LINETRACER_STOP_REASON_LINE_LOST;

        case APP_SAFETY_EVENT_OBSTACLE:
            return LINETRACER_STOP_REASON_OBSTACLE;

        case APP_SAFETY_EVENT_LOAD_LOST:
            return LINETRACER_STOP_REASON_LOAD_LOST;

        case APP_SAFETY_EVENT_OVERLOAD:
            return LINETRACER_STOP_REASON_OVERLOAD;

        case APP_SAFETY_EVENT_TURN_TIMEOUT:
            return LINETRACER_STOP_REASON_TURN_TIMEOUT;

        case APP_SAFETY_EVENT_MARKER_SEQUENCE:
            return LINETRACER_STOP_REASON_MARKER_SEQUENCE;

        case APP_SAFETY_EVENT_COMM_TIMEOUT:
            return LINETRACER_STOP_REASON_COMM_TIMEOUT;

        case APP_SAFETY_EVENT_SENSOR_FAULT:
            return LINETRACER_STOP_REASON_SENSOR_FAULT;

        case APP_SAFETY_EVENT_HEALTH_FAULT:
            return LINETRACER_STOP_REASON_HEALTH_FAULT;

        case APP_SAFETY_EVENT_NONE:
        case APP_SAFETY_EVENT_RESET_REQUEST:
        case APP_SAFETY_EVENT_RECOVERY_REQUEST:
        default:
            return LINETRACER_STOP_REASON_NONE;
    }
}

static uint8_t SafetyTask_ReasonPriority(linetracer_stop_reason_t reason) {
    switch (reason) {
        case LINETRACER_STOP_REASON_EMERGENCY:
            return 10U;

        case LINETRACER_STOP_REASON_HEALTH_FAULT:
            return 9U;

        case LINETRACER_STOP_REASON_SENSOR_FAULT:
            return 8U;

        case LINETRACER_STOP_REASON_COMM_TIMEOUT:
            return 7U;

        case LINETRACER_STOP_REASON_TURN_TIMEOUT:
        case LINETRACER_STOP_REASON_MARKER_SEQUENCE:
            return 6U;

        case LINETRACER_STOP_REASON_LINE_LOST:
            return 5U;

        case LINETRACER_STOP_REASON_OBSTACLE:
            return 4U;

        case LINETRACER_STOP_REASON_OVERLOAD:
            return 3U;

        case LINETRACER_STOP_REASON_LOAD_LOST:
            return 2U;

        case LINETRACER_STOP_REASON_COMMAND:
            return 1U;

        case LINETRACER_STOP_REASON_NONE:
        default:
            return 0U;
    }
}

static uint8_t SafetyTask_ReasonToUartError(linetracer_stop_reason_t reason) {
    switch (reason) {
        case LINETRACER_STOP_REASON_EMERGENCY:
            return (uint8_t)UART_ERROR_EMERGENCY_STOP;

        case LINETRACER_STOP_REASON_TURN_TIMEOUT:
        case LINETRACER_STOP_REASON_COMM_TIMEOUT:
            return (uint8_t)UART_ERROR_TIMEOUT;

        case LINETRACER_STOP_REASON_MARKER_SEQUENCE:
            return (uint8_t)UART_ERROR_SEQUENCE;

        case LINETRACER_STOP_REASON_LINE_LOST:
        case LINETRACER_STOP_REASON_OBSTACLE:
        case LINETRACER_STOP_REASON_LOAD_LOST:
        case LINETRACER_STOP_REASON_OVERLOAD:
        case LINETRACER_STOP_REASON_SENSOR_FAULT:
            return (uint8_t)UART_ERROR_SENSOR;

        case LINETRACER_STOP_REASON_COMMAND:
            return (uint8_t)UART_ERROR_BUSY;

        case LINETRACER_STOP_REASON_NONE:
            return (uint8_t)UART_ERROR_NONE;

        default:
            return (uint8_t)UART_ERROR_INTERNAL;
    }
}

static uint8_t SafetyTask_HealthErrorCode(uint8_t error_code) {
    switch ((uart_error_t)error_code) {
        case UART_ERROR_BUSY:
        case UART_ERROR_TIMEOUT:
        case UART_ERROR_INTERNAL:
            return error_code;

        default:
            return (uint8_t)UART_ERROR_INTERNAL;
    }
}

static uint8_t SafetyTask_HealthErrorPriority(uint8_t error_code) {
    switch ((uart_error_t)error_code) {
        case UART_ERROR_INTERNAL:
            return 3U;

        case UART_ERROR_TIMEOUT:
            return 2U;

        case UART_ERROR_BUSY:
            return 1U;

        default:
            return 0U;
    }
}

static uint8_t SafetyTask_EventToUartError(const app_safety_event_t* event, linetracer_stop_reason_t reason) {
    if ((event != NULL) && (event->type == APP_SAFETY_EVENT_HEALTH_FAULT)) {
        return SafetyTask_HealthErrorCode(event->error_code);
    }

    return SafetyTask_ReasonToUartError(reason);
}

static void SafetyTask_StoreLatestControlEvent(const app_control_safety_event_t* event) {
    if (event == NULL) {
        return;
    }

    s_latest_control_event = *event;
    s_latest_control_event_valid = 1U;
    ++s_control_event_count;
}

static uint8_t SafetyTask_PublishControlEvent(const app_control_safety_event_t* event) {
    app_control_safety_event_t queued_event;
    uint32_t tail;

    if ((event == NULL) || (controlSafetyQueue == NULL)) {
        ++s_control_event_drop_count;
        return 0U;
    }

    queued_event = *event;
    queued_event.motor_inhibit_generation = MotorControl_GetSafetyInhibitGeneration();
    if (s_control_event_outbox_count == 0U && osMessageQueuePut(controlSafetyQueue, &queued_event, 0U, 0U) == osOK) {
        SafetyTask_StoreLatestControlEvent(&queued_event);
        return 1U;
    }

    if (s_control_event_outbox_count < APP_CONTROL_SAFETY_QUEUE_DEPTH) {
        tail = (s_control_event_outbox_head + s_control_event_outbox_count) % APP_CONTROL_SAFETY_QUEUE_DEPTH;
        s_control_event_outbox[tail] = queued_event;
        ++s_control_event_outbox_count;
        ++s_control_event_deferred_count;
        return 1U;
    }

    ++s_control_event_drop_count;
    SafetyTask_PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, (uint32_t)event->type, event->occurred_at_ms);
    return 0U;
}

static void SafetyTask_FlushControlEventOutbox(void) {
    while (s_control_event_outbox_count != 0U && controlSafetyQueue != NULL) {
        app_control_safety_event_t* event = &s_control_event_outbox[s_control_event_outbox_head];

        if (osMessageQueuePut(controlSafetyQueue, event, 0U, 0U) != osOK) {
            return;
        }

        SafetyTask_StoreLatestControlEvent(event);
        s_control_event_outbox_head = (s_control_event_outbox_head + 1U) % APP_CONTROL_SAFETY_QUEUE_DEPTH;
        --s_control_event_outbox_count;
    }
}

static void SafetyTask_CopyRequestMetadata(app_control_safety_event_t* destination, const app_safety_event_t* request) {
    if ((destination == NULL) || (request == NULL)) {
        return;
    }

    destination->original_payload_crc = request->original_payload_crc;
    destination->request_sequence = request->request_sequence;
    destination->original_command = request->original_command;
    destination->original_payload_length = request->original_payload_length;
}

static void SafetyTask_PublishLatched(const app_safety_event_t* source) {
    app_control_safety_event_t event = { 0 };

    if (source == NULL) {
        return;
    }

    event.type = APP_CONTROL_SAFETY_LATCHED;
    event.occurred_at_ms = source->occurred_at_ms;
    event.reason = s_safety_context.latched_reason;
    event.error_code = s_safety_context.error_code;
    (void)SafetyTask_PublishControlEvent(&event);
}

static void SafetyTask_PublishObstacleState(const app_safety_event_t* source, uint8_t active) {
    app_control_safety_event_t event = { 0 };

    if (source == NULL) {
        return;
    }

    event.type = (active != 0U) ? APP_CONTROL_SAFETY_OBSTACLE_ACTIVE : APP_CONTROL_SAFETY_OBSTACLE_CLEARED;
    event.occurred_at_ms = source->occurred_at_ms;
    event.reason = (active != 0U) ? LINETRACER_STOP_REASON_OBSTACLE : LINETRACER_STOP_REASON_NONE;
    event.error_code = (active != 0U) ? (uint8_t)UART_ERROR_SENSOR : (uint8_t)UART_ERROR_NONE;
    event.minimum_distance_mm = (active != 0U) ? source->minimum_distance_mm : 0U;
    event.obstacle_direction_mask = (active != 0U) ? source->obstacle_direction_mask : 0U;
    event.motor_inhibit_release_allowed =
        (active == 0U && s_safety_context.latched == 0U && s_safety_context.active_hazard_mask == 0U) ? 1U : 0U;
    (void)SafetyTask_PublishControlEvent(&event);
}

static uint8_t SafetyTask_PublishResetResult(const app_safety_event_t* request, uint8_t approved) {
    app_control_safety_event_t event = { 0 };

    if (request == NULL) {
        return 0U;
    }

    event.type = (approved != 0U) ? APP_CONTROL_SAFETY_RESET_APPROVED : APP_CONTROL_SAFETY_RESET_REJECTED;
    event.occurred_at_ms = request->occurred_at_ms;
    event.reason = (approved != 0U) ? LINETRACER_STOP_REASON_NONE : s_safety_context.latched_reason;
    event.error_code = (approved != 0U) ? (uint8_t)UART_ERROR_NONE : s_safety_context.error_code;
    if ((approved == 0U) && (event.error_code == (uint8_t)UART_ERROR_NONE)) {
        event.error_code = (uint8_t)UART_ERROR_BUSY;
    }
    SafetyTask_CopyRequestMetadata(&event, request);
    return SafetyTask_PublishControlEvent(&event);
}

static uint8_t SafetyTask_PublishRecoveryResult(const app_safety_event_t* request, uint8_t approved,
                                                uint8_t error_code) {
    app_control_safety_event_t event = { 0 };

    if (request == NULL) {
        return 0U;
    }

    event.type = (approved != 0U) ? APP_CONTROL_SAFETY_RECOVERY_APPROVED : APP_CONTROL_SAFETY_RECOVERY_REJECTED;
    event.occurred_at_ms = request->occurred_at_ms;
    event.reason = (approved != 0U) ? LINETRACER_STOP_REASON_NONE : s_safety_context.latched_reason;
    event.error_code = (approved != 0U) ? (uint8_t)UART_ERROR_NONE : error_code;
    if ((approved == 0U) && (event.error_code == (uint8_t)UART_ERROR_NONE)) {
        event.error_code = (uint8_t)UART_ERROR_BUSY;
    }
    SafetyTask_CopyRequestMetadata(&event, request);
    return SafetyTask_PublishControlEvent(&event);
}

static uint8_t SafetyTask_PublishAutoRecoveryResult(uint8_t approved, uint32_t now_ms) {
    app_control_safety_event_t event = { 0 };

    event.type = (approved != 0U) ? APP_CONTROL_SAFETY_AUTO_RECOVERY_APPROVED : APP_CONTROL_SAFETY_AUTO_RECOVERY_FAILED;
    event.occurred_at_ms = now_ms;
    event.reason = LINETRACER_STOP_REASON_LINE_LOST;
    event.error_code = (approved != 0U) ? (uint8_t)UART_ERROR_NONE : (uint8_t)UART_ERROR_SENSOR;
    return SafetyTask_PublishControlEvent(&event);
}

static void SafetyTask_ClearLatch(void) {
    (void)memset(&s_safety_context, 0, sizeof(s_safety_context));
    s_safety_context.latched_reason = LINETRACER_STOP_REASON_NONE;
    s_safety_context.error_code = (uint8_t)UART_ERROR_NONE;
}

static void SafetyTask_ActivateHazard(const app_safety_event_t* event) {
    uint32_t hazard_mask;
    linetracer_stop_reason_t reason;
    uint8_t event_error_code;
    uint8_t was_latched;

    if (event == NULL) {
        return;
    }

    hazard_mask = SafetyTask_EventToHazardMask(event->type);
    reason = SafetyTask_EventToReason(event->type);
    event_error_code = SafetyTask_EventToUartError(event, reason);
    if ((hazard_mask == 0U) || (reason == LINETRACER_STOP_REASON_NONE)) {
        ++s_invalid_event_count;
        return;
    }

    /* Stop hardware immediately; a new hazard asserts a new inhibit generation below. */
    MotorControl_ForceStop();

    if ((s_safety_context.active_hazard_mask & hazard_mask) != 0U) {
        if ((reason == LINETRACER_STOP_REASON_HEALTH_FAULT) &&
            (s_safety_context.latched_reason == LINETRACER_STOP_REASON_HEALTH_FAULT) &&
            (SafetyTask_HealthErrorPriority(event_error_code) >
             SafetyTask_HealthErrorPriority(s_safety_context.error_code))) {
            s_safety_context.error_code = event_error_code;
            SafetyTask_PublishLatched(event);
            return;
        }

        ++s_duplicate_event_count;
        return;
    }

    /*
     * This high-priority task owns the safety gate. Assert it before queueing
     * so motor shutdown does not depend on ControlTask scheduling or capacity.
     * Duplicate reports do not create artificial generations.
     */
    MotorControl_SetSafetyInhibit(1U);
    was_latched = s_safety_context.latched;
    s_safety_context.active_hazard_mask |= hazard_mask;
    s_safety_context.latched_hazard_mask |= hazard_mask;
    s_safety_context.latched = 1U;

    if ((was_latched == 0U) ||
        (SafetyTask_ReasonPriority(reason) > SafetyTask_ReasonPriority(s_safety_context.latched_reason))) {
        s_safety_context.latched_reason = reason;
        s_safety_context.error_code = event_error_code;
    }

    if (was_latched == 0U) {
        s_safety_context.latched_at_ms = event->occurred_at_ms;
    }

    SafetyTask_PublishLatched(event);
}

static void SafetyTask_DeactivateHazard(const app_safety_event_t* event) {
    uint32_t hazard_mask;

    if (event == NULL) {
        return;
    }

    hazard_mask = SafetyTask_EventToHazardMask(event->type);
    if (hazard_mask == 0U) {
        ++s_invalid_event_count;
        return;
    }

    if ((s_safety_context.active_hazard_mask & hazard_mask) == 0U) {
        ++s_duplicate_event_count;
        return;
    }

    s_safety_context.active_hazard_mask &= ~hazard_mask;
}

static void SafetyTask_ProcessObstacle(const app_safety_event_t* event) {
    uint32_t direction_hazard_mask;

    if (event == NULL) {
        return;
    }

    if (event->active != 0U) {
        if (event->obstacle_direction_mask == 0U || event->minimum_distance_mm == 0U ||
            (event->obstacle_direction_mask & (uint8_t)(~APP_OBSTACLE_DIRECTION_ALL)) != 0U) {
            ++s_invalid_event_count;
            return;
        }

        direction_hazard_mask = ((uint32_t)event->obstacle_direction_mask << 10U) & SAFETY_HAZARD_OBSTACLE_DIRECTIONS;
        s_safety_context.active_hazard_mask &= ~SAFETY_HAZARD_OBSTACLE_DIRECTIONS;
        s_safety_context.active_hazard_mask |= direction_hazard_mask;

        if ((s_safety_context.active_hazard_mask & SAFETY_HAZARD_OBSTACLE) != 0U) {
            ++s_duplicate_event_count;
            return;
        }

        /* Assert the hardware gate before relying on ControlTask scheduling. */
        MotorControl_SetSafetyInhibit(1U);
        s_safety_context.active_hazard_mask |= SAFETY_HAZARD_OBSTACLE;
        SafetyTask_PublishObstacleState(event, 1U);
        return;
    }

    if ((s_safety_context.active_hazard_mask & SAFETY_HAZARD_OBSTACLE) == 0U) {
        ++s_duplicate_event_count;
        return;
    }

    s_safety_context.active_hazard_mask &= ~(SAFETY_HAZARD_OBSTACLE | SAFETY_HAZARD_OBSTACLE_DIRECTIONS);
    SafetyTask_PublishObstacleState(event, 0U);
}

static void SafetyTask_HandleReset(const app_safety_event_t* request) {
    if (request == NULL) {
        return;
    }

    if (s_safety_context.active_hazard_mask != 0U) {
        (void)SafetyTask_PublishResetResult(request, 0U);
        return;
    }

    if (SafetyTask_PublishResetResult(request, 1U) != 0U) {
        SafetyTask_ClearLatch();
    }
}

static uint8_t SafetyTask_CenteredIsStable(uint32_t now_ms) {
    return (s_centered_stable_valid != 0U && (uint32_t)(now_ms - s_centered_since_ms) >= SAFETY_LINE_RECOVERY_STABLE_MS)
               ? 1U
               : 0U;
}

static void SafetyTask_UpdateCenteredStability(uint32_t now_ms) {
    app_sensor_snapshot_t snapshot;

    if (!SensorTask_GetLatest(&snapshot) || SafetyPolicy_SensorSupportsRecovery(&snapshot, now_ms) == 0U) {
        s_centered_stable_valid = 0U;
        return;
    }

    if (s_centered_stable_valid == 0U) {
        s_centered_since_ms = snapshot.sampled_at_ms;
        s_centered_stable_valid = 1U;
    }
}

static void SafetyTask_HandleRecovery(const app_safety_event_t* request, uint32_t now_ms) {
    app_control_snapshot_t control_snapshot;
    app_sensor_snapshot_t sensor_snapshot;
    uint8_t error_code = (uint8_t)UART_ERROR_BUSY;
    uint8_t approved = 0U;

    if (request == NULL) {
        return;
    }

    if (s_safety_context.active_hazard_mask == 0U && s_safety_context.latched == 0U &&
        ControlTask_GetLatest(&control_snapshot) && SafetyPolicy_ControlSupportsRecovery(&control_snapshot) != 0U &&
        SensorTask_GetLatest(&sensor_snapshot) && SafetyPolicy_SensorSupportsRecovery(&sensor_snapshot, now_ms) != 0U &&
        SafetyTask_CenteredIsStable(now_ms) != 0U) {
        approved = 1U;
        error_code = (uint8_t)UART_ERROR_NONE;
    } else if (s_safety_context.error_code != (uint8_t)UART_ERROR_NONE) {
        error_code = s_safety_context.error_code;
    } else if (SensorTask_GetLatest(&sensor_snapshot) &&
               SafetyPolicy_SensorSupportsRecovery(&sensor_snapshot, now_ms) == 0U) {
        error_code = (uint8_t)UART_ERROR_SENSOR;
    }

    (void)SafetyTask_PublishRecoveryResult(request, approved, error_code);
}

static uint8_t SafetyTask_LineLossApplies(void) {
    app_control_snapshot_t snapshot;

    if (ControlTask_IsTurning()) {
        return 0U;
    }

    if (!ControlTask_GetLatest(&snapshot)) {
        return 0U;
    }

    return SafetyPolicy_LineLossApplies(&snapshot);
}

static void SafetyTask_ProcessEvent(const app_safety_event_t* event) {
    app_safety_event_t momentary_clear;

    if (event == NULL) {
        return;
    }

    if (event->type == APP_SAFETY_EVENT_RESET_REQUEST) {
        SafetyTask_HandleReset(event);
        return;
    }

    if (event->type == APP_SAFETY_EVENT_RECOVERY_REQUEST) {
        SafetyTask_HandleRecovery(event, event->occurred_at_ms);
        return;
    }

    if (event->type == APP_SAFETY_EVENT_OBSTACLE) {
        SafetyTask_ProcessObstacle(event);
        return;
    }

    if (event->type == APP_SAFETY_EVENT_LINE_LOST) {
        s_line_lost_sensor_active = (event->active != 0U) ? 1U : 0U;
        if (event->active != 0U && SafetyTask_LineLossApplies() == 0U) {
            return;
        }
        if (event->active == 0U && (s_safety_context.active_hazard_mask & SAFETY_HAZARD_LINE_LOST) == 0U) {
            return;
        }
        if (event->active == 0U) {
            /*
             * SensorLogic clears its raw line-loss condition as soon as any
             * line is seen.
             * Keep the safety hazard active until SafetyTask
             * confirms CENTERED continuously for the
             * recovery interval.
             */
            return;
        }
        if ((s_safety_context.active_hazard_mask & SAFETY_HAZARD_LINE_LOST) == 0U) {
            s_line_recovery_started_at_ms = event->occurred_at_ms;
            s_line_auto_recovery_failed = 0U;
            s_centered_stable_valid = 0U;
        }
    }

    if (event->active != 0U) {
        SafetyTask_ActivateHazard(event);
        if (SafetyPolicy_IsMomentaryRemoteEstop(event) != 0U) {
            momentary_clear = *event;
            momentary_clear.active = 0U;
            SafetyTask_DeactivateHazard(&momentary_clear);
        }
    } else {
        SafetyTask_DeactivateHazard(event);
    }
}

static void SafetyTask_MonitorLineRecovery(uint32_t now_ms) {
    uint32_t recovery_elapsed_ms;
    uint8_t centered_stable;

    if ((s_safety_context.active_hazard_mask & SAFETY_HAZARD_LINE_LOST) == 0U) {
        return;
    }

    recovery_elapsed_ms = now_ms - s_line_recovery_started_at_ms;
    centered_stable = (s_line_lost_sensor_active == 0U && SafetyTask_CenteredIsStable(now_ms) != 0U) ? 1U : 0U;

    if (centered_stable != 0U) {
        uint32_t hazards_without_line = s_safety_context.active_hazard_mask & ~SAFETY_HAZARD_LINE_LOST;
        uint32_t latches_without_line = s_safety_context.latched_hazard_mask & ~SAFETY_HAZARD_LINE_LOST;

        if (s_line_auto_recovery_failed == 0U && recovery_elapsed_ms <= SAFETY_LINE_AUTO_RECOVERY_WINDOW_MS &&
            hazards_without_line == 0U && latches_without_line == 0U) {
            if (SafetyTask_PublishAutoRecoveryResult(1U, now_ms) != 0U) {
                SafetyTask_ClearLatch();
                s_line_recovery_started_at_ms = 0U;
                s_centered_stable_valid = 0U;
            }
            return;
        }

        if (s_line_auto_recovery_failed == 0U) {
            if (SafetyTask_PublishAutoRecoveryResult(0U, now_ms) == 0U) {
                return;
            }
            s_line_auto_recovery_failed = 1U;
        }
        s_safety_context.active_hazard_mask &= ~SAFETY_HAZARD_LINE_LOST;
        return;
    }

    if (s_line_auto_recovery_failed == 0U && recovery_elapsed_ms > SAFETY_LINE_AUTO_RECOVERY_WINDOW_MS &&
        SafetyTask_PublishAutoRecoveryResult(0U, now_ms) != 0U) {
        s_line_auto_recovery_failed = 1U;
    }
}

static void SafetyTask_ReconcileLineLoss(uint32_t now_ms) {
    app_safety_event_t event = { 0 };

    if (s_line_lost_sensor_active == 0U || (s_safety_context.active_hazard_mask & SAFETY_HAZARD_LINE_LOST) != 0U ||
        SafetyTask_LineLossApplies() == 0U) {
        return;
    }

    event.type = APP_SAFETY_EVENT_LINE_LOST;
    event.occurred_at_ms = now_ms;
    event.reason = LINETRACER_STOP_REASON_LINE_LOST;
    event.source_task = APP_TASK_SENSOR;
    event.error_code = (uint8_t)UART_ERROR_SENSOR;
    event.active = 1U;
    s_line_recovery_started_at_ms = now_ms;
    s_line_auto_recovery_failed = 0U;
    s_centered_stable_valid = 0U;
    SafetyTask_ActivateHazard(&event);
}

static void SafetyTask_ProcessEmergencyStopInput(uint32_t now_ms) {
    app_safety_event_t event = { 0 };
    uint32_t flags;
    uint8_t interrupt_pending;
    uint8_t hardware_active;

    flags = osThreadFlagsWait(APP_SAFETY_NOTIFY_EMERGENCY_STOP, osFlagsWaitAny, 0U);
    interrupt_pending =
        (((flags & osFlagsError) == 0U) && ((flags & APP_SAFETY_NOTIFY_EMERGENCY_STOP) != 0U)) ? 1U : 0U;
    hardware_active = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_12) == GPIO_PIN_RESET) ? 1U : 0U;

    if (((interrupt_pending != 0U) || (hardware_active != 0U)) && (s_emergency_stop_input_reported == 0U)) {
        event.type = APP_SAFETY_EVENT_EMERGENCY_STOP;
        event.occurred_at_ms = now_ms;
        event.reason = LINETRACER_STOP_REASON_EMERGENCY;
        event.source_task = APP_TASK_SAFETY;
        event.error_code = (uint8_t)UART_ERROR_EMERGENCY_STOP;
        event.active = 1U;
        SafetyTask_ProcessEvent(&event);
        s_emergency_stop_input_reported = 1U;
    }

    /*
     * PB12 is the real active-low E-Stop input. A momentary B1 test event is
     * allowed to clear its active bit immediately; the safety latch remains
     * set until an explicit RESET is approved.
     */
    if ((hardware_active == 0U) && (s_emergency_stop_input_reported != 0U)) {
        event.type = APP_SAFETY_EVENT_EMERGENCY_STOP;
        event.occurred_at_ms = now_ms;
        event.reason = LINETRACER_STOP_REASON_EMERGENCY;
        event.source_task = APP_TASK_SAFETY;
        event.error_code = (uint8_t)UART_ERROR_EMERGENCY_STOP;
        event.active = 0U;
        SafetyTask_ProcessEvent(&event);
        s_emergency_stop_input_reported = 0U;
    }
}

static void SafetyTask_ProcessQueue(void) {
    app_safety_event_t event;
    uint32_t processed = 0U;

    if (safetyEventQueue == NULL) {
        return;
    }

    while ((processed < APP_SAFETY_EVENT_QUEUE_DEPTH) &&
           (osMessageQueueGet(safetyEventQueue, &event, NULL, 0U) == osOK)) {
        SafetyTask_ProcessEvent(&event);
        ++processed;
    }
}

static void SafetyTask_Initialize(void) {
    app_control_safety_event_t empty_event = { 0 };

    SafetyTask_ClearLatch();
    s_latest_control_event = empty_event;
    s_latest_control_event_valid = 0U;
    s_control_event_count = 0U;
    s_control_event_drop_count = 0U;
    s_control_event_deferred_count = 0U;
    s_control_event_outbox_head = 0U;
    s_control_event_outbox_count = 0U;
    (void)memset(s_control_event_outbox, 0, sizeof(s_control_event_outbox));
    s_duplicate_event_count = 0U;
    s_invalid_event_count = 0U;
    s_emergency_stop_interrupt_count = 0U;
    s_emergency_stop_input_reported = 0U;
    s_line_lost_sensor_active = 0U;
    s_centered_stable_valid = 0U;
    s_line_auto_recovery_failed = 0U;
    s_centered_since_ms = 0U;
    s_line_recovery_started_at_ms = 0U;
    MotorControl_SetSafetyInhibit(0U);
    (void)osThreadFlagsClear(APP_SAFETY_NOTIFY_EMERGENCY_STOP);
}

void StartSafetyTask(void* argument) {
    uint32_t next_wake;
    uint32_t last_alive_ms;
    uint32_t now_ms;

    (void)argument;

    SafetyTask_Initialize();
    now_ms = osKernelGetTickCount();
    next_wake = now_ms;
    last_alive_ms = now_ms;

    for (;;) {
        now_ms = osKernelGetTickCount();
        SafetyTask_FlushControlEventOutbox();
        SafetyTask_ProcessEmergencyStopInput(now_ms);
        SafetyTask_UpdateCenteredStability(now_ms);
        SafetyTask_ProcessQueue();
        SafetyTask_ReconcileLineLoss(now_ms);
        SafetyTask_MonitorLineRecovery(now_ms);
        SafetyTask_FlushControlEventOutbox();

        if ((uint32_t)(now_ms - last_alive_ms) >= APP_TIMING_HEALTH_PERIOD_MS) {
            SafetyTask_PublishHealthEvent(APP_HEALTH_EVENT_TASK_ALIVE, s_safety_context.latched_hazard_mask, now_ms);
            last_alive_ms = now_ms;
        }

        next_wake += APP_TIMING_SAFETY_PERIOD_MS;
        if (osDelayUntil(next_wake) != osOK) {
            next_wake = osKernelGetTickCount();
        }
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if ((GPIO_Pin != GPIO_PIN_12) && (GPIO_Pin != B1_Pin)) {
        return;
    }

    ++s_emergency_stop_interrupt_count;
    if ((SafetyTaskHandle != NULL) && (osKernelGetState() == osKernelRunning)) {
        (void)osThreadFlagsSet(SafetyTaskHandle, APP_SAFETY_NOTIFY_EMERGENCY_STOP);
    }
}
