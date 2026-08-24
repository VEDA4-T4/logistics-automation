#ifndef ROUTE_PLANNER_H
#define ROUTE_PLANNER_H

#include <stdint.h>

#include "logistics/contracts/uart/linetracer_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_PLANNER_INDEX_INVALID 0xFFU

typedef enum {
    ROUTE_PHASE_IDLE = 0,
    ROUTE_PHASE_TO_SOURCE_JUNCTION,
    ROUTE_PHASE_ON_COMMON_LINE,
    ROUTE_PHASE_TO_C_PICKUP_TURN,
    ROUTE_PHASE_TO_PICKUP,
    ROUTE_PHASE_WAITING_LOAD,
    ROUTE_PHASE_TO_C_RETURN_JUNCTION,
    ROUTE_PHASE_TO_TARGET_UNLOAD,
    ROUTE_PHASE_UNLOADING,
    ROUTE_PHASE_COMPLETE,
    ROUTE_PHASE_ERROR
} route_phase_t;

typedef enum { ROUTE_DIRECTION_NONE = 0, ROUTE_DIRECTION_LEFT, ROUTE_DIRECTION_RIGHT } route_direction_t;

typedef enum {
    ROUTE_MARKER_NONE = 0,
    ROUTE_MARKER_DEST_EXIT,
    ROUTE_MARKER_SOURCE_JUNCTION,
    ROUTE_MARKER_COMMON_JUNCTION,
    ROUTE_MARKER_C_PICKUP_TURN,
    ROUTE_MARKER_PICKUP,
    ROUTE_MARKER_C_RETURN_JUNCTION,
    ROUTE_MARKER_TARGET_JUNCTION,
    /* Retained for control-message compatibility; the direct delivery plan does not emit them. */
    ROUTE_MARKER_PICKUP_EXIT,
    ROUTE_MARKER_RETURN_JUNCTION,
    ROUTE_MARKER_DEST
} route_expected_marker_t;

typedef enum {
    ROUTE_ACTION_NONE = 0,
    ROUTE_ACTION_GO_STRAIGHT,
    ROUTE_ACTION_TURN_LEFT,
    ROUTE_ACTION_TURN_RIGHT,
    ROUTE_ACTION_TURN_AROUND,
    ROUTE_ACTION_REVERSE,
    ROUTE_ACTION_STOP_AT_PICKUP,
    ROUTE_ACTION_STOP_AT_DEST,
    ROUTE_ACTION_JOB_COMPLETE,
    ROUTE_ACTION_LOAD_LOST,
    ROUTE_ACTION_ERROR
} route_action_t;

typedef struct {
    route_phase_t phase;
    route_direction_t common_direction;
    route_expected_marker_t expected_marker;
    uint8_t origin_index;
    uint8_t target_index;
    uint8_t junctions_total;
    uint8_t junctions_remaining;
    uint8_t loaded;
    uint8_t valid;
} route_plan_t;

void RoutePlanner_Reset(route_plan_t* plan);
uint8_t RoutePlanner_Create(uart_linetracer_position_t current_position, uart_linetracer_route_t target_route,
                            route_plan_t* plan);
route_action_t RoutePlanner_OnMarker(route_plan_t* plan);
route_action_t RoutePlanner_OnLoadOn(route_plan_t* plan);
route_action_t RoutePlanner_OnLoadOff(route_plan_t* plan);
uart_linetracer_position_t RoutePlanner_TargetDestination(const route_plan_t* plan);

#ifdef __cplusplus
}
#endif

#endif /* ROUTE_PLANNER_H */
