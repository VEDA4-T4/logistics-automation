#include "hc_sr04.h"

#include <stddef.h>

#define HC_SR04_MAX_SENSORS 4U

static hc_sr04_sensor_t* registeredSensors[HC_SR04_MAX_SENSORS];
static uint8_t registeredSensorCount;

static HAL_TIM_ActiveChannel hc_sr04_active_channel_for(uint32_t channel) {
    /* TIM_CHANNEL_1=0x00, _2=0x04, _3=0x08, _4=0x0C -> ACTIVE_CHANNEL_1..4 = 1U<<0..3U */
    return (HAL_TIM_ActiveChannel)(1U << (channel >> 2U));
}

static void hc_sr04_set_polarity(const hc_sr04_sensor_t* sensor, uint32_t polarity) {
    TIM_IC_InitTypeDef icConfig = { 0 };

    icConfig.ICPolarity = polarity;
    icConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
    icConfig.ICPrescaler = TIM_ICPSC_DIV1;
    icConfig.ICFilter = 0U;

    (void)HAL_TIM_IC_ConfigChannel(sensor->timer, &icConfig, sensor->channel);
}

static void hc_sr04_delay_us(TIM_HandleTypeDef* timer, uint32_t us) {
    uint32_t start = __HAL_TIM_GET_COUNTER(timer);

    while ((__HAL_TIM_GET_COUNTER(timer) - start) < us) {
        /* 1MHz 자유 실행 카운터를 이용한 busy-wait. TRIG 펄스 폭(10us)에만 사용한다. */
    }
}

void hc_sr04_init(hc_sr04_sensor_t* sensor, TIM_HandleTypeDef* timer, uint32_t channel, GPIO_TypeDef* trigPort,
                  uint16_t trigPin) {
    if (sensor == NULL) {
        return;
    }

    sensor->timer = timer;
    sensor->channel = channel;
    sensor->activeChannel = hc_sr04_active_channel_for(channel);
    sensor->trigPort = trigPort;
    sensor->trigPin = trigPin;
    sensor->state = HC_SR04_STATE_IDLE;
    sensor->risingCapture = 0U;
    sensor->pulseWidthUs = 0U;
    sensor->captureReady = 0U;

    HAL_GPIO_WritePin(trigPort, trigPin, GPIO_PIN_RESET);
    (void)HAL_TIM_IC_Start_IT(timer, channel);

    if (registeredSensorCount < HC_SR04_MAX_SENSORS) {
        registeredSensors[registeredSensorCount] = sensor;
        registeredSensorCount++;
    }
}

hc_sr04_result_t hc_sr04_trigger(hc_sr04_sensor_t* sensor) {
    if (sensor == NULL) {
        return HC_SR04_BUSY;
    }

    if ((sensor->state == HC_SR04_STATE_ARMED_RISING) || (sensor->state == HC_SR04_STATE_ARMED_FALLING)) {
        return HC_SR04_BUSY;
    }

    sensor->captureReady = 0U;
    sensor->pulseWidthUs = 0U;
    hc_sr04_set_polarity(sensor, TIM_ICPOLARITY_RISING);
    sensor->state = HC_SR04_STATE_ARMED_RISING;

    HAL_GPIO_WritePin(sensor->trigPort, sensor->trigPin, GPIO_PIN_SET);
    hc_sr04_delay_us(sensor->timer, HC_SR04_TRIG_PULSE_US);
    HAL_GPIO_WritePin(sensor->trigPort, sensor->trigPin, GPIO_PIN_RESET);

    return HC_SR04_OK;
}

uint8_t hc_sr04_is_ready(const hc_sr04_sensor_t* sensor) {
    return ((sensor != NULL) && (sensor->captureReady != 0U)) ? 1U : 0U;
}

hc_sr04_result_t hc_sr04_read(hc_sr04_sensor_t* sensor, uint16_t* distanceCm) {
    uint32_t pulseUs;

    if ((sensor == NULL) || (distanceCm == NULL) || (sensor->captureReady == 0U)) {
        return HC_SR04_TIMEOUT;
    }

    pulseUs = sensor->pulseWidthUs;
    sensor->state = HC_SR04_STATE_IDLE;
    sensor->captureReady = 0U;

    if ((pulseUs < HC_SR04_MIN_PULSE_US) || (pulseUs > HC_SR04_MAX_PULSE_US)) {
        return HC_SR04_OUT_OF_RANGE;
    }

    *distanceCm = hc_sr04_pulse_to_cm(pulseUs);
    return HC_SR04_OK;
}

void hc_sr04_abort(hc_sr04_sensor_t* sensor) {
    if (sensor == NULL) {
        return;
    }

    sensor->state = HC_SR04_STATE_IDLE;
    sensor->captureReady = 0U;
}

uint16_t hc_sr04_pulse_to_cm(uint32_t pulseUs) {
    /* distance_cm = pulse_us * 0.0343 / 2 = pulse_us * 343 / 20000 */
    uint32_t distanceCm = (pulseUs * 343U) / 20000U;

    if (distanceCm > 0xFFFFU) {
        distanceCm = 0xFFFFU;
    }

    return (uint16_t)distanceCm;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef* htim) {
    uint8_t i;
    hc_sr04_sensor_t* sensor;
    uint32_t captured;

    for (i = 0U; i < registeredSensorCount; i++) {
        sensor = registeredSensors[i];

        if ((sensor->timer != htim) || (htim->Channel != sensor->activeChannel)) {
            continue;
        }

        if (sensor->state == HC_SR04_STATE_ARMED_RISING) {
            captured = HAL_TIM_ReadCapturedValue(htim, sensor->channel);
            sensor->risingCapture = captured;
            sensor->state = HC_SR04_STATE_ARMED_FALLING;
            hc_sr04_set_polarity(sensor, TIM_ICPOLARITY_FALLING);
        } else if (sensor->state == HC_SR04_STATE_ARMED_FALLING) {
            captured = HAL_TIM_ReadCapturedValue(htim, sensor->channel);
            sensor->pulseWidthUs = captured - sensor->risingCapture;
            sensor->state = HC_SR04_STATE_DONE;
            sensor->captureReady = 1U;
        }
        /* IDLE/DONE 상태에서의 캡처는 노이즈로 간주하고 무시한다. */

        return;
    }
}
