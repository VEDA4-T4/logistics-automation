#ifndef UNLOAD_LOGIC_H
#define UNLOAD_LOGIC_H

#include <stdint.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UNLOAD_LOGIC_IDLE = 0,
    UNLOAD_LOGIC_MOVING_TO_RELEASE,
    UNLOAD_LOGIC_WAITING_LOAD_OFF,
    UNLOAD_LOGIC_MOVING_HOME,
    UNLOAD_LOGIC_FAILED
} unload_logic_state_t;

typedef enum {
    UNLOAD_SERVO_OUTPUT_DISABLE = 0,
    UNLOAD_SERVO_OUTPUT_HOME,
    UNLOAD_SERVO_OUTPUT_RELEASE
} unload_servo_output_t;

typedef struct {
    unload_logic_state_t state;
    app_unload_command_t active_command;
    app_unload_result_t pending_result;
    uint32_t started_at_ms;
    uint32_t state_entered_at_ms;
    uint8_t active;
    uint8_t load_present_seen;
    uint8_t result_pending;
} unload_logic_context_t;

void UnloadLogic_Init(unload_logic_context_t* context, uint32_t now_ms);
uint8_t UnloadLogic_Start(unload_logic_context_t* context, const app_unload_command_t* command, uint32_t now_ms);
void UnloadLogic_Abort(unload_logic_context_t* context, uint32_t now_ms, uart_error_t error_code);
void UnloadLogic_Fail(unload_logic_context_t* context, uint32_t now_ms, uart_error_t error_code);
void UnloadLogic_Reset(unload_logic_context_t* context, uint32_t now_ms);
void UnloadLogic_Update(unload_logic_context_t* context, uart_linetracer_load_state_t load_state, uint32_t now_ms);
unload_servo_output_t UnloadLogic_GetServoOutput(const unload_logic_context_t* context);
uint8_t UnloadLogic_GetPendingResult(const unload_logic_context_t* context, app_unload_result_t* result);
void UnloadLogic_AcknowledgeResult(unload_logic_context_t* context);

#ifdef __cplusplus
}
#endif

#endif /* UNLOAD_LOGIC_H */
