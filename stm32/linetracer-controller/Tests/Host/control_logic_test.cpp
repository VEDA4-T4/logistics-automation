#include "control_logic.h"

#include <cassert>
#include <cstdint>

namespace {

app_control_command_t MakeCommand(app_control_command_type_t type, std::uint32_t received_at_ms,
                                  std::uint8_t sequence) {
    app_control_command_t command{};
    command.type = type;
    command.received_at_ms = received_at_ms;
    command.sequence = sequence;
    return command;
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
    assert(resume_result.accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
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

    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_SOURCE_JUNCTION, 30U) == 0U);
    assert(ControlLogic_Transition(&context, LINETRACER_CONTROL_MOVING_TO_PICKUP, 30U) != 0U);
}

void TestSameRoutesSkipCommonLine() {
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_A, 101U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, 102U);
    TestSameRouteCase(UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_C, 103U);
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

}  // namespace

int main() {
    TestInitializationAndPosition();
    TestSafetyLatchRejectsDriveUntilApprovedReset();
    TestObstacleSafetyState();
    TestRouteStopAndResume();
    TestInvalidStateStatusAndTimeout();
    TestResetSafetyAndCommandMapping();
    TestRouteTransitionRules();
    TestSameRoutesSkipCommonLine();
    TestJobCompletionAllowsNextAssignment();
    TestCompletionOutsideUnloadingDoesNothing();
    return 0;
}
