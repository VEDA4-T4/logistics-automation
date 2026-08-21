#ifndef UNLOAD_TASK_H
#define UNLOAD_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Strong implementation replacing the generated weak UnloadTask loop. */
void StartUnloadTask(void* argument);

/* Safe to call from SafetyTask: disables the servo before queue processing. */
uint8_t UnloadTask_RequestSafetyStop(void);
/* Used by ControlTask to stop an active unload as soon as STOP is accepted. */
uint8_t UnloadTask_RequestAbort(void);

#ifdef __cplusplus
}
#endif

#endif /* UNLOAD_TASK_H */
