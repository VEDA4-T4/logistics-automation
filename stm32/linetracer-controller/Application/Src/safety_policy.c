#include "safety_policy.h"

#include <stddef.h>

#include "logistics/contracts/uart/linetracer_commands.h"

uint8_t SafetyPolicy_LineLossApplies(const app_control_snapshot_t* snapshot) {
    if (snapshot == NULL || uart_linetracer_job_id_is_valid(snapshot->job_id) == 0U ||
        uart_linetracer_route_is_valid(snapshot->route_id) == 0U) {
        return 0U;
    }

    // Line departure is expected while CORRECTING.
    // ControlTask's turn timeout remains the safety backstop.
    return (snapshot->state == UART_LINETRACER_STATE_FOLLOWING_LINE) ? 1U : 0U;
}

uint8_t SafetyPolicy_IsMomentaryRemoteEstop(const app_safety_event_t* event) {
    if (event == NULL) {
        return 0U;
    }

    return (event->type == APP_SAFETY_EVENT_EMERGENCY_STOP && event->source_task == APP_TASK_COMM_RX &&
            event->active != 0U)
               ? 1U
               : 0U;
}
