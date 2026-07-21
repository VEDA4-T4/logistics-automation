#include "route_planner.h"

#include <cassert>
#include <cstdint>

namespace {

uart_linetracer_position_t PositionForIndex(std::uint8_t index) {
    return static_cast<uart_linetracer_position_t>(UART_LINETRACER_POSITION_MIN + index);
}

uart_linetracer_route_t RouteForIndex(std::uint8_t index) {
    return static_cast<uart_linetracer_route_t>(UART_LINETRACER_ROUTE_MIN + index);
}

void TestOutboundAndReturn(std::uint8_t origin, std::uint8_t target) {
    route_plan_t plan{};
    const auto position = PositionForIndex(origin);
    const auto route = RouteForIndex(target);
    const auto distance = static_cast<std::uint8_t>((origin > target) ? origin - target : target - origin);

    assert(RoutePlanner_Create(position, route, &plan) != 0U);
    assert(plan.valid != 0U);
    assert(plan.origin_index == origin);
    assert(plan.target_index == target);
    assert(plan.junctions_total == distance);
    assert(plan.junctions_remaining == distance);
    assert(plan.phase == ROUTE_PHASE_TO_SOURCE_JUNCTION);
    assert(plan.expected_marker == ROUTE_MARKER_DEST_EXIT);
    assert(RoutePlanner_TargetDestination(&plan) == PositionForIndex(target));

    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(plan.phase == ROUTE_PHASE_TO_SOURCE_JUNCTION);
    assert(plan.expected_marker == ROUTE_MARKER_SOURCE_JUNCTION);

    if (target == origin) {
        assert(plan.common_direction == ROUTE_DIRECTION_NONE);
        assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    } else {
        const auto source_action = (target > origin) ? ROUTE_ACTION_TURN_RIGHT : ROUTE_ACTION_TURN_LEFT;
        const auto target_action = (target > origin) ? ROUTE_ACTION_TURN_LEFT : ROUTE_ACTION_TURN_RIGHT;

        assert(plan.common_direction == ((target > origin) ? ROUTE_DIRECTION_RIGHT : ROUTE_DIRECTION_LEFT));
        assert(RoutePlanner_OnMarker(&plan) == source_action);
        assert(plan.phase == ROUTE_PHASE_ON_COMMON_LINE);

        for (std::uint8_t marker = 0U; marker < distance; ++marker) {
            const auto expected_action = (marker + 1U == distance) ? target_action : ROUTE_ACTION_GO_STRAIGHT;
            assert(RoutePlanner_OnMarker(&plan) == expected_action);
        }
    }

    assert(plan.phase == ROUTE_PHASE_TO_PICKUP);
    assert(plan.expected_marker == ROUTE_MARKER_PICKUP);
    assert(plan.junctions_remaining == 0U);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(plan.phase == ROUTE_PHASE_WAITING_LOAD);

    assert(RoutePlanner_OnLoadOn(&plan) == ROUTE_ACTION_TURN_AROUND);
    assert(plan.loaded != 0U);
    assert(plan.phase == ROUTE_PHASE_RETURN_TO_DEST);
    assert(plan.expected_marker == ROUTE_MARKER_PICKUP_EXIT);

    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(plan.expected_marker == ROUTE_MARKER_RETURN_JUNCTION);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(plan.expected_marker == ROUTE_MARKER_DEST);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_STOP_AT_DEST);
    assert(plan.phase == ROUTE_PHASE_UNLOADING);

    assert(RoutePlanner_OnLoadOff(&plan) == ROUTE_ACTION_JOB_COMPLETE);
    assert(plan.phase == ROUTE_PHASE_COMPLETE);
    assert(plan.loaded == 0U);
}

void TestAllOutboundAndReturnRoutes() {
    for (std::uint8_t origin = 0U; origin < 3U; ++origin) {
        for (std::uint8_t target = 0U; target < 3U; ++target) {
            TestOutboundAndReturn(origin, target);
        }
    }
}

void MovePlanToLoadedReturn(route_plan_t* plan) {
    const auto distance = plan->junctions_total;

    assert(RoutePlanner_OnMarker(plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(RoutePlanner_OnMarker(plan) != ROUTE_ACTION_ERROR);
    for (std::uint8_t marker = 0U; marker < distance; ++marker) {
        assert(RoutePlanner_OnMarker(plan) != ROUTE_ACTION_ERROR);
    }
    assert(RoutePlanner_OnMarker(plan) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(RoutePlanner_OnLoadOn(plan) == ROUTE_ACTION_TURN_AROUND);
}

void TestLoadLostDuringReturn() {
    route_plan_t plan{};

    assert(RoutePlanner_Create(UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_C, &plan) != 0U);
    MovePlanToLoadedReturn(&plan);
    assert(RoutePlanner_OnLoadOff(&plan) == ROUTE_ACTION_LOAD_LOST);
    assert(plan.phase == ROUTE_PHASE_ERROR);
    assert(plan.valid == 0U);
}

void TestInvalidInputsAndUnexpectedMarker() {
    route_plan_t plan{};

    assert(RoutePlanner_Create(UART_LINETRACER_POSITION_NONE, UART_LINETRACER_ROUTE_A, &plan) == 0U);
    assert(plan.valid == 0U);
    assert(RoutePlanner_Create(UART_LINETRACER_POSITION_DEST_A, UART_LINETRACER_ROUTE_NONE, &plan) == 0U);
    assert(plan.valid == 0U);

    assert(RoutePlanner_Create(UART_LINETRACER_POSITION_DEST_B, UART_LINETRACER_ROUTE_B, &plan) != 0U);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_GO_STRAIGHT);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_STOP_AT_PICKUP);
    assert(RoutePlanner_OnMarker(&plan) == ROUTE_ACTION_ERROR);
    assert(plan.phase == ROUTE_PHASE_ERROR);
}

void TestReset() {
    route_plan_t plan{};

    assert(RoutePlanner_Create(UART_LINETRACER_POSITION_DEST_C, UART_LINETRACER_ROUTE_A, &plan) != 0U);
    RoutePlanner_Reset(&plan);
    assert(plan.phase == ROUTE_PHASE_IDLE);
    assert(plan.expected_marker == ROUTE_MARKER_NONE);
    assert(plan.origin_index == ROUTE_PLANNER_INDEX_INVALID);
    assert(plan.target_index == ROUTE_PLANNER_INDEX_INVALID);
    assert(plan.valid == 0U);
}

}  // namespace

int main() {
    TestAllOutboundAndReturnRoutes();
    TestLoadLostDuringReturn();
    TestInvalidInputsAndUnexpectedMarker();
    TestReset();
    return 0;
}
