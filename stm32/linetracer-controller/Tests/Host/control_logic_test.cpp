#include "control_logic.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

void RunMotorControlLogicTests();

namespace {

app_control_command_t MakeCommand(app_control_command_type_t type, std::uint32_t received_at_ms,
                                  std::uint8_t sequence) {
    app_control_command_t command{};
    command.type = type;
    command.received_at_ms = received_at_ms;
    command.sequence = sequence;
    return command;
}

app_unload_result_t MakeUnloadComplete(std::uint16_t job_id, uart_linetracer_route_t route_id) {
    app_unload_result_t result{};
    result.type = APP_UNLOAD_RESULT_COMPLETE;
    result.job_id = job_id;
    result.route_id = route_id;
    return result;
}

route_action_t HandleExpectedMarker(control_context_t& context, std::uint32_t now_ms) {
    const auto marker_code = ControlLogic_ExpectedMarkerCode(&context);
    return ControlLogic_HandleMarker(&context, marker_code, now_ms, now_ms);
}

app_marker_code_t MarkerForRoute(uart_linetracer_route_t route_id) {
    (void)route_id;
    return APP_MARKER_JUNCTION;
}

void StartRoute(control_context_t& context, uart_linetracer_position_t position_id, uart_linetracer_route_t route_id,
                std::uint16_t job_id, std::uint32_t now_ms) {
    ControlLogic_Init(&context, now_ms);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, now_ms + 1U, 1U);
    position.position = position_id;
    assert(ControlLogic_HandleCommand(&context, &position, now_ms + 1U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, now_ms + 2U, 2U);
    assign.job_id = job_id;
    assign.route_id = route_id;
    assert(ControlLogic_HandleCommand(&context, &assign, now_ms + 2U).accepted != 0U);
}

void TestInitializationAndPosition() {
    control_context_t context{};
    ControlLogic_Init(&context, 10U);

    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
    assert(context.current_position == UART_LINETRACER_POSITION_NONE);
    assert(context.active_route == UART_LINETRACER_ROUTE_NONE);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);
    assert(context.route_active == 0U);
    assert(context.safety_latched == 0U);
    assert(context.safety_error_code == UART_ERROR_NONE);
    assert(context.route_plan.valid == 0U);
    assert(context.pending_route_action == ROUTE_ACTION_NONE);

    auto command = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 20U, 1U);
    command.position = UART_LINETRACER_POSITION_DEST_A;
    const auto result = ControlLogic_HandleCommand(&context, &command, 20U);

    assert(result.accepted != 0U);
    assert(result.state_changed != 0U);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);
    assert(context.current_position == UART_LINETRACER_POSITION_DEST_A);
}

void TestSafetyLatchRejectsDriveUntilApprovedReset() {
    control_context_t context{};
    ControlLogic_Init(&context, 100U);

    app_control_safety_event_t safety_event{};
    safety_event.type = APP_CONTROL_SAFETY_LATCHED;
    safety_event.reason = LINETRACER_STOP_REASON_EMERGENCY;
    safety_event.error_code = UART_ERROR_EMERGENCY_STOP;

    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 110U) != 0U);
    assert(context.safety_latched != 0U);
    assert(context.state == LINETRACER_CONTROL_EMERGENCY_STOPPED);
    assert(context.resume_valid == 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 120U, 1U);
    assign.job_id = 31U;
    assign.route_id = UART_LINETRACER_ROUTE_A;
    const auto assign_result = ControlLogic_HandleCommand(&context, &assign, 120U);
    assert(assign_result.accepted == 0U);
    assert(assign_result.status == UART_STATUS_NACK);
    assert(assign_result.error_code == UART_ERROR_EMERGENCY_STOP);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);

    const auto resume = MakeCommand(APP_CONTROL_COMMAND_RESUME_DRIVE, 130U, 2U);
    const auto resume_result = ControlLogic_HandleCommand(&context, &resume, 130U);
    assert(resume_result.accepted == 0U);
    assert(resume_result.status == UART_STATUS_NACK);
    assert(resume_result.error_code == UART_ERROR_EMERGENCY_STOP);

    const auto reset = MakeCommand(APP_CONTROL_COMMAND_RESET_SYSTEM, 140U, 3U);
    const auto reset_result = ControlLogic_HandleCommand(&context, &reset, 140U);
    assert(reset_result.accepted == 0U);
    assert(reset_result.status == UART_STATUS_NACK);
    assert(reset_result.error_code == UART_ERROR_EMERGENCY_STOP);

    safety_event.type = APP_CONTROL_SAFETY_RESET_REJECTED;
    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 150U) != 0U);
    assert(context.safety_latched != 0U);
    assert(context.state == LINETRACER_CONTROL_EMERGENCY_STOPPED);

    safety_event.type = APP_CONTROL_SAFETY_RESET_APPROVED;
    safety_event.reason = LINETRACER_STOP_REASON_NONE;
    safety_event.error_code = UART_ERROR_NONE;
    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 160U) != 0U);
    assert(context.safety_latched == 0U);
    assert(context.safety_error_code == UART_ERROR_NONE);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
    assert(context.current_position == UART_LINETRACER_POSITION_NONE);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);
}

void TestEmergencyRecoveryResumesActiveRoute() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 84U, 200U);
    assert(HandleExpectedMarker(context, 250U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_StartPendingManeuver(&context, 260U) != 0U);

    const auto expected_marker = context.route_plan.expected_marker;
    const auto junction_phase = context.junction_phase;
    const auto junction_action = context.junction_action;

    app_control_safety_event_t safety_event{};
    safety_event.type = APP_CONTROL_SAFETY_LATCHED;
    safety_event.reason = LINETRACER_STOP_REASON_EMERGENCY;
    safety_event.error_code = UART_ERROR_EMERGENCY_STOP;

    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 300U) != 0U);
    assert(context.state == LINETRACER_CONTROL_EMERGENCY_STOPPED);
    assert(context.resume_valid != 0U);
    assert(context.resume_state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.active_job_id == 84U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);
    assert(context.route_active != 0U);
    assert(context.route_plan.expected_marker == expected_marker);
    assert(context.junction_phase == junction_phase);
    assert(context.junction_action == junction_action);

    safety_event.type = APP_CONTROL_SAFETY_RESET_REJECTED;
    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 400U) != 0U);
    assert(context.resume_valid != 0U);

    safety_event.type = APP_CONTROL_SAFETY_RESET_APPROVED;
    safety_event.reason = LINETRACER_STOP_REASON_NONE;
    safety_event.error_code = UART_ERROR_NONE;
    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 1000U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.safety_latched == 0U);
    assert(context.safety_error_code == UART_ERROR_NONE);
    assert(context.stop_reason == LINETRACER_STOP_REASON_NONE);
    assert(context.resume_valid == 0U);
    assert(context.active_job_id == 84U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);
    assert(context.route_active != 0U);
    assert(context.route_plan.expected_marker == expected_marker);
    assert(context.junction_phase == junction_phase);
    assert(context.junction_action == junction_action);
    assert(context.junction_phase_started_at_ms == 1000U);
    assert(context.junction_turn_started_at_ms == 1000U);
    assert(context.marker_wait_started_at_ms == 1000U);
}

void TestObstacleSafetyState() {
    control_context_t context{};
    ControlLogic_Init(&context, 200U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 201U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_B;
    assert(ControlLogic_HandleCommand(&context, &position, 201U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 202U, 2U);
    assign.job_id = 55U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &assign, 202U).accepted != 0U);
    const auto moving_state = context.state;

    app_control_safety_event_t safety_event{};
    safety_event.type = APP_CONTROL_SAFETY_OBSTACLE_ACTIVE;
    safety_event.reason = LINETRACER_STOP_REASON_OBSTACLE;
    safety_event.error_code = UART_ERROR_SENSOR;

    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 210U) != 0U);
    assert(context.safety_latched == 0U);
    assert(context.state == LINETRACER_CONTROL_OBSTACLE_STOP);
    assert(context.stop_reason == LINETRACER_STOP_REASON_OBSTACLE);
    assert(context.safety_error_code == UART_ERROR_SENSOR);
    assert(context.resume_valid != 0U);
    assert(context.resume_state == moving_state);
    assert(context.route_active != 0U);
    assert(context.active_job_id == 55U);

    safety_event.type = APP_CONTROL_SAFETY_OBSTACLE_CLEARED;
    safety_event.reason = LINETRACER_STOP_REASON_NONE;
    safety_event.error_code = UART_ERROR_NONE;
    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 220U) != 0U);
    assert(context.state == moving_state);
    assert(context.safety_latched == 0U);
    assert(context.stop_reason == LINETRACER_STOP_REASON_NONE);
    assert(context.safety_error_code == UART_ERROR_NONE);
    assert(context.resume_valid == 0U);
    assert(context.route_active != 0U);
    assert(context.active_job_id == 55U);
}

void TestRouteStopAndResume() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_B;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign.job_id = 42U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    const auto assign_result = ControlLogic_HandleCommand(&context, &assign, 20U);

    assert(assign_result.accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.active_job_id == 42U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);

    auto wrong_stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 30U, 3U);
    wrong_stop.job_id = 43U;
    const auto wrong_stop_result = ControlLogic_HandleCommand(&context, &wrong_stop, 30U);
    assert(wrong_stop_result.accepted == 0U);
    assert(wrong_stop_result.error_code == UART_ERROR_INVALID_PAYLOAD);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);

    auto stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 40U, 4U);
    stop.job_id = 42U;
    const auto stop_result = ControlLogic_HandleCommand(&context, &stop, 40U);
    assert(stop_result.accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
    assert(context.resume_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.resume_valid != 0U);

    const auto repeated_stop_result = ControlLogic_HandleCommand(&context, &stop, 50U);
    assert(repeated_stop_result.accepted != 0U);
    assert(repeated_stop_result.state_changed == 0U);

    const auto resume = MakeCommand(APP_CONTROL_COMMAND_RESUME_DRIVE, 60U, 5U);
    const auto resume_result = ControlLogic_HandleCommand(&context, &resume, 60U);
    assert(resume_result.accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.resume_valid == 0U);
}

void TestInvalidStateStatusAndTimeout() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 10U, 1U);
    assign.job_id = 10U;
    assign.route_id = UART_LINETRACER_ROUTE_A;
    const auto assign_result = ControlLogic_HandleCommand(&context, &assign, 10U);
    assert(assign_result.accepted == 0U);
    assert(assign_result.status == UART_STATUS_BUSY);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);

    const auto status = MakeCommand(APP_CONTROL_COMMAND_STATUS_REQUEST, 20U, 2U);
    const auto status_result = ControlLogic_HandleCommand(&context, &status, 20U);
    assert(status_result.accepted != 0U);
    assert(status_result.status_requested != 0U);
    assert(status_result.state_changed == 0U);
    assert(ControlLogic_CommandResponseEventType(&status_result) == APP_TX_EVENT_STATUS);

    const auto stale_status = MakeCommand(APP_CONTROL_COMMAND_STATUS_REQUEST, 0U, 3U);
    const auto stale_result = ControlLogic_HandleCommand(&context, &stale_status, UART_COMMAND_TIMEOUT_MS + 1U);
    assert(stale_result.accepted == 0U);
    assert(stale_result.error_code == UART_ERROR_TIMEOUT);
    assert(ControlLogic_CommandResponseEventType(&stale_result) == APP_TX_EVENT_COMMAND_ACK);
    assert(ControlLogic_CommandResponseEventType(nullptr) == APP_TX_EVENT_NONE);

    const auto manual_unload = MakeCommand(APP_CONTROL_COMMAND_MANUAL_UNLOAD, 30U, 4U);
    const auto manual_result = ControlLogic_HandleCommand(&context, &manual_unload, 30U);
    assert(manual_result.accepted == 0U);
    assert(manual_result.error_code == UART_ERROR_UNSUPPORTED_COMMAND);
}

void TestResetSafetyAndCommandMapping() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_C;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign.job_id = 77U;
    assign.route_id = UART_LINETRACER_ROUTE_A;
    assert(ControlLogic_HandleCommand(&context, &assign, 20U).accepted != 0U);

    const auto reset = MakeCommand(APP_CONTROL_COMMAND_RESET_SYSTEM, 30U, 3U);
    const auto reset_result = ControlLogic_HandleCommand(&context, &reset, 30U);
    assert(reset_result.accepted != 0U);
    assert(reset_result.unload_command == APP_UNLOAD_COMMAND_RESET);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
    assert(context.current_position == UART_LINETRACER_POSITION_NONE);
    assert(context.active_route == UART_LINETRACER_ROUTE_NONE);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);
    assert(context.route_active == 0U);

    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_EMERGENCY_STOPPED, 40U) != 0U);
    const auto emergency_reset_result = ControlLogic_HandleCommand(&context, &reset, 40U);
    assert(emergency_reset_result.accepted == 0U);
    assert(emergency_reset_result.error_code == UART_ERROR_EMERGENCY_STOP);
    assert(context.state == LINETRACER_CONTROL_EMERGENCY_STOPPED);

    assert(ControlLogic_CommandToUartCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE) == UART_CMD_LINETRACER_ASSIGN_ROUTE);
    assert(ControlLogic_CommandToUartCommand(APP_CONTROL_COMMAND_STATUS_REQUEST) == UART_CMD_LINETRACER_STATUS_REQUEST);
}

void TestRouteTransitionRules() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_DEST, 1U) == 0U);
    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 2U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 2U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 3U, 2U);
    assign.job_id = 100U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &assign, 3U).accepted != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION, 4U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_ON_COMMON_LINE, 5U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_TO_PICKUP, 6U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_PICKUP, 7U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_PICKUP_READY, 8U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_WAITING_LOAD, 9U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_AT_PICKUP, 10U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_DEST, 11U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_UNLOADING, 12U) != 0U);
}

void TestSameRouteCase(uart_linetracer_position_t position_id, uart_linetracer_route_t route_id, std::uint16_t job_id) {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = position_id;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign.job_id = job_id;
    assign.route_id = route_id;
    assert(ControlLogic_HandleCommand(&context, &assign, 20U).accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.route_plan.valid != 0U);
    assert(context.route_plan.junctions_total == 0U);

    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);
    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    std::uint32_t pickup_marker_ms = 50U;
    if (route_id == UART_LINETRACER_ROUTE_C) {
        assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
        assert(HandleExpectedMarker(context, 50U) == ROUTE_ACTION_TURN_LEFT);
        assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
        assert(ControlLogic_CompleteTurn(&context, 55U) != 0U);
        pickup_marker_ms = 60U;
    }
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(HandleExpectedMarker(context, pickup_marker_ms) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestSameRoutesSkipCommonLine() {
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 101U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 102U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 103U);
}

void TestMarkerValidationAndDuplicateSuppression() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 301U, 0U);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_JUNCTION, 100U, 100U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);
    const auto expected_after_first = context.route_plan.expected_marker;
    const auto state_after_first = context.state;

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_JUNCTION, 100U, 110U) == ROUTE_ACTION_NONE);
    assert(context.route_plan.expected_marker == expected_after_first);
    assert(context.state == state_after_first);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_JUNCTION, 500U, 500U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_DEST_C, 900U, 900U) == ROUTE_ACTION_ERROR);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 302U, 1000U);
    assert(ControlLogic_HandleMarker(&context, APP_MARKER_INVALID, 1100U, 1100U) == ROUTE_ACTION_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
}

void TestUnclassifiedMarkerIsRejected() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 303U, 0U);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_NONE, 100U, 100U) == ROUTE_ACTION_ERROR);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
}

void TestRouteTimeouts() {
    control_context_t context{};

#if CONTROL_ROUTE_TIMEOUTS_ENABLED == 0U
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 401U, 0U);
    assert(ControlLogic_CheckRouteTimeout(&context, CONTROL_MARKER_TIMEOUT_MS + 60000U) == LINETRACER_STOP_REASON_NONE);
    assert(context.state != LINETRACER_CONTROL_ERROR);

    assert(HandleExpectedMarker(context, CONTROL_MARKER_TIMEOUT_MS + 60100U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CheckRouteTimeout(&context, CONTROL_MARKER_TIMEOUT_MS + CONTROL_TURN_TIMEOUT_MS + 120000U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(context.state != LINETRACER_CONTROL_ERROR);

    context.state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
    context.state_entered_at_ms = 200000U;
    assert(ControlLogic_CheckRouteTimeout(&context, 200000U + CONTROL_UTURN_TIMEOUT_MS + 120000U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(context.state != LINETRACER_CONTROL_ERROR);
    return;
#endif

    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 401U, 0U);

    assert(ControlLogic_CheckRouteTimeout(&context, 2U + CONTROL_MARKER_TIMEOUT_MS - 1U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 2U + CONTROL_MARKER_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_MARKER_SEQUENCE);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 402U, 5000U);
    assert(HandleExpectedMarker(context, 5200U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.pending_route_action == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CheckRouteTimeout(&context, 5200U + CONTROL_TURN_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_TURN_TIMEOUT);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 404U, 20000U);
    context.state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
    context.state_entered_at_ms = 20100U;
    assert(ControlLogic_CheckRouteTimeout(&context, 20100U + CONTROL_TURN_TIMEOUT_MS) == LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 20100U + CONTROL_UTURN_TIMEOUT_MS - 1U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 20100U + CONTROL_UTURN_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_TURN_TIMEOUT);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, 403U, 10000U);
    assert(ControlLogic_CheckRouteTimeout(&context, 10002U + CONTROL_MARKER_TIMEOUT_MS - 1U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 10002U + CONTROL_MARKER_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_MARKER_SEQUENCE);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
}

void TestTurningStateDetection() {
    control_context_t context{};

    assert(ControlLogic_IsTurning(nullptr) == 0U);
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 404U, 0U);
    assert(ControlLogic_IsTurning(&context) == 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 200U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.pending_route_action == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_IsTurning(&context) != 0U);

    assert(ControlLogic_CompleteTurn(&context, 300U) != 0U);
    assert(ControlLogic_IsTurning(&context) == 0U);
}

void TestTurnAroundActivityDetection() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);
    assert(ControlLogic_IsTurnAroundActive(&context) == 0U);

    context.state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
    assert(ControlLogic_IsTurnAroundActive(&context) != 0U);

    context.junction_phase = CONTROL_JUNCTION_TURN_SEARCH_TARGET;
    context.junction_action = ROUTE_ACTION_TURN_AROUND;
    assert(ControlLogic_IsTurnAroundActive(&context) != 0U);

    context.state = LINETRACER_CONTROL_OBSTACLE_STOP;
    context.resume_valid = 1U;
    context.resume_state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
    context.junction_phase = CONTROL_JUNCTION_IDLE;
    context.junction_action = ROUTE_ACTION_NONE;
    assert(ControlLogic_IsTurnAroundActive(&context) != 0U);

    context.state = LINETRACER_CONTROL_MOVING_TO_DEST;
    context.resume_valid = 0U;
    context.pending_route_action = ROUTE_ACTION_GO_STRAIGHT;
    assert(ControlLogic_IsTurnAroundActive(&context) == 0U);

    context.state = LINETRACER_CONTROL_TURNING_TO_PICKUP;
    context.pending_route_action = ROUTE_ACTION_TURN_LEFT;
    context.junction_phase = CONTROL_JUNCTION_TURN_SEARCH_TARGET;
    context.junction_action = ROUTE_ACTION_TURN_LEFT;
    assert(ControlLogic_IsTurnAroundActive(&context) == 0U);
}

void AdvanceToTargetSearch(control_context_t& context, route_action_t action, std::uint32_t started_at_ms) {
    context.junction_phase = CONTROL_JUNCTION_TURN_CLEAR_SOURCE;
    context.junction_action = action;
    context.junction_turn_started_at_ms = started_at_ms;
    context.junction_phase_started_at_ms = started_at_ms;

    /* The center sensor still seeing the source line must not arm target search. */
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, started_at_ms);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, started_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* Every pivot direction requires a stable 000 source-line gap. */
    const auto clear_at = started_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS + 10U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, clear_at);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, clear_at + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);
}

void TestJunctionAcceptsOneDebouncedOuterBlackSample() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 405U, 0U);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);

    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 100U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_TURN_RIGHT);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);
}

void TestExpectedMarkerEdgeStartsLimitedPidWindow() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 428U, 0U);

    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, 100U);
    assert(result.action_valid == 0U);
    assert(ControlLogic_MarkerApproachHoldActive(&context) != 0U);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, 110U);
    assert(result.action_valid == 0U);
    assert(ControlLogic_MarkerApproachHoldActive(&context) != 0U);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 119U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_GO_STRAIGHT);
    assert(ControlLogic_MarkerApproachHoldActive(&context) == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);
}

void TestPersistentSingleOuterHitReturnsControlToPid() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 429U, 0U);

    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, 100U);
    assert(ControlLogic_MarkerApproachHoldActive(&context) != 0U);

    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, 100U + CONTROL_MARKER_APPROACH_HOLD_MS);
    assert(ControlLogic_MarkerApproachHoldActive(&context) == 0U);
    assert(context.marker_approach_state == CONTROL_MARKER_APPROACH_REJECTED);

    /* Do not repeatedly restart the hold while the same genuine line-error pattern persists. */
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, 110U + CONTROL_MARKER_APPROACH_HOLD_MS);
    assert(ControlLogic_MarkerApproachHoldActive(&context) == 0U);

    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, 120U + CONTROL_MARKER_APPROACH_HOLD_MS);
    assert(context.marker_approach_state == CONTROL_MARKER_APPROACH_IDLE);
}

void TestRouteBLeftTurnUsesFirstDetectedTargetLine() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 417U, 0U);

    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CompleteTurn(&context, 40U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.route_plan.expected_marker == ROUTE_MARKER_COMMON_JUNCTION);
    assert(context.route_plan.junctions_remaining == 1U);

    const auto after_guard = 40U + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, after_guard);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_TURN_LEFT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);

    const auto pivot_started_at = after_guard + CONTROL_JUNCTION_CENTER_ADVANCE_MS;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, pivot_started_at);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    const auto source_clear_at = pivot_started_at + 1U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, source_clear_at);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U,
                                                   source_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    const auto target_black_at = source_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS + 1U;
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, target_black_at);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(context.junction_guard_active != 0U);
}

void TestOtherLeftTurnsDoNotSkipFirstTargetLine() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, 418U, 0U);

    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 100U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_TURN_LEFT);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
}

void TestStopMarkerUsesSameOuterPairAsTurnMarker() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 415U, 0U);

    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    /* Turn and stop markers both use the same-sample outer DO pair; center may remain white. */
    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 50U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestStopMarkerRejectsSeparatedOuterHits() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 422U, 0U);

    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, 100U);
    assert(result.action_valid == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, 110U);
    assert(result.action_valid == 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
}

void TestStopMarkerUsesSameExitGuardAsTurnMarker() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 416U, 0U);

    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    /* Stop markers obey the same post-maneuver guard used by turn markers. */
    context.junction_guard_active = 1U;
    context.junction_guard_until_ms = 200U;

    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, 60U, 60U) != 0U);
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 60U);
    assert(result.action_valid == 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 201U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestDelayedOneStripeMarkerFallbackIsNotDropped() {
    control_context_t turn_context{};
    StartRoute(turn_context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 419U, 0U);

    /* If the immediate 101/111 sample is missed, the finalized one-stripe event must still start the turn. */
    assert(ControlLogic_ShouldIgnoreMarker(&turn_context, APP_MARKER_JUNCTION, 100U, 451U) == 0U);
    assert(ControlLogic_HandleMarker(&turn_context, APP_MARKER_JUNCTION, 100U, 451U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_StartPendingManeuver(&turn_context, 451U) != 0U);
    assert(turn_context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    control_context_t stop_context{};
    StartRoute(stop_context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 420U, 0U);
    assert(HandleExpectedMarker(stop_context, 50U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(stop_context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    /* The same fallback must stop at an endpoint; all route markers intentionally use one stripe. */
    assert(ControlLogic_ShouldIgnoreMarker(&stop_context, APP_MARKER_JUNCTION, 100U, 451U) == 0U);
    assert(ControlLogic_HandleMarker(&stop_context, APP_MARKER_JUNCTION, 100U, 451U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(stop_context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestCToBDelayedRightMarkerRetainsCenterAdvance() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_B, 430U, 0U);

    assert(HandleExpectedMarker(context, 100U) == ROUTE_ACTION_TURN_LEFT);
    assert(ControlLogic_CompleteTurn(&context, 200U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);

    /* Simulate the delayed SensorTask marker-event fallback at B's right turn. */
    assert(ControlLogic_HandleMarker(&context, APP_MARKER_JUNCTION, 500U, 500U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(ControlLogic_StartPendingManeuver(&context, 500U) != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);

    auto result =
        ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 500U + CONTROL_JUNCTION_CENTER_ADVANCE_MS - 1U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 500U + CONTROL_JUNCTION_CENTER_ADVANCE_MS);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    /* The time budget alone cannot start a pivot while a slow vehicle is still on the marker. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, 501U + CONTROL_JUNCTION_CENTER_ADVANCE_MS);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
}

void TestRightTurnHandsTargetEdgeToNormalPidImmediately() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 406U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_RIGHT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* A left-side edge is not the target side of a clockwise right turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, search_at + 10U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    /* A right-side edge stops the pivot and hands this sample directly to normal PID. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, search_at + 20U);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.junction_guard_active != 0U);
}

void TestCenteredSampleCannotCompleteTurnWithoutDirectionalEdge() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 416U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_RIGHT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* 010 alone may still be the old intersection; it cannot complete a right turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, search_at + 10U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, search_at + 20U);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.junction_guard_active != 0U);
}

void TestTurnAroundHandsFirstLeftOrCenterTargetToPid() {
    const std::array<std::array<std::uint8_t, 3>, 2> target_samples = { {
        { 1U, 0U, 0U },
        { 0U, 1U, 0U },
    } };

    for (std::size_t index = 0U; index < target_samples.size(); ++index) {
        control_context_t context{};
        StartRoute(context, UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B,
                   static_cast<std::uint32_t>(409U + index), 0U);
        context.state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
        context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
        AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_AROUND, 100U);
        const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;
        assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, search_at, search_at) != 0U);

        /* 000 and the old right-side edge cannot finish the U-turn. */
        auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, search_at + 40U);
        assert(result.maneuver_completed == 0U);
        result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, search_at + 50U);
        assert(result.maneuver_completed == 0U);
        assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

        /* The first 100 or 010 sample stops pivoting and is handed directly to normal PID. */
        const auto completed_at = search_at + 60U;
        result = ControlLogic_ProcessLineSampleWithCenter(&context, target_samples[index][0], target_samples[index][1],
                                                          target_samples[index][2], completed_at);
        assert(result.maneuver_completed != 0U);
        assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
        assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);

        const auto after_guard = completed_at + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
        assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, search_at, after_guard) != 0U);
        assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, completed_at + 1U, after_guard) == 0U);
    }
}

void TestPickupTurnAroundRejectsReversePhaseLineSamples() {
    control_context_t context{};
    constexpr std::uint32_t kLoadDetectedAtMs = 100U;
    constexpr std::uint32_t kTurnStartedAtMs = kLoadDetectedAtMs + CONTROL_PICKUP_REVERSE_MS;

    ControlLogic_Init(&context, 0U);
    context.state = LINETRACER_CONTROL_WAITING_LOAD;
    context.current_position = UART_LINETRACER_POSITION_DEST_A;
    context.active_route = UART_LINETRACER_ROUTE_A;
    context.active_job_id = 418U;
    context.route_active = 1U;
    assert(RoutePlanner_Create(context.current_position, context.active_route, &context.route_plan) != 0U);
    context.route_plan.phase = ROUTE_PHASE_WAITING_LOAD;
    context.route_plan.loaded = 0U;

    assert(ControlLogic_HandleLoadOn(&context, kLoadDetectedAtMs) == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_StartPendingManeuver(&context, kLoadDetectedAtMs) != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_PICKUP_REVERSE);
    assert(ControlLogic_UpdateTimedManeuver(&context, kTurnStartedAtMs) != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* Reverse-phase 000 and 100 samples cannot advance or complete the U-turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, kTurnStartedAtMs - 20U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, kTurnStartedAtMs - 10U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* Only post-reverse 000 samples establish the common source-line gap. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, kTurnStartedAtMs);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    result =
        ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, kTurnStartedAtMs + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U,
                                                      kTurnStartedAtMs + CONTROL_TURN_SOURCE_CLEAR_MS + 10U);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
}

void TestLeftTurnHandsTargetEdgeToNormalPidImmediately() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, 407U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_LEFT);
    const auto original_state = context.state;
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_LEFT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* A right-side edge is not the target side of a counter-clockwise turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, search_at + 10U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    /* 100 stops the pivot and is immediately handed to normal PID. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, search_at + 20U);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(result.state_changed == 0U);
    assert(context.state == original_state);
    assert(context.junction_guard_active != 0U);
}

void TestTurnTargetEdgeDoesNotWaitForCenterHit() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, 428U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_LEFT);
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_LEFT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* 100 acquires the new left branch even if the centre sensor never turns black. */
    const auto target_edge_at = search_at + 20U;
    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, target_edge_at);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.junction_guard_active != 0U);
}

void TestStraightJunctionCrossingAndGuard() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 408U, 0U);

    /* A→C turns onto the common line first; its next crossing is passed straight through. */
    assert(HandleExpectedMarker(context, 50U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CompleteTurn(&context, 60U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);

    const auto detected = ControlLogic_ProcessLineSample(&context, 1U, 1U, 100U);
    assert(detected.action_valid != 0U);
    assert(detected.action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);

    auto completed = ControlLogic_ProcessLineSample(&context, 0U, 0U, 200U);
    assert(completed.maneuver_completed == 0U);
    completed = ControlLogic_ProcessLineSample(&context, 0U, 0U, 200U + CONTROL_JUNCTION_CROSS_CLEAR_MS);
    assert(completed.maneuver_completed != 0U);
    const auto completed_at_ms = 200U + CONTROL_JUNCTION_CROSS_CLEAR_MS;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, 100U, completed_at_ms + 1U) != 0U);

    const auto after_guard_ms = completed_at_ms + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, 100U, after_guard_ms) != 0U);
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, completed_at_ms + 1U, after_guard_ms) == 0U);
}

void TestRouteCSecondMarkerIsAcceptedImmediatelyAfterFirstCrossingClears() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 425U, 0U);

    const auto first_marker = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 100U);
    assert(first_marker.action_valid != 0U);
    assert(first_marker.action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.route_plan.phase == ROUTE_PHASE_TO_C_PICKUP_TURN);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);

    /* Leave the first stripe on its edge so PID can resume immediately. */
    auto cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, 101U);
    assert(cleared.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.c_marker_rearm_pending != 0U);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_NONE);

    /* A repeated 111 is still the first physical marker and must not turn. */
    cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 102U);
    assert(cleared.maneuver_completed == 0U);
    assert(cleared.action_valid == 0U);
    assert(context.c_marker_rearm_pending != 0U);
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, 102U, 102U) != 0U);

    const auto clear_started_at_ms = 103U;
    cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, clear_started_at_ms);
    assert(cleared.maneuver_completed == 0U);
    assert(context.c_marker_rearm_pending != 0U);
    cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U,
                                                       clear_started_at_ms + CONTROL_C_JUNCTION_CROSS_CLEAR_MS - 1U);
    assert(cleared.maneuver_completed == 0U);
    assert(context.c_marker_rearm_pending != 0U);

    const auto completed_at_ms = clear_started_at_ms + CONTROL_C_JUNCTION_CROSS_CLEAR_MS;
    cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, completed_at_ms);
    assert(cleared.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.junction_guard_active == 0U);
    assert(context.c_marker_rearm_pending == 0U);

    /* A delayed event from the first stripe is still rejected by its detection timestamp. */
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, 100U, completed_at_ms + 1U) != 0U);

    /* Its first one-sided edge limits PID correction until the full-width marker is visible. */
    const auto second_edge = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, completed_at_ms + 1U);
    assert(second_edge.action_valid == 0U);
    assert(ControlLogic_MarkerApproachHoldActive(&context) != 0U);

    /* The nearby second live marker must be accepted without waiting another fixed guard interval. */
    const auto second_marker = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, completed_at_ms + 2U);
    assert(second_marker.action_valid != 0U);
    assert(second_marker.action == ROUTE_ACTION_TURN_LEFT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);
    assert(ControlLogic_MarkerApproachHoldActive(&context) == 0U);

    /* The interval is a minimum; the pivot waits until the source marker has also cleared. */
    const auto pivot_started_at_ms = completed_at_ms + 2U + CONTROL_JUNCTION_CENTER_ADVANCE_MS;
    auto turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, pivot_started_at_ms - 1U);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, pivot_started_at_ms);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    /* The stripe that triggered the turn is the source marker and must not complete the turn. */
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, pivot_started_at_ms + 1U);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* Stable 000 clears the source marker; 100 hands the new line directly to normal PID. */
    const auto source_clear_at_ms = pivot_started_at_ms + 2U;
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, source_clear_at_ms);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U,
                                                       source_clear_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    const auto target_edge_at_ms = source_clear_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS + 1U;
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, target_edge_at_ms);
    assert(turning.maneuver_completed != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.junction_guard_active != 0U);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_NONE);
}

void TestRouteCReturnTurnRequiresStableSourceClear() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 426U, 0U);

    assert(HandleExpectedMarker(context, 10U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 20U) == ROUTE_ACTION_TURN_LEFT);
    assert(ControlLogic_CompleteTurn(&context, 30U) != 0U);
    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(ControlLogic_HandleLoadOn(&context, 50U) == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_CompleteTurn(&context, 60U) != 0U);
    assert(context.route_plan.phase == ROUTE_PHASE_TO_C_RETURN_JUNCTION);

    const auto return_marker = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 70U);
    assert(return_marker.action_valid != 0U);
    assert(return_marker.action == ROUTE_ACTION_TURN_RIGHT);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);

    const auto pivot_started_at_ms = 70U + CONTROL_JUNCTION_CENTER_ADVANCE_MS;
    auto turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, pivot_started_at_ms - 1U);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, pivot_started_at_ms);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, pivot_started_at_ms + 1U);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    const auto source_clear_at_ms = pivot_started_at_ms + 2U;
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, source_clear_at_ms);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U,
                                                       source_clear_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U,
                                                       source_clear_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS + 1U);
    assert(turning.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.junction_guard_active != 0U);
}

void TestRouteCToBUsesCommonImmediateReacquisition() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_B, 427U, 0U);

    auto turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 100U);
    assert(turning.action_valid != 0U);
    assert(turning.action == ROUTE_ACTION_TURN_LEFT);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    const auto first_pivot_at = 100U + CONTROL_JUNCTION_CENTER_ADVANCE_MS;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, first_pivot_at);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    const auto first_clear_at = first_pivot_at + 1U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, first_clear_at);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, first_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);
    const auto first_target_edge_at = first_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS + 1U;
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, first_target_edge_at);
    assert(turning.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.junction_guard_active != 0U);

    const auto target_marker_at = first_target_edge_at + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, target_marker_at);
    assert(turning.action_valid != 0U);
    assert(turning.action == ROUTE_ACTION_TURN_RIGHT);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);

    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U,
                                                   target_marker_at + CONTROL_JUNCTION_CENTER_ADVANCE_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    const auto return_clear_at = target_marker_at + CONTROL_JUNCTION_CENTER_ADVANCE_MS + 1U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, return_clear_at);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* A brief 000 followed by the source marker's right edge must not finish the turn. */
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, return_clear_at + 1U);
    assert(turning.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    const auto stable_clear_at = return_clear_at + 2U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, stable_clear_at);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U,
                                                   stable_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);
    turning = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U,
                                                       stable_clear_at + CONTROL_TURN_SOURCE_CLEAR_MS + 1U);
    assert(turning.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(context.junction_guard_active != 0U);
}

void TestStraightJunctionClearsDuringSingleOuterCorrection() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 421U, 0U);

    const auto detected = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 100U);
    assert(detected.action_valid != 0U);
    assert(detected.action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);

    /* 100 is a valid post-marker PID correction and must still clear the crossing. */
    auto cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, 110U);
    assert(cleared.maneuver_completed == 0U);
    cleared = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, 110U + CONTROL_JUNCTION_CROSS_CLEAR_MS);
    assert(cleared.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);

    /* Stop and turn markers are both accepted after the common exit guard. */
    const auto stopped = ControlLogic_ProcessLineSampleWithCenter(
        &context, 1U, 0U, 1U, 110U + CONTROL_JUNCTION_CROSS_CLEAR_MS + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U);
    assert(stopped.action_valid != 0U);
    assert(stopped.action == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestStopMarkerWaitsForSameCrossingAndGuardAsTurnMarker() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 424U, 0U);

    const auto detected = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 100U);
    assert(detected.action_valid != 0U);
    assert(detected.action == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);

    /* A second full-width sample cannot become a stop marker while the crossing maneuver is active. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, 110U);
    assert(result.action_valid == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_CROSS_STRAIGHT);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 120U);
    assert(result.action_valid == 0U);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, 130U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, 130U + CONTROL_JUNCTION_CROSS_CLEAR_MS);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, 200U);
    assert(result.action_valid == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(
        &context, 1U, 1U, 1U, 130U + CONTROL_JUNCTION_CROSS_CLEAR_MS + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestDestinationStopUsesSameOuterPairAsTurnMarker() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 426U, 0U);

    assert(HandleExpectedMarker(context, 20U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(ControlLogic_HandleLoadOn(&context, 40U) == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_CompleteTurn(&context, 50U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);

    assert(HandleExpectedMarker(context, 60U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.route_plan.expected_marker == ROUTE_MARKER_DEST);

    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 70U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_STOP_AT_DEST);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);
}

void TestJobCompletionAllowsNextAssignment() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto first_assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    first_assign.job_id = 102U;
    first_assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &first_assign, 20U).accepted != 0U);

    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION, 30U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_ON_COMMON_LINE, 40U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_TO_PICKUP, 50U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_PICKUP, 60U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_PICKUP_READY, 70U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_WAITING_LOAD, 80U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_AT_PICKUP, 90U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_DEST, 100U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_UNLOADING, 110U) != 0U);

    const auto stale_result = MakeUnloadComplete(999U, UART_LINETRACER_ROUTE_C);
    assert(ControlLogic_HandleUnloadResult(&context, &stale_result, 115U).completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    auto timeout_result = MakeUnloadComplete(102U, UART_LINETRACER_ROUTE_C);
    timeout_result.type = APP_UNLOAD_RESULT_TIMEOUT;
    assert(ControlLogic_HandleUnloadResult(&context, &timeout_result, 116U).completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    const auto unload_result = MakeUnloadComplete(102U, UART_LINETRACER_ROUTE_C);
    const auto completion = ControlLogic_HandleUnloadResult(&context, &unload_result, 120U);
    assert(completion.completed != 0U);
    assert(completion.job_id == 102U);
    assert(completion.route_id == UART_LINETRACER_ROUTE_C);
    assert(completion.destination == UART_LINETRACER_POSITION_DEST_C);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);
    assert(context.current_position == UART_LINETRACER_POSITION_DEST_C);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);
    assert(context.active_route == UART_LINETRACER_ROUTE_NONE);
    assert(context.route_active == 0U);
    assert(context.resume_valid == 0U);

    auto second_assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 130U, 3U);
    second_assign.job_id = 103U;
    second_assign.route_id = UART_LINETRACER_ROUTE_A;
    assert(ControlLogic_HandleCommand(&context, &second_assign, 130U).accepted != 0U);
    assert(context.active_job_id == 103U);
    assert(context.active_route == UART_LINETRACER_ROUTE_A);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.pending_route_action == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_StartPendingManeuver(&context, 130U) != 0U);
    assert(ControlLogic_CompleteTurn(&context, 140U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
}

void TestCompletionOutsideUnloadingDoesNothing() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    const auto unload_result = MakeUnloadComplete(1U, UART_LINETRACER_ROUTE_A);
    const auto completion = ControlLogic_HandleUnloadResult(&context, &unload_result, 10U);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
}

void TestStopDuringUnloadRequestsAbort() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 1U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 1U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 2U, 2U);
    assign.job_id = 301U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &assign, 2U).accepted != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION, 3U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_ON_COMMON_LINE, 4U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_TO_PICKUP, 5U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_PICKUP, 6U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_PICKUP_READY, 7U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_WAITING_LOAD, 8U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_TURNING_AT_PICKUP, 9U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_DEST, 10U) != 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_UNLOADING, 11U) != 0U);

    auto stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 12U, 3U);
    stop.job_id = 301U;
    const auto result = ControlLogic_HandleCommand(&context, &stop, 12U);
    assert(result.accepted != 0U);
    assert(result.unload_command == APP_UNLOAD_COMMAND_ABORT);
    assert(result.action_job_id == 301U);
    assert(result.action_route_id == UART_LINETRACER_ROUTE_C);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
    assert(context.resume_valid == 0U);

    const auto resume = MakeCommand(APP_CONTROL_COMMAND_RESUME_DRIVE, 13U, 4U);
    const auto resume_result = ControlLogic_HandleCommand(&context, &resume, 13U);
    assert(resume_result.accepted == 0U);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
}

void TestMarkerAndLoadEventsDriveRouteB() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign.job_id = 201U;
    assign.route_id = UART_LINETRACER_ROUTE_B;
    assert(ControlLogic_HandleCommand(&context, &assign, 20U).accepted != 0U);
    assert(context.route_plan.common_direction == ROUTE_DIRECTION_RIGHT);
    assert(context.route_plan.junctions_remaining == 1U);

    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(HandleExpectedMarker(context, 50U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);

    assert(HandleExpectedMarker(context, 60U) == ROUTE_ACTION_TURN_LEFT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(ControlLogic_CompleteTurn(&context, 70U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    assert(HandleExpectedMarker(context, 80U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
    assert(ControlLogic_HandleLoadOn(&context, 90U) == ROUTE_ACTION_TURN_AROUND);
    assert(context.state == LINETRACER_CONTROL_TURNING_AT_PICKUP);
    assert(ControlLogic_CompleteTurn(&context, 100U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);

    assert(HandleExpectedMarker(context, 120U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 130U) == ROUTE_ACTION_STOP_AT_DEST);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    const auto route_target = context.route_plan.target_index;
    assert(ControlLogic_HandleMarker(&context, APP_MARKER_DEST_B, 131U, 131U) == ROUTE_ACTION_NONE);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);
    assert(context.stop_reason == LINETRACER_STOP_REASON_NONE);
    assert(context.route_plan.target_index == route_target);

    control_job_completion_t completion{};
    assert(ControlLogic_HandleLoadOff(&context, 140U, &completion) == ROUTE_ACTION_NONE);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);
    assert(context.active_job_id == 201U);

    const auto unload_result = MakeUnloadComplete(201U, UART_LINETRACER_ROUTE_B);
    completion = ControlLogic_HandleUnloadResult(&context, &unload_result, 150U);
    assert(completion.completed != 0U);
    assert(completion.job_id == 201U);
    assert(completion.route_id == UART_LINETRACER_ROUTE_B);
    assert(completion.destination == UART_LINETRACER_POSITION_DEST_B);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);
    assert(context.current_position == UART_LINETRACER_POSITION_DEST_B);
    assert(context.route_plan.valid == 0U);
}

void TestMarkerAndLoadEventsDriveRouteC() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 203U, 0U);

    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(HandleExpectedMarker(context, 10U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(HandleExpectedMarker(context, 20U) == ROUTE_ACTION_TURN_LEFT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(ControlLogic_CompleteTurn(&context, 30U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
    assert(ControlLogic_HandleLoadOn(&context, 50U) == ROUTE_ACTION_TURN_AROUND);
    assert(context.route_plan.phase == ROUTE_PHASE_TO_C_RETURN_JUNCTION);
    assert(context.route_plan.expected_marker == ROUTE_MARKER_C_RETURN_JUNCTION);
    assert(ControlLogic_CompleteTurn(&context, 60U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);

    assert(HandleExpectedMarker(context, 70U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);
    assert(ControlLogic_CompleteTurn(&context, 80U) != 0U);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);

    assert(HandleExpectedMarker(context, 90U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.route_plan.expected_marker == ROUTE_MARKER_DEST);
    assert(HandleExpectedMarker(context, 100U) == ROUTE_ACTION_STOP_AT_DEST);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);
}

void TestLoadOffDuringReturnIsFault() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 1U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_C;
    assert(ControlLogic_HandleCommand(&context, &position, 1U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 2U, 2U);
    assign.job_id = 202U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &assign, 2U).accepted != 0U);
    assert(HandleExpectedMarker(context, 5U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 6U) == ROUTE_ACTION_TURN_LEFT);
    assert(ControlLogic_CompleteTurn(&context, 7U) != 0U);
    assert(HandleExpectedMarker(context, 8U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(ControlLogic_HandleLoadOn(&context, 9U) == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_CompleteTurn(&context, 10U) != 0U);

    control_job_completion_t completion{};
    assert(ControlLogic_HandleLoadOff(&context, 11U, &completion) == ROUTE_ACTION_LOAD_LOST);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_LOAD_LOST);
}

void TestEveryPickupReversesForTwoSecondsBeforeTurnAround() {
    const uart_linetracer_route_t routes[] = {
        UART_LINETRACER_ROUTE_A,
        UART_LINETRACER_ROUTE_B,
        UART_LINETRACER_ROUTE_C,
    };

    for (const auto route : routes) {
        control_context_t context{};
        constexpr std::uint32_t kLoadDetectedAtMs = 100U;

        ControlLogic_Init(&context, 0U);
        context.state = LINETRACER_CONTROL_WAITING_LOAD;
        context.current_position = UART_LINETRACER_POSITION_DEST_A;
        context.active_route = route;
        context.active_job_id = 501U;
        context.route_active = 1U;
        assert(RoutePlanner_Create(context.current_position, route, &context.route_plan) != 0U);
        context.route_plan.phase = ROUTE_PHASE_WAITING_LOAD;
        context.route_plan.loaded = 0U;

        assert(ControlLogic_HandleLoadOn(&context, kLoadDetectedAtMs) == ROUTE_ACTION_TURN_AROUND);
        assert(context.state == LINETRACER_CONTROL_TURNING_AT_PICKUP);
        assert(ControlLogic_StartPendingManeuver(&context, kLoadDetectedAtMs) != 0U);
        assert(context.junction_phase == CONTROL_JUNCTION_PICKUP_REVERSE);
        assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_REVERSE);

        assert(ControlLogic_UpdateTimedManeuver(&context, kLoadDetectedAtMs + CONTROL_PICKUP_REVERSE_MS - 1U) == 0U);
        assert(context.junction_phase == CONTROL_JUNCTION_PICKUP_REVERSE);
        assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_REVERSE);

        assert(ControlLogic_UpdateTimedManeuver(&context, kLoadDetectedAtMs + CONTROL_PICKUP_REVERSE_MS) != 0U);
        assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
        assert(context.junction_turn_started_at_ms == kLoadDetectedAtMs + CONTROL_PICKUP_REVERSE_MS);
        assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_TURN_AROUND);
    }
}

void TestDepartureTurnAroundDoesNotReverse() {
    control_context_t context{};

    ControlLogic_Init(&context, 0U);
    context.state = LINETRACER_CONTROL_TURNING_FROM_DEST;
    context.current_position = UART_LINETRACER_POSITION_DEST_A;
    context.active_route = UART_LINETRACER_ROUTE_B;
    context.active_job_id = 502U;
    context.route_active = 1U;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;

    assert(ControlLogic_StartPendingManeuver(&context, 100U) != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_TURN_AROUND);
}

void TestTelemetrySnapshotAndLifecycleEvents() {
    control_context_t context{};
    app_control_snapshot_t snapshot{};
    app_tx_event_t tx_event{};

    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 10U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 10U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 2U);
    assign.job_id = 77U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    const auto assign_result = ControlLogic_HandleCommand(&context, &assign, 20U);
    assert(assign_result.accepted != 0U);
    assert(ControlLogic_BuildStartedEvent(&context, &assign, &assign_result, UART_LINETRACER_LOAD_EMPTY, 20U,
                                          &tx_event) != 0U);
    assert(tx_event.type == APP_TX_EVENT_STARTED);
    assert(tx_event.job_id == 77U);
    assert(tx_event.route_id == UART_LINETRACER_ROUTE_C);

    ControlLogic_MakeSnapshot(&context, UART_LINETRACER_LOAD_PRESENT, 25U, &snapshot);
    assert(snapshot.updated_at_ms == 25U);
    assert(snapshot.job_id == 77U);
    assert(snapshot.route_id == UART_LINETRACER_ROUTE_C);
    assert(snapshot.load_state == UART_LINETRACER_LOAD_PRESENT);
    assert(snapshot.error_code == UART_ERROR_NONE);
    assert(snapshot.safety_latched == 0U);

    app_control_safety_event_t emergency{};
    emergency.type = APP_CONTROL_SAFETY_LATCHED;
    emergency.reason = LINETRACER_STOP_REASON_EMERGENCY;
    emergency.error_code = UART_ERROR_EMERGENCY_STOP;
    assert(ControlLogic_ApplySafetyEvent(&context, &emergency, 30U) != 0U);
    assert(ControlLogic_BuildSafetyFaultEvent(&context, &emergency, UART_LINETRACER_LOAD_PRESENT, 30U, &tx_event) !=
           0U);
    assert(tx_event.type == APP_TX_EVENT_FAULT);
    assert(tx_event.error_code == UART_ERROR_EMERGENCY_STOP);

    ControlLogic_MakeSnapshot(&context, UART_LINETRACER_LOAD_PRESENT, 30U, &snapshot);
    assert(snapshot.state == UART_LINETRACER_STATE_EMERGENCY_STOP);
    assert(snapshot.error_code == UART_ERROR_EMERGENCY_STOP);
    assert(snapshot.safety_latched != 0U);
}

}  // namespace

int main() {
    RunMotorControlLogicTests();
    TestInitializationAndPosition();
    TestSafetyLatchRejectsDriveUntilApprovedReset();
    TestEmergencyRecoveryResumesActiveRoute();
    TestObstacleSafetyState();
    TestRouteStopAndResume();
    TestInvalidStateStatusAndTimeout();
    TestResetSafetyAndCommandMapping();
    TestRouteTransitionRules();
    TestSameRoutesSkipCommonLine();
    TestMarkerValidationAndDuplicateSuppression();
    TestUnclassifiedMarkerIsRejected();
    TestRouteTimeouts();
    TestTurningStateDetection();
    TestTurnAroundActivityDetection();
    TestJunctionAcceptsOneDebouncedOuterBlackSample();
    TestRouteBLeftTurnUsesFirstDetectedTargetLine();
    TestOtherLeftTurnsDoNotSkipFirstTargetLine();
    TestStopMarkerUsesSameOuterPairAsTurnMarker();
    TestStopMarkerRejectsSeparatedOuterHits();
    TestStopMarkerUsesSameExitGuardAsTurnMarker();
    TestDelayedOneStripeMarkerFallbackIsNotDropped();
    TestCToBDelayedRightMarkerRetainsCenterAdvance();
    TestExpectedMarkerEdgeStartsLimitedPidWindow();
    TestPersistentSingleOuterHitReturnsControlToPid();
    TestRightTurnHandsTargetEdgeToNormalPidImmediately();
    TestCenteredSampleCannotCompleteTurnWithoutDirectionalEdge();
    TestTurnAroundHandsFirstLeftOrCenterTargetToPid();
    TestPickupTurnAroundRejectsReversePhaseLineSamples();
    TestLeftTurnHandsTargetEdgeToNormalPidImmediately();
    TestTurnTargetEdgeDoesNotWaitForCenterHit();
    TestStraightJunctionCrossingAndGuard();
    TestRouteCSecondMarkerIsAcceptedImmediatelyAfterFirstCrossingClears();
    TestRouteCReturnTurnRequiresStableSourceClear();
    TestRouteCToBUsesCommonImmediateReacquisition();
    TestStraightJunctionClearsDuringSingleOuterCorrection();
    TestStopMarkerWaitsForSameCrossingAndGuardAsTurnMarker();
    TestDestinationStopUsesSameOuterPairAsTurnMarker();
    TestJobCompletionAllowsNextAssignment();
    TestCompletionOutsideUnloadingDoesNothing();
    TestStopDuringUnloadRequestsAbort();
    TestMarkerAndLoadEventsDriveRouteB();
    TestMarkerAndLoadEventsDriveRouteC();
    TestLoadOffDuringReturnIsFault();
    TestEveryPickupReversesForTwoSecondsBeforeTurnAround();
    TestDepartureTurnAroundDoesNotReverse();
    TestTelemetrySnapshotAndLifecycleEvents();
    return 0;
}
