#ifndef HC_SR04_H
#define HC_SR04_H

#include <stdint.h>

#include "main.h"
#include "tim.h"

/*
 * ============================================================================
 * HC-SR04 초음파 센서 드라이버
 * ============================================================================
 *
 * 센서 1개당 TIM Input Capture 채널 1개 + TRIG용 GPIO 출력 1개를 사용한다.
 * TRIG 10us 펄스를 보낸 뒤, ECHO 라인의 rising->falling 캡처 간격(us)으로
 * 거리(cm)를 계산한다. FreeRTOS에 의존하지 않는 순수 드라이버 계층이며,
 * SensorTask가 hc_sr04_trigger()/hc_sr04_is_ready()/hc_sr04_read()를 통해
 * 논블로킹으로 사용한다.
 *
 * 전제 조건 (SensorTask 초기화 시 hc_sr04_init으로 등록하기 전에 완료):
 *   - timer는 84MHz/84 = 1MHz(1us tick)로 이미 설정되어 있어야 한다
 *     (PSC=83, 자유 실행 카운터. TRIG 펄스 폭을 이 카운터로 busy-wait한다).
 *   - channel은 Input Capture Direct 모드로 구성되어 있어야 한다.
 *
 * 동시에 여러 센서를 트리거하지 않는다(초음파 간섭 방지). 이 드라이버는
 * 센서별 상태 머신으로 이를 보장하지 않으므로 호출 순서는 SensorTask가
 * 책임진다.
 */

typedef enum {
    HC_SR04_OK = 0,
    HC_SR04_BUSY,        /* 이전 측정이 아직 진행 중 (read/abort로 비우기 전) */
    HC_SR04_TIMEOUT,     /* echo 캡처가 아직(또는 끝내) 완료되지 않음 */
    HC_SR04_OUT_OF_RANGE /* pulse 폭이 유효 범위를 벗어남 */
} hc_sr04_result_t;

typedef enum {
    HC_SR04_STATE_IDLE = 0,
    HC_SR04_STATE_ARMED_RISING,
    HC_SR04_STATE_ARMED_FALLING,
    HC_SR04_STATE_DONE
} hc_sr04_capture_state_t;

typedef struct {
    TIM_HandleTypeDef* timer;
    uint32_t channel;                    /* TIM_CHANNEL_x */
    HAL_TIM_ActiveChannel activeChannel; /* htim->Channel과 비교할 값. channel로부터 파생 */
    GPIO_TypeDef* trigPort;
    uint16_t trigPin;

    volatile hc_sr04_capture_state_t state;
    volatile uint32_t risingCapture;
    volatile uint32_t pulseWidthUs;
    volatile uint8_t captureReady;
} hc_sr04_sensor_t;

/* echo 폭 유효 범위. 이 밖은 HC_SR04_OUT_OF_RANGE로 판정한다. */
#define HC_SR04_TRIG_PULSE_US 10U
#define HC_SR04_MIN_PULSE_US 60U    /* 약 1cm 미만은 노이즈로 간주 */
#define HC_SR04_MAX_PULSE_US 38000U /* 약 400cm, echo timeout과 동일 기준 */

/*
 * timer/channel/trigPort/trigPin을 등록하고 캡처 인터럽트를 시작한다.
 * 최대 4개 센서까지 등록 가능(HC_SR04_MAX_SENSORS). timer는 이미
 * 1MHz로 설정되어 있어야 한다.
 */
void hc_sr04_init(hc_sr04_sensor_t* sensor, TIM_HandleTypeDef* timer, uint32_t channel, GPIO_TypeDef* trigPort,
                  uint16_t trigPin);

/*
 * TRIG 펄스를 보내고 rising edge 캡처를 arm한다(논블로킹, 10us busy-wait 제외).
 * 이전 측정 결과를 hc_sr04_read()/hc_sr04_abort()로 비우기 전에는 HC_SR04_BUSY.
 */
hc_sr04_result_t hc_sr04_trigger(hc_sr04_sensor_t* sensor);

/* 캡처(rising+falling) 완료 여부. 폴링용. */
uint8_t hc_sr04_is_ready(const hc_sr04_sensor_t* sensor);

/*
 * 캡처 결과를 읽어 distanceCm에 저장하고 센서를 IDLE로 되돌린다.
 * 아직 준비되지 않았으면 HC_SR04_TIMEOUT, pulse 폭이 범위를 벗어나면
 * HC_SR04_OUT_OF_RANGE(이 경우도 IDLE로 되돌아간다).
 */
hc_sr04_result_t hc_sr04_read(hc_sr04_sensor_t* sensor, uint16_t* distanceCm);

/* 캡처를 기다리다 타임아웃된 경우 등, 측정을 포기하고 IDLE로 되돌린다. */
void hc_sr04_abort(hc_sr04_sensor_t* sensor);

/* pulse_us를 cm로 변환한다: distance_cm = pulse_us * 0.0343 / 2 */
uint16_t hc_sr04_pulse_to_cm(uint32_t pulseUs);

/*
 * 공용 HAL 콜백. hc_sr04_init으로 등록된 센서 중 htim/활성 채널이 일치하는
 * 것을 찾아 rising/falling 캡처를 처리한다. ISR 컨텍스트에서 호출된다.
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef* htim);

#endif /* HC_SR04_H */
