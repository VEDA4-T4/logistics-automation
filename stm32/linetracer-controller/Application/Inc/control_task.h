#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

#include <stdbool.h>

#include "app_messages.h"

#define CONTROL_TASK_SNAPSHOT_API_AVAILABLE 1U

#ifdef __cplusplus
extern "C" {
#endif

void StartControlTask(void* argument);
bool ControlTask_GetLatest(app_control_snapshot_t* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_TASK_H */
