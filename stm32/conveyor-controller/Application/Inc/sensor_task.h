#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>

#include "app_comm_tx.h"

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

/*
 * ============================================================================
 * HealthTask가 센서 갱신 지연을 판정하는 데 쓰는 조회 함수
 * ============================================================================
 */

/* 등록된 센서 채널 수(현재 4: US1..US4). */
uint8_t SensorTask_GetChannelCount(void);

/* index 채널의 마지막 유효 표본 시각(HAL_GetTick 기준). 부팅 후 아직 유효
 * 표본이 없으면 0. */
uint32_t SensorTask_GetChannelLastSampleTick(uint8_t index);

/* index 채널이 보고하는 대상 공정(투입/분류). */
comm_tx_channel_t SensorTask_GetChannelProcess(uint8_t index);

#endif /* SENSOR_TASK_H */
