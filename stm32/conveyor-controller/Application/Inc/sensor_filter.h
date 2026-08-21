#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

/*
 * ============================================================================
 * 센서 측정 필터 (median filter + fault latch)
 * ============================================================================
 *
 * HC-SR04 원시 거리값을 잡음 제거만 거쳐 그대로 보고한다. HAL/FreeRTOS에
 * 의존하지 않는 순수 로직이며, SensorTask가 매 측정 주기마다
 * sensor_filter_record_sample() 또는 sensor_filter_record_fault()를 호출한다.
 *
 *   median filter : 최근 3개 표본의 중앙값을 보고값으로 사용해 순간 노이즈를
 *                   제거한다. 이건 측정값 자체를 다듬는 것이지 업무 판단이 아니다.
 *   fault latch   : 무효 측정(timeout/out-of-range)이 연속 10회면 즉시 FAULT로
 *                   전환한다(안전 신호이므로 지연 없이 반영). 복구는 유효 표본
 *                   3회를 요구해 문턱에서 FAULT가 깜빡이는 것을 막는다.
 *
 * 상자 존재 판단(있음/없음)은 여기서 하지 않는다.
 * ----------------------------------------------------------------------------
 * 예전에는 진입/이탈 임계값 + 히스테리시스 + 디바운스로 CLEAR/DETECTED를
 * 확정해 보고했다. 그 판정은 중앙 서버로 옮겼다 - 임계값을 바꾸려고 이 헤더의
 * #define을 고치고 펌웨어를 다시 굽는 대신, 서버 설정(server.ini의
 * [sensor_detection])만 바꾸면 되게 하기 위해서다. STM32는 거리값과 측정
 * 건전성만 올린다.
 */

typedef struct {
    uint16_t samples[3];
    uint8_t sampleCount;
    uint8_t sampleIndex;

    uint8_t faulted;           /* 1이면 UART_SENSOR_FAULT를 보고한다 */
    uint8_t consecutiveFaults; /* FAULT 진입 카운트 */
    uint8_t consecutiveValid;  /* FAULT 복구 카운트 */

    uint16_t lastDistanceCm;
} sensor_filter_t;

#define SENSOR_FILTER_FAULT_THRESHOLD 10U
#define SENSOR_FILTER_RECOVERY_COUNT 3U

void sensor_filter_init(sensor_filter_t* filter);

/* 유효한 거리 측정값을 반영한다. 연속 fault 카운트를 리셋한다. */
void sensor_filter_record_sample(sensor_filter_t* filter, uint16_t distanceCm);

/* echo timeout/out-of-range 등 무효 측정을 반영한다. */
void sensor_filter_record_fault(sensor_filter_t* filter);

/* 현재 측정 건전성(UART_SENSOR_OK 또는 UART_SENSOR_FAULT)을 반환한다. */
uint8_t sensor_filter_get_state(const sensor_filter_t* filter);

/* 현재 보고할 거리(cm)를 반환한다. FAULT 상태면 UART_SENSOR_DISTANCE_UNKNOWN. */
uint16_t sensor_filter_get_distance_cm(const sensor_filter_t* filter);

#endif /* SENSOR_FILTER_H */
