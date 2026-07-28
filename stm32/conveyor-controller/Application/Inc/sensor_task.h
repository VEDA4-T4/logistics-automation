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
 * HealthTask가 채널 서비스 지연을 판정하는 데 쓰는 조회 함수
 * ============================================================================
 */

/* 등록된 센서 채널 수(현재 4: US1..US4). */
uint8_t SensorTask_GetChannelCount(void);

/* index 채널을 마지막으로 폴링한 시각(HAL_GetTick 기준). 부팅 후 아직 한 번도
 * 폴링하지 않았으면 0.
 *
 * 측정 성공 여부와 무관하게 갱신된다 - HealthTask가 판정하려는 것은 "SensorTask가
 * 이 채널을 계속 서비스하고 있는가"이지 "센서가 유효한 echo를 받았는가"가 아니기
 * 때문이다. 후자(echo 누락)는 HC-SR04의 정상 특성이라 health 이상이 아니며,
 * 지속되면 sensor_filter가 UART_SENSOR_FAULT로 따로 보고한다. */
uint32_t SensorTask_GetChannelLastPollTick(uint8_t index);

/* index 채널이 보고하는 대상 공정(투입/분류). */
comm_tx_channel_t SensorTask_GetChannelProcess(uint8_t index);

/* index 채널의 UART sensorId(UART_INPUT_SENSOR_ID_1 또는
 * UART_SORTING_SENSOR_ID_1..3). SENSOR_STALE HEALTH EVENT에 실어 보내
 * 분류 쪽 센서 3개(US2/US3/US4)를 서로 구분하는 데 쓴다. */
uint8_t SensorTask_GetChannelSensorId(uint8_t index);

#endif /* SENSOR_TASK_H */
