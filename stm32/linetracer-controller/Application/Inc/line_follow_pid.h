#ifndef LINE_FOLLOW_PID_H
#define LINE_FOLLOW_PID_H

#include <stdint.h>

#include "app_timing.h"
#include "motor_control_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINE_FOLLOW_PID_UPDATE_PERIOD_MS 10U
#define LINE_FOLLOW_PID_LEFT_BASE_PWM MOTOR_CONTROL_LEFT_BASE_PWM
#define LINE_FOLLOW_PID_RIGHT_BASE_PWM MOTOR_CONTROL_RIGHT_BASE_PWM
#define LINE_FOLLOW_PID_KP 0.375F
#define LINE_FOLLOW_PID_KI 0.0F
#define LINE_FOLLOW_PID_KD 0.03125F
#define LINE_FOLLOW_PID_CORRECTION_LIMIT 200.0F
#define LINE_FOLLOW_PID_INTEGRAL_LIMIT 250.0F

#if (APP_TIMING_CONTROL_PERIOD_MS != LINE_FOLLOW_PID_UPDATE_PERIOD_MS)
#error "Line-follow PID constants require a 10 ms ControlTask period"
#endif

typedef struct {
    int16_t previous_error;
    float integral;
    uint32_t last_update_ms;
    uint8_t initialized;
} line_follow_pid_t;

void LineFollowPid_Init(line_follow_pid_t* pid);
void LineFollowPid_Reset(line_follow_pid_t* pid);
int16_t LineFollowPid_Update(line_follow_pid_t* pid, int16_t error, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LINE_FOLLOW_PID_H */
