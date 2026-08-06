#include <cassert>
#include <cstdint>

#include "control_logic.h"

namespace {

app_control_command_t MakeCommand(app_control_command_type_t type, std::uint32_t received_at_ms,
                                  std::uint8_t sequence) {
    app_control_command_t command{};
    command.type = type;
    command.received_at_ms = received_at_ms;
    command.sequence = sequence;
    return command;
}

void TestObstacleIsIgnoredOutsideRouteMotion() {
    control_context_t context{};
    app_control_safety_event_t event{};

    ControlLogic_Init(&context, 0U);
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) == 0U);
    event.type = APP_CONTROL_SAFETY_OBSTACLE_ACTIVE;
    event.reason = LINETRACER_STOP_REASON_OBSTACLE;
    event.error_code = UART_ERROR_SENSOR;
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 10U) == 0U);
    assert(context.state == LINETRACER_CONTROL_INITIALIZING);
    assert(context.resume_state == LINETRACER_CONTROL_INITIALIZING);
    assert(context.safety_latched == 0U);
    assert(context.resume_valid == 0U);
}

void TestMovingRouteResumesWithoutLatching() {
    control_context_t context{};
    app_control_safety_event_t event{};

    ControlLogic_Init(&context, 100U);
    auto position = MakeCommand(APP_CONTROL_COMMAND_SET_CURRENT_POSITION, 101U, 1U);
    position.position = UART_LINETRACER_POSITION_DEST_B;
    assert(ControlLogic_HandleCommand(&context, &position, 101U).accepted != 0U);

    auto assign = MakeCommand(APP_CONTROL_COMMAND_ASSIGN_ROUTE, 102U, 2U);
    assign.job_id = 55U;
    assign.route_id = UART_LINETRACER_ROUTE_C;
    assert(ControlLogic_HandleCommand(&context, &assign, 102U).accepted != 0U);
    const auto moving_state = context.state;
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) != 0U);

    event.type = APP_CONTROL_SAFETY_OBSTACLE_ACTIVE;
    event.reason = LINETRACER_STOP_REASON_OBSTACLE;
    event.error_code = UART_ERROR_SENSOR;
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 110U) != 0U);
    assert(context.state == LINETRACER_CONTROL_OBSTACLE_STOP);
    assert(context.safety_latched == 0U);
    assert(context.route_active != 0U);
    assert(context.active_job_id == 55U);
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) != 0U);
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 111U) == 0U);

    event.type = APP_CONTROL_SAFETY_OBSTACLE_CLEARED;
    event.reason = LINETRACER_STOP_REASON_NONE;
    event.error_code = UART_ERROR_NONE;
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 120U) != 0U);
    assert(context.state == moving_state);
    assert(context.stop_reason == LINETRACER_STOP_REASON_NONE);
    assert(context.safety_error_code == UART_ERROR_NONE);
    assert(context.safety_latched == 0U);
    assert(context.route_active != 0U);
    assert(context.active_job_id == 55U);
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) != 0U);
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 121U) == 0U);
}

void TestTurningRouteResumesWithFreshTimeouts() {
    control_context_t context{};
    app_control_safety_event_t event{};

    ControlLogic_Init(&context, 0U);
    context.state = LINETRACER_CONTROL_TURNING_FROM_DEST;
    context.route_active = 1U;
    context.active_job_id = 12U;
    context.active_route = UART_LINETRACER_ROUTE_A;
    context.pending_route_action = ROUTE_ACTION_TURN_AROUND;
    context.junction_phase = CONTROL_JUNCTION_TURN_SEARCH_TARGET;
    context.junction_phase_started_at_ms = 10U;
    context.junction_turn_started_at_ms = 20U;
    assert(ControlLogic_IsTurning(&context) != 0U);

    event.type = APP_CONTROL_SAFETY_OBSTACLE_ACTIVE;
    event.reason = LINETRACER_STOP_REASON_OBSTACLE;
    event.error_code = UART_ERROR_SENSOR;
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 100U) != 0U);
    assert(context.state == LINETRACER_CONTROL_OBSTACLE_STOP);

    event.type = APP_CONTROL_SAFETY_OBSTACLE_CLEARED;
    event.reason = LINETRACER_STOP_REASON_NONE;
    event.error_code = UART_ERROR_NONE;
    assert(ControlLogic_ApplySafetyEvent(&context, &event, 500U) != 0U);
    assert(context.state == LINETRACER_CONTROL_TURNING_FROM_DEST);
    assert(context.junction_phase == CONTROL_JUNCTION_TURN_SEARCH_TARGET);
    assert(context.junction_phase_started_at_ms == 500U);
    assert(context.junction_turn_started_at_ms == 500U);
}

void TestUltrasonicMonitoringDisabledWhileParkedOrLoading() {
    control_context_t context{};

    ControlLogic_Init(&context, 0U);
    context.route_active = 1U;

    context.state = LINETRACER_CONTROL_WAITING_AT_DEST;
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) == 0U);
    context.state = LINETRACER_CONTROL_PICKUP_READY;
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) == 0U);
    context.state = LINETRACER_CONTROL_WAITING_LOAD;
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) == 0U);
    context.state = LINETRACER_CONTROL_UNLOADING;
    assert(ControlLogic_UltrasonicMonitoringRequired(&context) == 0U);
}

}  // namespace

int main() {
    TestObstacleIsIgnoredOutsideRouteMotion();
    TestMovingRouteResumesWithoutLatching();
    TestTurningRouteResumesWithFreshTimeouts();
    TestUltrasonicMonitoringDisabledWhileParkedOrLoading();
    return 0;
}
