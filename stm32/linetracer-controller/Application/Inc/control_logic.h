#ifndef CONTROL_LOGIC_H
#define CONTROL_LOGIC_H

#include <stdint.h>

#include "app_messages.h"
#include "route_planner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    linetracer_control_state_t state;
    linetracer_control_state_t resume_state;
    linetracer_stop_reason_t stop_reason;
    uart_linetracer_position_t current_position;
    uart_linetracer_route_t active_route;
    uint32_t state_entered_at_ms;
    uint32_t last_command_at_ms;
    uint16_t active_job_id;
    uint8_t route_active;
    uint8_t resume_valid;
    uint8_t safety_latched;
    uint8_t safety_error_code;
    route_plan_t route_plan;
    route_action_t pending_route_action;
} control_context_t;

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
uint8_t ControlLogic_CompleteTurn(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_HandleMarker(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_HandleLoadOn(control_context_t* context, uint32_t now_ms);
route_action_t ControlLogic_HandleLoadOff(control_context_t* context, uint32_t now_ms,
                                          control_job_completion_t* completion);
control_job_completion_t ControlLogic_CompleteJob(control_context_t* context, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_LOGIC_H */
