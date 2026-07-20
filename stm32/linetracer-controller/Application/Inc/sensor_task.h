#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdbool.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strong implementation replacing the weak CubeMX task body in freertos.c. */
void StartSensorTask(void *argument);

/* Non-blocking access to the most recent normalized sensor snapshot. */
bool SensorTask_GetLatest(app_sensor_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_TASK_H */
