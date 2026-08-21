#include "line_follow_pid.h"

#include <stddef.h>

static float LineFollowPid_Clamp(float value, float limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

void LineFollowPid_Init(line_follow_pid_t *pid)
{
    LineFollowPid_Reset(pid);
}

void LineFollowPid_Reset(line_follow_pid_t *pid)
{
    if (pid == NULL) {
        return;
    }

    pid->previous_error = 0;
    pid->integral = 0.0F;
    pid->last_update_ms = 0U;
    pid->initialized = 0U;
}

int16_t LineFollowPid_Update(line_follow_pid_t *pid,
                             int16_t error,
                             uint32_t now_ms)
{
    float proportional;
    float derivative = 0.0F;
    float correction;

    if (pid == NULL) {
        return 0;
    }

    proportional = LINE_FOLLOW_PID_KP * (float)error;

    if (pid->initialized != 0U) {
        uint32_t elapsed_ms = now_ms - pid->last_update_ms;
        float period_scale;

        if (elapsed_ms == 0U) {
            elapsed_ms = LINE_FOLLOW_PID_UPDATE_PERIOD_MS;
        }

        period_scale =
            (float)elapsed_ms / (float)LINE_FOLLOW_PID_UPDATE_PERIOD_MS;
        pid->integral += LINE_FOLLOW_PID_KI * (float)error * period_scale;
        pid->integral =
            LineFollowPid_Clamp(pid->integral,
                                LINE_FOLLOW_PID_INTEGRAL_LIMIT);
        derivative =
            LINE_FOLLOW_PID_KD *
            ((float)error - (float)pid->previous_error) / period_scale;
    } else {
        pid->integral = LineFollowPid_Clamp(
            LINE_FOLLOW_PID_KI * (float)error,
            LINE_FOLLOW_PID_INTEGRAL_LIMIT);
        pid->initialized = 1U;
    }

    correction = proportional + pid->integral + derivative;
    correction =
        LineFollowPid_Clamp(correction, LINE_FOLLOW_PID_CORRECTION_LIMIT);

    pid->previous_error = error;
    pid->last_update_ms = now_ms;
    return (int16_t)correction;
}
