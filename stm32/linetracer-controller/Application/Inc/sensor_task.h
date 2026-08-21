#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdbool.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strong implementation replacing the weak CubeMX task body in freertos.c. */
void StartSensorTask(void* argument);

/* Non-blocking access to the most recent normalized sensor snapshot. */
bool SensorTask_GetLatest(app_sensor_snapshot_t* snapshot);

/* Drop line-follow history after a pivot without resetting marker or safety state. */
void SensorTask_RequestLineTrackingReset(void);

typedef enum {
    SENSOR_TASK_FSR_BASELINE_FOR_LOAD_ON = 0,
    SENSOR_TASK_FSR_BASELINE_FOR_LOAD_OFF
} sensor_task_fsr_baseline_mode_t;

/* Capture a fresh stopped-vehicle FSR reference on the SensorTask thread. */
void SensorTask_RequestFsrBaselineCapture(sensor_task_fsr_baseline_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_TASK_H */
