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
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);
    assert(HandleExpectedMarker(context, 50U) == ROUTE_ACTION_STOP_AT_PICKUP);
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

void AdvanceToTargetSearch(control_context_t& context, route_action_t action, std::uint32_t started_at_ms) {
    context.junction_phase = CONTROL_JUNCTION_TURN_CLEAR_SOURCE;
    context.junction_action = action;
    context.junction_turn_started_at_ms = started_at_ms;
    context.junction_phase_started_at_ms = started_at_ms;

    /* The center sensor still seeing the source line must not arm target search. */
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, started_at_ms);
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, started_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_CLEAR_SOURCE);

    /* Search starts only after all three sensors are white for the clear interval. */
    const auto clear_at = started_at_ms + CONTROL_TURN_SOURCE_CLEAR_MS + 10U;
    (void)ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, clear_at);
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

void TestRouteBTargetJunctionStartsLeftTurnFromLineSample() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_B, 417U, 0U);

    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    assert(ControlLogic_CompleteTurn(&context, 40U) != 0U);
    assert(context.state == LINETRACER_CONTROL_MOVING_ON_COMMON_LINE);
    assert(context.route_plan.expected_marker == ROUTE_MARKER_COMMON_JUNCTION);
    assert(context.route_plan.junctions_remaining == 1U);

    const auto after_guard = 40U + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 1U, after_guard);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_TURN_LEFT);
    assert(context.state == LINETRACER_CONTROL_TURNING_TO_PICKUP);
    assert(context.junction_phase == CONTROL_JUNCTION_APPROACH_CENTER);
    assert(ControlLogic_JunctionMotorAction(&context) == ROUTE_ACTION_GO_STRAIGHT);
}

void TestEndpointStopPrioritizesOuterDo() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 415U, 0U);

    assert(HandleExpectedMarker(context, 40U) == ROUTE_ACTION_GO_STRAIGHT);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_PICKUP);

    /* 101 is an endpoint marker even when center AO/DO remains white. */
    const auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 1U, 50U);
    assert(result.action_valid != 0U);
    assert(result.action == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(context.state == LINETRACER_CONTROL_WAITING_LOAD);
}

void TestRightTurnCompletesOnStableRightTargetSide() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 406U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_RIGHT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* A left-side edge is not the target side of a clockwise right turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, search_at + 10U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    /* 011 without a preceding 001 is still the old/source line and must be ignored. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, search_at + 20U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U,
                                                      search_at + 20U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed == 0U);

    /* The new right target must enter through 001 before 011 can finish alignment. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, search_at + 80U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_target_edge_seen != 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, search_at + 90U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U,
                                                      search_at + 90U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
}

void TestTurnCompletesWhenCenterIsStable() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, 416U, 0U);
    assert(HandleExpectedMarker(context, 30U) == ROUTE_ACTION_TURN_RIGHT);
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_RIGHT, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;

    /* Center-only 010 cannot complete a turn before the direction-specific edge is seen. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, search_at + 10U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U,
                                                      search_at + 10U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed == 0U);

    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, search_at + 80U);
    assert(context.junction_target_edge_seen != 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U, search_at + 90U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 0U,
                                                      search_at + 90U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.pending_route_action == ROUTE_ACTION_GO_STRAIGHT);
}

void TestTurnAroundCompletesOnStableRightTargetSide() {
    control_context_t context{};
    StartRoute(context, UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 409U, 0U);
    context.state = LINETRACER_CONTROL_TURNING_AT_PICKUP;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
    AdvanceToTargetSearch(context, ROUTE_ACTION_TURN_AROUND, 100U);
    const auto search_at = 100U + (2U * CONTROL_TURN_SOURCE_CLEAR_MS) + 10U;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, search_at, search_at) != 0U);

    /* 000 is line loss and must not complete the turn. */
    auto result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 0U, search_at + 40U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);

    /* 001 arms target alignment but cannot complete the turn by itself. */
    const auto completed_at = search_at + 50U;
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 0U, 1U, completed_at);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U, completed_at + 10U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 0U, 1U, 1U,
                                                      completed_at + 10U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed != 0U);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
    assert(context.state == LINETRACER_CONTROL_MOVING_TO_DEST);

    const auto stable_completed_at = completed_at + 10U + CONTROL_TURN_TARGET_CENTERED_MS;
    const auto after_guard = stable_completed_at + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, search_at, after_guard) != 0U);
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, stable_completed_at + 1U, after_guard) == 0U);
}

void TestLeftTurnCompletesOnStableLeftTargetSide() {
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

    /* 110 without a preceding 100 is still the old/source line and must be ignored. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, search_at + 20U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U,
                                                      search_at + 20U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed == 0U);

    /* The new left target must enter through 100 before 110 can finish alignment. */
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 0U, 0U, search_at + 80U);
    assert(result.maneuver_completed == 0U);
    assert(context.junction_target_edge_seen != 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U, search_at + 90U);
    assert(result.maneuver_completed == 0U);
    result = ControlLogic_ProcessLineSampleWithCenter(&context, 1U, 1U, 0U,
                                                      search_at + 90U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(result.maneuver_completed != 0U);
    assert(result.state_changed == 0U);
    assert(context.state == original_state);
    assert(context.junction_phase == CONTROL_JUNCTION_IDLE);
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
    completed = ControlLogic_ProcessLineSample(&context, 0U, 0U, 200U + CONTROL_TURN_TARGET_CENTERED_MS);
    assert(completed.maneuver_completed != 0U);
    const auto completed_at_ms = 200U + CONTROL_TURN_TARGET_CENTERED_MS;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_JUNCTION, 100U, completed_at_ms + 1U) != 0U);

    const auto after_guard_ms = completed_at_ms + CONTROL_JUNCTION_EXIT_GUARD_MS + 1U;
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, 100U, after_guard_ms) != 0U);
    assert(ControlLogic_ShouldIgnoreMarker(&context, APP_MARKER_DEST_A, completed_at_ms + 1U, after_guard_ms) == 0U);
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
    TestJunctionAcceptsOneDebouncedOuterBlackSample();
    TestRouteBTargetJunctionStartsLeftTurnFromLineSample();
    TestEndpointStopPrioritizesOuterDo();
    TestRightTurnCompletesOnStableRightTargetSide();
    TestTurnCompletesWhenCenterIsStable();
    TestTurnAroundCompletesOnStableRightTargetSide();
    TestLeftTurnCompletesOnStableLeftTargetSide();
    TestStraightJunctionCrossingAndGuard();
    TestJobCompletionAllowsNextAssignment();
    TestCompletionOutsideUnloadingDoesNothing();
    TestStopDuringUnloadRequestsAbort();
    TestMarkerAndLoadEventsDriveRouteB();
    TestLoadOffDuringReturnIsFault();
    TestTelemetrySnapshotAndLifecycleEvents();
    return 0;
}
