#include <cassert>
#include <cstdint>

extern "C" {
#include "control_logic.h"
}

namespace {

app_control_command_t MakeCommand(app_control_command_type_t type, std::uint32_t now_ms, std::uint8_t sequence) {
    app_control_command_t command{};
    command.type = type;
    command.received_at_ms = now_ms;
    command.sequence = sequence;
    return command;
}

void PrepareUnloading(control_context_t& context, std::uint16_t job_id, uart_linetracer_route_t route_id) {
    ControlLogic_Init(&context, 0U);

    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 1U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_A;
    assert(ControlLogic_HandleCommand(&context, &position, 1U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 2U, 2U);
    assign.job_id = job_id;
    assign.route_id = route_id;
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
}

app_unload_result_t MakeUnloadResult(app_unload_result_type_t type, std::uint16_t job_id,
                                     uart_linetracer_route_t route_id) {
    app_unload_result_t result{};
    result.type = type;
    result.job_id = job_id;
    result.route_id = route_id;
    return result;
}

void TestFilteredEmptyWaitsForServoCompletion() {
    control_context_t context{};
    control_job_completion_t completion{};

    PrepareUnloading(context, 501U, UART_LINETRACER_ROUTE_B);

    assert(ControlLogic_HandleLoadOff(&context, 20U, &completion) == ROUTE_ACTION_NONE);
    assert(completion.completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);
    assert(context.active_job_id == 501U);

    const auto timeout = MakeUnloadResult(APP_UNLOAD_RESULT_TIMEOUT, 501U, UART_LINETRACER_ROUTE_B);
    assert(ControlLogic_HandleUnloadResult(&context, &timeout, 21U).completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    const auto stale = MakeUnloadResult(APP_UNLOAD_RESULT_COMPLETE, 999U, UART_LINETRACER_ROUTE_B);
    assert(ControlLogic_HandleUnloadResult(&context, &stale, 22U).completed == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    const auto complete = MakeUnloadResult(APP_UNLOAD_RESULT_COMPLETE, 501U, UART_LINETRACER_ROUTE_B);
    completion = ControlLogic_HandleUnloadResult(&context, &complete, 23U);
    assert(completion.completed != 0U);
    assert(completion.job_id == 501U);
    assert(completion.destination == UART_LINETRACER_POSITION_DEST_B);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);
    assert(context.active_job_id == UART_LINETRACER_JOB_ID_NONE);
    assert(context.route_active == 0U);
}

void TestNewRouteStartsUturnOnlyAfterCompletion() {
    control_context_t context{};

    PrepareUnloading(context, 502U, UART_LINETRACER_ROUTE_C);

    auto premature = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 20U, 3U);
    premature.job_id = 503U;
    premature.route_id = UART_LINETRACER_ROUTE_A;
    assert(ControlLogic_HandleCommand(&context, &premature, 20U).accepted == 0U);
    assert(context.state == LINETRACER_CONTROL_UNLOADING);

    const auto complete = MakeUnloadResult(APP_UNLOAD_RESULT_COMPLETE, 502U, UART_LINETRACER_ROUTE_C);
    assert(ControlLogic_HandleUnloadResult(&context, &complete, 21U).completed != 0U);
    assert(context.state == LINETRACER_CONTROL_WAITING_AT_DEST);

    auto next_route = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 22U, 4U);
    next_route.job_id = 503U;
    next_route.route_id = UART_LINETRACER_ROUTE_A;
    assert(ControlLogic_HandleCommand(&context, &next_route, 22U).accepted != 0U);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.pending_route_action == ROUTE_ACTION_TURN_AROUND);
}

void TestStopDuringUnloadCannotResumeAutomatically() {
    control_context_t context{};

    PrepareUnloading(context, 504U, UART_LINETRACER_ROUTE_A);

    auto stop = MakeCommand(APP_CONTROL_COMMAND_STOP_DRIVE, 20U, 3U);
    stop.job_id = 504U;
    const auto stop_result = ControlLogic_HandleCommand(&context, &stop, 20U);
    assert(stop_result.accepted != 0U);
    assert(stop_result.unload_command == APP_UNLOAD_COMMAND_ABORT);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
    assert(context.resume_valid == 0U);

    const auto resume = MakeCommand(APP_CONTROL_COMMAND_RESUME_DRIVE, 21U, 4U);
    assert(ControlLogic_HandleCommand(&context, &resume, 21U).accepted == 0U);
    assert(context.state == LINETRACER_CONTROL_STOPPED);
}

}  // namespace

int main() {
    TestFilteredEmptyWaitsForServoCompletion();
    TestNewRouteStartsUturnOnlyAfterCompletion();
    TestStopDuringUnloadCannotResumeAutomatically();
    return 0;
}
