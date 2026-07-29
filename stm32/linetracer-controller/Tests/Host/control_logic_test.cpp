#include "control_logic.h"

#include <cassert>
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

route_action_t HandleExpectedMarker(control_context_t& context, std::uint32_t now_ms) {
    const auto marker_code = ControlLogic_ExpectedMarkerCode(&context);
    return ControlLogic_HandleMarker(&context, marker_code, now_ms, now_ms);
}

app_marker_code_t MarkerForRoute(uart_linetracer_route_t route_id) {
    switch (route_id) {
        case UART_LINETRACER_ROUTE_A:
            return APP_MARKER_DEST_A;

        case UART_LINETRACER_ROUTE_B:
            return APP_MARKER_DEST_B;

        case UART_LINETRACER_ROUTE_C:
            return APP_MARKER_DEST_C;

        default:
            return APP_MARKER_INVALID;
    }
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

void TestObstacleSafetyState() {
    control_context_t context{};
    ControlLogic_Init(&context, 200U);

    app_control_safety_event_t safety_event{};
    safety_event.type = APP_CONTROL_SAFETY_LATCHED;
    safety_event.reason = LINETRACER_STOP_REASON_OBSTACLE;
    safety_event.error_code = UART_ERROR_SENSOR;

    assert(ControlLogic_ApplySafetyEvent(&context, &safety_event, 210U) != 0U);
    assert(context.safety_latched != 0U);
    assert(context.state == LINETRACER_CONTROL_OBSTACLE_STOP);
    assert(context.stop_reason == LINETRACER_STOP_REASON_OBSTACLE);
    assert(context.safety_error_code == UART_ERROR_SENSOR);
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
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.active_job_id == 42U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);

    auto wrong_stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 30U, 3U);
    wrong_stop.job_id = 43U;
    const auto wrong_stop_result = ControlLogic_HandleCommand(&context, &wrong_stop, 30U);
    assert(wrong_stop_result.accepted == 0U);
    assert(wrong_stop_result.error_code == UART_ERROR_INVALID_PAYLOAD);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);

    auto stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 40U, 4U);
    stop.job_id = 42U;
    const auto stop_result = ControlLogic_HandleCommand(&context, &stop, 40U);
    assert(stop_result.accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
    assert(context.resume_state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.resume_valid != 0U);

    const auto repeated_stop_result = ControlLogic_HandleCommand(&context, &stop, 50U);
    assert(repeated_stop_result.accepted != 0U);
    assert(repeated_stop_result.state_changed == 0U);

    const auto resume = MakeCommand(APP_CONTROL_COMMAND_RESUME_DRIVE, 60U, 5U);
    const auto resume_result = ControlLogic_HandleCommand(&context, &resume, 60U);
    assert(resume_result.accepted == 0U);
    assert(resume_result.error_code == UART_ERROR_BUSY);
    assert(context.state == LINETRACER_CONTROL_STOPPED);

    app_control_safety_event_t recovery{};
    recovery.type = APP_CONTROL_SAFETY_RECOVERY_APPROVED;
    assert(ControlLogic_ApplySafetyEvent(&context, &recovery, 60U) != 0U);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.resume_valid == 0U);
    assert(context.state_entered_at_ms == 60U);
    assert(context.marker_wait_started_at_ms == 60U);
}

void TestLineLossAutoRecoveryPreservesRouteAndRestartsTimeouts() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 55U, 0U);
    assert(ControlLogic_CompleteTurn(&context, 10U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);

    app_control_safety_event_t line_lost{};
    line_lost.type = APP_CONTROL_SAFETY_LATCHED;
    line_lost.reason = LINETRACER_STOP_REASON_LINE_LOST;
    line_lost.error_code = UART_ERROR_SENSOR;
    assert(ControlLogic_ApplySafetyEvent(&context, &line_lost, 1000U) != 0U);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.resume_state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.resume_valid != 0U);
    assert(context.active_job_id == 55U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);

    app_control_safety_event_t recovered{};
    recovered.type = APP_CONTROL_SAFETY_AUTO_RECOVERY_APPROVED;
    recovered.reason = LINETRACER_STOP_REASON_LINE_LOST;
    assert(ControlLogic_ApplySafetyEvent(&context, &recovered, 5000U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(context.active_job_id == 55U);
    assert(context.active_route == UART_LINETRACER_ROUTE_C);
    assert(context.safety_latched == 0U);
    assert(context.stop_reason == LINETRACER_STOP_REASON_NONE);
    assert(context.state_entered_at_ms == 5000U);
    assert(context.marker_wait_started_at_ms == 5000U);
    assert(ControlLogic_CheckRouteTimeout(&context, 5000U + CONTROL_MARKER_TIMEOUT_MS - 1U) ==
           LINETRACER_STOP_REASON_NONE);
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

    const auto stale_status = MakeCommand(APP_CONTROL_COMMAND_STATUS_REQUEST, 0U, 3U);
    const auto stale_result = ControlLogic_HandleCommand(&context, &stale_status, UART_COMMAND_TIMEOUT_MS + 1U);
    assert(stale_result.accepted == 0U);
    assert(stale_result.error_code == UART_ERROR_TIMEOUT);

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
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.route_plan.valid != 0U);
    assert(context.route_plan.junctions_total == 0U);

    assert(ControlLogic_CompleteTurn(&context, 30U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(ControlLogic_ExpectedMarkerCode(&context) == MarkerForRoute(route_id));
    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);
    assert(HandleExpectedMarker(context, 50U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(ControlLogic_ExpectedMarkerCode(&context) == MarkerForRoute(route_id));
}

void TestSameRoutesSkipCommonLine() {
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 101U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 102U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 103U);
}

void TestMarkerValidationAndDuplicateSuppression() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 301U, 0U);
    assert(ControlLogic_CompleteTurn(&context, 10U) != 0U);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_DEST_A);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_DEST_A, 100U, 100U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(ControlLogic_ExpectedMarkerCode(&context) == APP_MARKER_JUNCTION);
    const auto expected_after_first = context.route_plan.expected_marker;
    const auto state_after_first = context.state;

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_DEST_A, 100U, 110U) == ROUTE_ACTION_NONE);
    assert(context.route_plan.expected_marker == expected_after_first);
    assert(context.state == state_after_first);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_JUNCTION, 500U, 500U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_DEST_C, 900U, 900U) == ROUTE_ACTION_ERROR);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 302U, 1000U);
    assert(ControlLogic_CompleteTurn(&context, 1010U) != 0U);
    assert(ControlLogic_HandleMarker(&context, APP_MARKER_INVALID, 1100U, 1100U) == ROUTE_ACTION_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
}

void TestUnclassifiedMarkerIsRejected() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 303U, 0U);
    assert(ControlLogic_CompleteTurn(&context, 10U) != 0U);

    assert(ControlLogic_HandleMarker(&context, APP_MARKER_NONE, 100U, 100U) == ROUTE_ACTION_ERROR);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
}

void TestRouteTimeouts() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 401U, 0U);

    assert(ControlLogic_CheckRouteTimeout(&context, 2U + CONTROL_TURN_TIMEOUT_MS - 1U) == LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 2U + CONTROL_TURN_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_TURN_TIMEOUT);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_TURN_TIMEOUT);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 402U, 5000U);
    assert(ControlLogic_CompleteTurn(&context, 5010U) != 0U);
    assert(HandleExpectedMarker(context, 5100U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 5200U) == ROUTE_ACTION_TURN_RIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.pending_route_action == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CheckRouteTimeout(&context, 5200U + CONTROL_TURN_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_TURN_TIMEOUT);

    StartRoute(context, UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, 403U, 10000U);
    assert(ControlLogic_CompleteTurn(&context, 10100U) != 0U);
    assert(ControlLogic_CheckRouteTimeout(&context, 10100U + CONTROL_MARKER_TIMEOUT_MS - 1U) ==
           LINETRACER_STOP_REASON_NONE);
    assert(ControlLogic_CheckRouteTimeout(&context, 10100U + CONTROL_MARKER_TIMEOUT_MS) ==
           LINETRACER_STOP_REASON_MARKER_SEQUENCE);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_MARKER_SEQUENCE);
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

    const auto completion = ControlLogic_CompleteJob(&context, 120U);
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
}

void TestCompletionOutsideUnloadingDoesNothing() {
    control_context_t context{};
    ControlLogic_Init(&context, 0U);

    const auto completion = ControlLogic_CompleteJob(&context, 10U);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
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

    assert(ControlLogic_CompleteTurn(&context, 30U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION);
    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
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

    assert(HandleExpectedMarker(context, 110U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 120U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 130U) == ROUTE_ACTION_STOP_AT_DEST);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    control_job_completion_t completion{};
    assert(ControlLogic_HandleLoadOff(&context, 140U, &completion) == ROUTE_ACTION_JOB_COMPLETE);
    assert(completion.completed != 0U);
    assert(completion.job_id == 201U);
    assert(completion.route_id == UART_LINETRACER_ROUTE_B);
    assert(completion.destination == UART_LINETRACER_POSITION_DEST_B);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);
    assert(context.current_position == UART_LINETRACER_POSITION_DEST_B);
    assert(context.route_plan.valid == 0U);
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
    assert(ControlLogic_CompleteTurn(&context, 3U) != 0U);
    assert(HandleExpectedMarker(context, 4U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 5U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(HandleExpectedMarker(context, 6U) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(ControlLogic_HandleLoadOn(&context, 7U) == ROUTE_ACTION_TURN_AROUND);
    assert(ControlLogic_CompleteTurn(&context, 8U) != 0U);

    control_job_completion_t completion{};
    assert(ControlLogic_HandleLoadOff(&context, 9U, &completion) == ROUTE_ACTION_LOAD_LOST);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_ERROR);
    assert(context.stop_reason == LINETRACER_STOP_REASON_LOAD_LOST);
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

    auto status = MakeCommand(APP_CONTROL_COMMAND_STATUS_REQUEST, 31U, 0x5AU);
    status.original_payload_crc = 0x1234U;
    const auto status_result = ControlLogic_HandleCommand(&context, &status, 31U);
    assert(status_result.accepted != 0U);
    assert(status_result.error_code == UART_ERROR_NONE);
    assert(ControlLogic_BuildStatusEvent(&context, &status, &status_result, UART_LINETRACER_LOAD_PRESENT, 31U,
                                         &tx_event) != 0U);
    assert(tx_event.type == APP_TX_EVENT_STATUS);
    assert(tx_event.created_at_ms == 31U);
    assert(tx_event.request_sequence == 0x5AU);
    assert(tx_event.original_command == UART_CMD_LINETRACER_STATUS_REQUEST);
    assert(tx_event.original_payload_crc == 0x1234U);
    assert(tx_event.status == UART_STATUS_ACK);
    assert(tx_event.state == UART_LINETRACER_STATE_EMERGENCY_STOP);
    assert(tx_event.error_code == UART_ERROR_EMERGENCY_STOP);
    assert(tx_event.job_id == 77U);
    assert(tx_event.route_id == UART_LINETRACER_ROUTE_C);
    assert(tx_event.load_state == UART_LINETRACER_LOAD_PRESENT);
}

}  // namespace

int main() {
    RunMotorControlLogicTests();
    TestInitializationAndPosition();
    TestSafetyLatchRejectsDriveUntilApprovedReset();
    TestObstacleSafetyState();
    TestRouteStopAndResume();
    TestLineLossAutoRecoveryPreservesRouteAndRestartsTimeouts();
    TestInvalidStateStatusAndTimeout();
    TestResetSafetyAndCommandMapping();
    TestRouteTransitionRules();
    TestSameRoutesSkipCommonLine();
    TestMarkerValidationAndDuplicateSuppression();
    TestUnclassifiedMarkerIsRejected();
    TestRouteTimeouts();
    TestJobCompletionAllowsNextAssignment();
    TestCompletionOutsideUnloadingDoesNothing();
    TestMarkerAndLoadEventsDriveRouteB();
    TestLoadOffDuringReturnIsFault();
    TestTelemetrySnapshotAndLifecycleEvents();
    return 0;
}
