#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H

#include <stdint.h>

#include "logistics/contracts/uart_protocol.h"

/*
 * ============================================================================
 * 센서 판정 필터 (median filter + hysteresis + debounce + fault latch)
 * ============================================================================
 *
 * HC-SR04 원시 거리값을 uart_sensor_state_t(CLEAR/DETECTED/FAULT)로 판정한다.
 * HAL/FreeRTOS에 의존하지 않는 순수 로직이며, SensorTask가 매 측정 주기마다
 * sensor_filter_record_sample() 또는 sensor_filter_record_fault()를 호출한다.
 *
 *   median filter : 최근 3개 표본의 중앙값을 판정에 사용해 순간 노이즈를 제거한다.
 *   hysteresis    : 진입(ENTER) 임계값과 이탈(EXIT) 임계값을 분리해 문턱 근처
 *                   채터링을 막는다(진입 <= 10cm, 이탈 > 12cm 이면 CLEAR로 판정).
 *   debounce      : 후보 상태가 3회 연속 동일해야 confirmedState에 반영한다.
 *   fault latch   : 무효 측정(timeout/out-of-range)이 연속 10회면 즉시 FAULT로
 *                   전환한다(디바운스 없이 즉시 반영 - 안전 신호이므로).
 */

typedef struct {
    uint16_t samples[3];
    uint8_t sampleCount;
    uint8_t sampleIndex;

    uint8_t confirmedState;   /* uart_sensor_state_t: 보고에 사용하는 실제 상태 */
    uint8_t pendingCandidate; /* uart_sensor_state_t: 디바운스 중인 후보 */
    uint8_t consecutiveMatches;
    uint8_t consecutiveFaults;

    uint16_t lastDistanceCm;
} sensor_filter_t;

#define SENSOR_FILTER_ENTER_THRESHOLD_CM 10U
#define SENSOR_FILTER_EXIT_THRESHOLD_CM 12U
#define SENSOR_FILTER_DEBOUNCE_COUNT 3U
#define SENSOR_FILTER_FAULT_THRESHOLD 10U

void sensor_filter_init(sensor_filter_t* filter);

/* 유효한 거리 측정값을 반영한다. 연속 fault 카운트를 리셋한다. */
void sensor_filter_record_sample(sensor_filter_t* filter, uint16_t distanceCm);

/* echo timeout/out-of-range 등 무효 측정을 반영한다. */
void sensor_filter_record_fault(sensor_filter_t* filter);

/* 현재 보고 상태(uart_sensor_state_t)를 반환한다. */
uint8_t sensor_filter_get_state(const sensor_filter_t* filter);

/* 현재 보고할 거리(cm)를 반환한다. FAULT 상태면 UART_SENSOR_DISTANCE_UNKNOWN. */
uint16_t sensor_filter_get_distance_cm(const sensor_filter_t* filter);

#endif /* SENSOR_FILTER_H */
