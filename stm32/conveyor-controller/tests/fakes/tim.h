#ifndef INPUT_MOTOR_TEST_TIM_H
#define INPUT_MOTOR_TEST_TIM_H

#include <stdint.h>

#include "main.h"

typedef enum {
    HAL_TIM_ACTIVE_CHANNEL_CLEARED = 0x00U,
    HAL_TIM_ACTIVE_CHANNEL_1 = 0x01U,
    HAL_TIM_ACTIVE_CHANNEL_2 = 0x02U,
    HAL_TIM_ACTIVE_CHANNEL_3 = 0x04U,
    HAL_TIM_ACTIVE_CHANNEL_4 = 0x08U
} HAL_TIM_ActiveChannel;

typedef struct {
    uint32_t autoreload;
    uint32_t compare;
    uint32_t counter;
    HAL_TIM_ActiveChannel Channel;
} TIM_HandleTypeDef;

typedef struct {
    uint32_t ICPolarity;
    uint32_t ICSelection;
    uint32_t ICPrescaler;
    uint32_t ICFilter;
} TIM_IC_InitTypeDef;

extern TIM_HandleTypeDef htim1;

/* 실제 HAL과 동일한 간격(0x00, 0x04, 0x08, 0x0C)을 유지한다.
 * hc_sr04 드라이버가 이 간격으로 activeChannel을 역산하기 때문이다. */
#define TIM_CHANNEL_1 0x00000000U
#define TIM_CHANNEL_2 0x00000004U
#define TIM_CHANNEL_3 0x00000008U
#define TIM_CHANNEL_4 0x0000000CU

#define TIM_ICPOLARITY_RISING 0x00000000U
#define TIM_ICPOLARITY_FALLING 0x00000002U
#define TIM_ICSELECTION_DIRECTTI 0x00000001U
#define TIM_ICPSC_DIV1 0x00000000U

#define __HAL_TIM_SET_COMPARE(handle, channel, value) \
    do {                                              \
        (void)(channel);                              \
        (handle)->compare = (uint32_t)(value);        \
    } while (0)

#define __HAL_TIM_GET_AUTORELOAD(handle) ((handle)->autoreload)

/* 실제 카운터 대신 읽을 때마다 1씩 전진시켜, hc_sr04의 busy-wait 루프가
 * 호스트 테스트에서도 유한 시간에 끝나도록 한다(테스트 파일이 정의). */
uint32_t hc_sr04_test_tick_counter(TIM_HandleTypeDef* handle);
#define __HAL_TIM_GET_COUNTER(handle) hc_sr04_test_tick_counter(handle)

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* handle, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef* handle, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_IC_ConfigChannel(TIM_HandleTypeDef* handle, const TIM_IC_InitTypeDef* config,
                                           uint32_t channel);
uint32_t HAL_TIM_ReadCapturedValue(const TIM_HandleTypeDef* handle, uint32_t channel);

#endif /* INPUT_MOTOR_TEST_TIM_H */
