#ifndef SAFETY_TASK_H
#define SAFETY_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Strong implementation replacing the weak CubeMX task body in freertos.c. */
void StartSafetyTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_TASK_H */
