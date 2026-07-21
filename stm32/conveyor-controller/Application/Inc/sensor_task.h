#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

/* freertos.c의 __weak StartSensorTask를 이 파일의 구현이 덮는다. */
void StartSensorTask(void* argument);

#endif /* SENSOR_TASK_H */
