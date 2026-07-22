#include "route_planner.h"

#include <stddef.h>

static uint8_t RoutePlanner_PositionIndex(uart_linetracer_position_t position) {
    if (uart_linetracer_position_is_valid(position) == 0U) {
        return ROUTE_PLANNER_INDEX_INVALID;
    }

    return (uint8_t)(position - UART_LINETRACER_POSITION_MIN);
}

static uint8_t RoutePlanner_RouteIndex(uart_linetracer_route_t route) {
    if (uart_linetracer_route_is_valid(route) == 0U) {
        return ROUTE_PLANNER_INDEX_INVALID;
    }

    return (uint8_t)(route - UART_LINETRACER_ROUTE_MIN);
}

static route_action_t RoutePlanner_Fail(route_plan_t* plan) {
    plan->phase = ROUTE_PHASE_ERROR;
    plan->expected_marker = ROUTE_MARKER_NONE;
    plan->valid = 0U;
    return ROUTE_ACTION_ERROR;
}

void RoutePlanner_Reset(route_plan_t* plan) {
    if (plan == NULL) {
        return;
    }

    plan->phase = ROUTE_PHASE_IDLE;
    plan->common_direction = ROUTE_DIRECTION_NONE;
    plan->expected_marker = ROUTE_MARKER_NONE;
    plan->origin_index = ROUTE_PLANNER_INDEX_INVALID;
    plan->target_index = ROUTE_PLANNER_INDEX_INVALID;
    plan->junctions_total = 0U;
    plan->junctions_remaining = 0U;
    plan->loaded = 0U;
    plan->valid = 0U;
}

uint8_t RoutePlanner_Create(uart_linetracer_position_t current_position, uart_linetracer_route_t target_route,
                            route_plan_t* plan) {
    uint8_t origin_index;
    uint8_t target_index;

    if (plan == NULL) {
        return 0U;
    }

    RoutePlanner_Reset(plan);
    origin_index = RoutePlanner_PositionIndex(current_position);
    target_index = RoutePlanner_RouteIndex(target_route);
    if (origin_index == ROUTE_PLANNER_INDEX_INVALID || target_index == ROUTE_PLANNER_INDEX_INVALID) {
        return 0U;
    }

    plan->origin_index = origin_index;
    plan->target_index = target_index;
    if (target_index > origin_index) {
        plan->common_direction = ROUTE_DIRECTION_RIGHT;
        plan->junctions_total = (uint8_t)(target_index - origin_index);
    } else if (target_index < origin_index) {
        plan->common_direction = ROUTE_DIRECTION_LEFT;
        plan->junctions_total = (uint8_t)(origin_index - target_index);
    } else {
        plan->common_direction = ROUTE_DIRECTION_NONE;
        plan->junctions_total = 0U;
    }

    plan->junctions_remaining = plan->junctions_total;
    plan->phase = ROUTE_PHASE_TO_SOURCE_JUNCTION;
    plan->expected_marker = ROUTE_MARKER_DEST_EXIT;
    plan->valid = 1U;
    return 1U;
}

route_action_t RoutePlanner_OnMarker(route_plan_t* plan) {
    if (plan == NULL || plan->valid == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    switch (plan->phase) {
        case ROUTE_PHASE_TO_SOURCE_JUNCTION:
            if (plan->expected_marker == ROUTE_MARKER_DEST_EXIT) {
                plan->expected_marker = ROUTE_MARKER_SOURCE_JUNCTION;
                return ROUTE_ACTION_GO_STRAIGHT;
            }

            if (plan->expected_marker != ROUTE_MARKER_SOURCE_JUNCTION) {
                return RoutePlanner_Fail(plan);
            }

            if (plan->common_direction == ROUTE_DIRECTION_NONE) {
                plan->phase = ROUTE_PHASE_TO_PICKUP;
                plan->expected_marker = ROUTE_MARKER_PICKUP;
                return ROUTE_ACTION_GO_STRAIGHT;
            }

            plan->phase = ROUTE_PHASE_ON_COMMON_LINE;
            plan->expected_marker = ROUTE_MARKER_COMMON_JUNCTION;
            return (plan->common_direction == ROUTE_DIRECTION_RIGHT) ? ROUTE_ACTION_TURN_RIGHT : ROUTE_ACTION_TURN_LEFT;

        case ROUTE_PHASE_ON_COMMON_LINE:
            if (plan->expected_marker != ROUTE_MARKER_COMMON_JUNCTION || plan->junctions_remaining == 0U) {
                return RoutePlanner_Fail(plan);
            }

            --plan->junctions_remaining;
            if (plan->junctions_remaining != 0U) {
                return ROUTE_ACTION_GO_STRAIGHT;
            }

            plan->phase = ROUTE_PHASE_TO_PICKUP;
            plan->expected_marker = ROUTE_MARKER_PICKUP;
            return (plan->common_direction == ROUTE_DIRECTION_RIGHT) ? ROUTE_ACTION_TURN_LEFT : ROUTE_ACTION_TURN_RIGHT;

        case ROUTE_PHASE_TO_PICKUP:
            if (plan->expected_marker != ROUTE_MARKER_PICKUP) {
                return RoutePlanner_Fail(plan);
            }

            plan->phase = ROUTE_PHASE_WAITING_LOAD;
            plan->expected_marker = ROUTE_MARKER_NONE;
            return ROUTE_ACTION_STOP_AT_PICKUP;

        case ROUTE_PHASE_RETURN_TO_DEST:
            if (plan->expected_marker == ROUTE_MARKER_PICKUP_EXIT) {
                plan->expected_marker = ROUTE_MARKER_RETURN_JUNCTION;
                return ROUTE_ACTION_GO_STRAIGHT;
            }

            if (plan->expected_marker == ROUTE_MARKER_RETURN_JUNCTION) {
                plan->expected_marker = ROUTE_MARKER_DEST;
                return ROUTE_ACTION_GO_STRAIGHT;
            }

            if (plan->expected_marker == ROUTE_MARKER_DEST) {
                plan->phase = ROUTE_PHASE_UNLOADING;
                plan->expected_marker = ROUTE_MARKER_NONE;
                return ROUTE_ACTION_STOP_AT_DEST;
            }

            return RoutePlanner_Fail(plan);

        case ROUTE_PHASE_IDLE:
        case ROUTE_PHASE_WAITING_LOAD:
        case ROUTE_PHASE_UNLOADING:
        case ROUTE_PHASE_COMPLETE:
        case ROUTE_PHASE_ERROR:
        default:
            return RoutePlanner_Fail(plan);
    }
}

route_action_t RoutePlanner_OnLoadOn(route_plan_t* plan) {
    if (plan == NULL || plan->valid == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    if (plan->phase == ROUTE_PHASE_WAITING_LOAD && plan->loaded == 0U) {
        plan->loaded = 1U;
        plan->phase = ROUTE_PHASE_RETURN_TO_DEST;
        plan->expected_marker = ROUTE_MARKER_PICKUP_EXIT;
        return ROUTE_ACTION_TURN_AROUND;
    }

    if (plan->loaded != 0U && (plan->phase == ROUTE_PHASE_RETURN_TO_DEST || plan->phase == ROUTE_PHASE_UNLOADING)) {
        return ROUTE_ACTION_NONE;
    }

    return ROUTE_ACTION_NONE;
}

route_action_t RoutePlanner_OnLoadOff(route_plan_t* plan) {
    if (plan == NULL || plan->valid == 0U) {
        return ROUTE_ACTION_ERROR;
    }

    if (plan->phase == ROUTE_PHASE_RETURN_TO_DEST && plan->loaded != 0U) {
        plan->phase = ROUTE_PHASE_ERROR;
        plan->expected_marker = ROUTE_MARKER_NONE;
        plan->valid = 0U;
        return ROUTE_ACTION_LOAD_LOST;
    }

    if (plan->phase == ROUTE_PHASE_UNLOADING && plan->loaded != 0U) {
        plan->loaded = 0U;
        plan->phase = ROUTE_PHASE_COMPLETE;
        plan->expected_marker = ROUTE_MARKER_NONE;
        return ROUTE_ACTION_JOB_COMPLETE;
    }

    return ROUTE_ACTION_NONE;
}

uart_linetracer_position_t RoutePlanner_TargetDestination(const route_plan_t* plan) {
    uint32_t destination;

    if (plan == NULL || plan->target_index == ROUTE_PLANNER_INDEX_INVALID) {
        return UART_LINETRACER_POSITION_NONE;
    }

    destination = (uint32_t)UART_LINETRACER_POSITION_MIN + plan->target_index;
    return uart_linetracer_position_is_valid(destination) != 0U ? (uart_linetracer_position_t)destination
                                                                : UART_LINETRACER_POSITION_NONE;
}
