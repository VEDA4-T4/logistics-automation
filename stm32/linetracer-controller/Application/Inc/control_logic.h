#ifndef CONTROL_LOGIC_H
#define CONTROL_LOGIC_H

#include <stdint.h>

#include "app_messages.h"
#include "route_planner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_ROUTE_TIMEOUTS_ENABLED 0U
#define CONTROL_TURN_TIMEOUT_MS 3000U
#define CONTROL_UTURN_TIMEOUT_MS 5000U
#define CONTROL_MARKER_TIMEOUT_MS 15000U
/*
 * SensorTask already publishes debounced line inputs. A narrow transverse
 * junction stripe may be present for only one 10 ms SensorTask sample, so
 * requiring a second sample can silently miss the junction at driving speed.
 */
#define CONTROL_JUNCTION_BLACK_STABLE_MS 0U
#define CONTROL_ENDPOINT_STOP_BLACK_STABLE_MS 0U
/*
 * The three sensors are mounted ahead of the wheel axle. After the full-width
 * junction stripe is detected, drive
 * forward long enough to place the axle
 * near the intersection centre before beginning the pivot turn.
 */
#define CONTROL_JUNCTION_CENTER_ADVANCE_MS 250U
#define CONTROL_JUNCTION_CROSS_TIMEOUT_MS 1000U
#define CONTROL_TURN_SOURCE_CLEAR_MS 60U
#define CONTROL_TURN_TARGET_CENTERED_MS 50U
#define CONTROL_JUNCTION_EXIT_GUARD_MS 100U

#if ((CONTROL_ROUTE_TIMEOUTS_ENABLED != 0U) && (CONTROL_ROUTE_TIMEOUTS_ENABLED != 1U))
#error "CONTROL_ROUTE_TIMEOUTS_ENABLED must be either 0 or 1"
#endif

#if CONTROL_TURN_TIMEOUT_MS == 0U
#error "Control turn timeout must be greater than zero"
#endif

#if CONTROL_UTURN_TIMEOUT_MS <= CONTROL_TURN_TIMEOUT_MS
#error "Control U-turn timeout must be greater than the regular turn timeout"
#endif

#if CONTROL_MARKER_TIMEOUT_MS == 0U
#error "Control marker timeout must be greater than zero"
#endif

typedef enum {
    CONTROL_JUNCTION_IDLE = 0,
    CONTROL_JUNCTION_APPROACH_CENTER,
    CONTROL_JUNCTION_CROSS_STRAIGHT,
    CONTROL_JUNCTION_TURN_CLEAR_SOURCE,
    CONTROL_JUNCTION_TURN_SEARCH_TARGET
} control_junction_phase_t;

typedef struct {
    linetracer_control_state_t state;
    linetracer_control_state_t resume_state;
    linetracer_stop_reason_t stop_reason;
    uart_linetracer_position_t current_position;
    uart_linetracer_route_t active_route;
    uint32_t state_entered_at_ms;
    uint32_t last_command_at_ms;
    uint32_t marker_wait_started_at_ms;
    uint32_t last_marker_detected_at_ms;
    uint32_t junction_phase_started_at_ms;
    uint32_t junction_turn_started_at_ms;
    uint32_t junction_condition_since_ms;
    uint32_t junction_candidate_since_ms;
    uint32_t junction_guard_until_ms;
    uint32_t delayed_marker_ignore_before_ms;
    uint16_t active_job_id;
    app_marker_code_t last_marker_code;
    control_junction_phase_t junction_phase;
    route_action_t junction_action;
    uint8_t route_active;
    uint8_t resume_valid;
    uint8_t safety_latched;
    uint8_t safety_error_code;
    uint8_t last_marker_valid;
    uint8_t junction_condition_active;
    uint8_t junction_candidate_active;
    uint8_t junction_guard_active;
    uint8_t delayed_marker_ignore_valid;
    /* Set after an unload completes; the next assigned job must turn around before departure. */
    uint8_t departure_turn_required;
    /* A pickup must observe EMPTY after arrival before a new load can start the return. */
    uint8_t load_wait_armed;
    route_plan_t route_plan;
    route_action_t pending_route_action;
} control_context_t;

typedef struct {
    route_action_t action;
    uint8_t action_valid;
    uint8_t maneuver_completed;
    uint8_t state_changed;
} control_line_result_t;

typedef struct {
    uart_status_t status;
    uart_error_t error_code;
    linetracer_control_state_t previous_state;
    linetracer_control_state_t current_state;
    app_unload_command_type_t unload_command;
    uint16_t action_job_id;
    uart_linetracer_route_t action_route_id;
    uint8_t accepted;
    uint8_t state_changed;
    uint8_t status_requested;
} control_command_result_t;

typedef struct {
    uint16_t job_id;
    uart_linetracer_route_t route_id;
    uart_linetracer_position_t destination;
    uint8_t completed;
} control_job_completion_t;

void ControlLogic_Init(control_context_t* context, uint32_t now_ms);
uint8_t ControlLogic_Transition(control_context_t* context, linetracer_control_state_t next_state, uint32_t now_ms);
uint8_t ControlLogic_StateCanResume(linetracer_control_state_t state);
uint8_t ControlLogic_CommandToUartCommand(app_control_command_type_t command);
uint8_t ControlLogic_ApplySafetyEvent(control_context_t* context, const app_control_safety_event_t* event,
                                      uint32_t now_ms);
control_command_result_t ControlLogic_HandleCommand(control_context_t* context, const app_control_command_t* command,
                                                    uint32_t now_ms);
app_tx_event_type_t ControlLogic_CommandResponseEventType(const control_command_result_t* result);
void ControlLogic_MakeSnapshot(const control_context_t* context, uart_linetracer_load_state_t load_state,
                               uint32_t now_ms, app_control_snapshot_t* snapshot);
uint8_t ControlLogic_BuildStartedEvent(const control_context_t* context, const app_control_command_t* command,
                                       const control_command_result_t* result, uart_linetracer_load_state_t load_state,
                                       uint32_t now_ms, app_tx_event_t* event);
uint8_t ControlLogic_BuildStatusEvent(const control_context_t* context, const app_control_command_t* command,
                                      const control_command_result_t* result, uart_linetracer_load_state_t load_state,
                                      uint32_t now_ms, app_tx_event_t* event);
uint8_t ControlLogic_BuildSafetyFaultEvent(const control_context_t* context,
                                           const app_control_safety_event_t* safety_event,
                                           uart_linetracer_load_state_t load_state, uint32_t now_ms,
                                           app_tx_event_t* event);
uint8_t ControlLogic_IsTurning(const control_context_t* context);
uint8_t ControlLogic_UltrasonicMonitoringRequired(const control_context_t* context);
uint8_t ControlLogic_JunctionManeuverActive(const control_context_t* context);
uint8_t ControlLogic_StartPendingManeuver(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_JunctionMotorAction(const control_context_t* context);
uint8_t ControlLogic_ShouldIgnoreMarker(const control_context_t* context, app_marker_code_t marker_code,
                                        uint32_t marker_detected_at_ms, uint32_t now_ms);
control_line_result_t ControlLogic_ProcessLineSample(control_context_t* context, uint8_t line_left, uint8_t line_right,
                                                     uint32_t now_ms);
control_line_result_t ControlLogic_ProcessLineSampleWithCenter(control_context_t* context, uint8_t line_left,
                                                               uint8_t line_center, uint8_t line_right,
                                                               uint32_t now_ms);
uint8_t ControlLogic_CompleteTurn(control_context_t* context, uint32_t now_ms);
app_marker_code_t ControlLogic_ExpectedMarkerCode(const control_context_t* context);
route_action_t ControlLogic_HandleMarker(control_context_t* context, app_marker_code_t marker_code,
                                         uint32_t marker_detected_at_ms, uint32_t now_ms);
linetracer_stop_reason_t ControlLogic_CheckRouteTimeout(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_HandleLoadOn(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_HandleLoadOff(control_context_t* context, uint32_t now_ms,
                                          control_job_completion_t* completion);
control_job_completion_t ControlLogic_CompleteJob(control_context_t* context, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_LOGIC_H */
