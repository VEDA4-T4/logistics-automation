#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>

/* freertos.c의 __weak StartSensorTask를 이 파일의 구현이 덮는다. */
void StartSensorTask(void* argument);

/*
 * ============================================================================
 * 내부 처리 진입점 (StartSensorTask 루프 및 호스트 연동 테스트에서 사용)
 * ============================================================================
 */

/* hc_sr04/sensor_filter 채널 4개를 등록/초기화한다. */
void SensorTask_Init(void);

/* index(0=US1..3=US4) 채널 1개를 측정->필터링->보고->heartbeat 갱신까지 처리한다. */
void SensorTask_PollChannel(uint8_t index);

#endif /* SENSOR_TASK_H */
