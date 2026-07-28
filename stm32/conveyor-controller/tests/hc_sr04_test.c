#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hc_sr04.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "tim.h"

/* input_motor_tb6612_test.c와 fakes/tim.h를 공유하므로 extern 요구사항을 채워야 한다.
 * 이 테스트에서는 사용하지 않는다. */
TIM_HandleTypeDef htim1 = { 0 };

static TIM_HandleTypeDef testTimerA;
static TIM_HandleTypeDef testTimerB;
static GPIO_TypeDef testTrigPortA;
static GPIO_TypeDef testTrigPortB;

static hc_sr04_sensor_t sensorOnA; /* testTimerA, TIM_CHANNEL_2 */
static hc_sr04_sensor_t sensorOnB; /* testTimerB, TIM_CHANNEL_1 */

static GPIO_TypeDef* lastGpioPort;
static uint16_t lastGpioPin;
static GPIO_PinState lastGpioState;
static uint32_t gpioWriteCalls;

static uint32_t icStartCalls;
static TIM_HandleTypeDef* lastIcStartTimer;
static uint32_t lastIcStartChannel;

static uint32_t icConfigCalls;
static uint32_t lastIcConfigPolarity;

static uint32_t nextCapturedValue;

void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState state) {
    lastGpioPort = port;
    lastGpioPin = pin;
    lastGpioState = state;
    gpioWriteCalls++;
}

HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef* handle, uint32_t channel) {
    icStartCalls++;
    lastIcStartTimer = handle;
    lastIcStartChannel = channel;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_IC_ConfigChannel(TIM_HandleTypeDef* handle, const TIM_IC_InitTypeDef* config,
                                           uint32_t channel) {
    (void)handle;
    (void)channel;
    icConfigCalls++;
    lastIcConfigPolarity = config->ICPolarity;
    return HAL_OK;
}

uint32_t HAL_TIM_ReadCapturedValue(const TIM_HandleTypeDef* handle, uint32_t channel) {
    (void)handle;
    (void)channel;
    return nextCapturedValue;
}

/* fakes/tim.h의 __HAL_TIM_GET_COUNTER: 읽을 때마다 1씩 전진시켜
 * hc_sr04의 TRIG busy-wait이 호스트 테스트에서도 유한 시간에 끝나게 한다. */
uint32_t hc_sr04_test_tick_counter(TIM_HandleTypeDef* handle) {
    handle->counter++;
    return handle->counter;
}

static void reset_call_observations(void) {
    lastGpioPort = NULL;
    lastGpioPin = 0U;
    lastGpioState = GPIO_PIN_RESET;
    gpioWriteCalls = 0U;
    icStartCalls = 0U;
    lastIcStartTimer = NULL;
    lastIcStartChannel = 0U;
    icConfigCalls = 0U;
    lastIcConfigPolarity = 0U;
    nextCapturedValue = 0U;
}

static void test_init_registers_sensor_and_starts_capture(void) {
    reset_call_observations();
    testTimerA.counter = 0U;

    hc_sr04_init(&sensorOnA, &testTimerA, TIM_CHANNEL_2, &testTrigPortA, 0x0020U);

    assert(icStartCalls == 1U);
    assert(lastIcStartTimer == &testTimerA);
    assert(lastIcStartChannel == TIM_CHANNEL_2);
    assert(gpioWriteCalls == 1U); /* TRIG를 LOW로 리셋 */
    assert(lastGpioPort == &testTrigPortA);
    assert(lastGpioState == GPIO_PIN_RESET);
    assert(sensorOnA.state == HC_SR04_STATE_IDLE);
    assert(sensorOnA.activeChannel == HAL_TIM_ACTIVE_CHANNEL_2);

    reset_call_observations();
    testTimerB.counter = 0U;

    hc_sr04_init(&sensorOnB, &testTimerB, TIM_CHANNEL_1, &testTrigPortB, 0x0010U);

    assert(icStartCalls == 1U);
    assert(lastIcStartTimer == &testTimerB);
    assert(sensorOnB.activeChannel == HAL_TIM_ACTIVE_CHANNEL_1);
}

static void test_trigger_pulses_trig_and_arms_rising(void) {
    hc_sr04_result_t result;

    reset_call_observations();
    result = hc_sr04_trigger(&sensorOnA);

    assert(result == HC_SR04_OK);
    assert(sensorOnA.state == HC_SR04_STATE_ARMED_RISING);
    assert(icConfigCalls == 1U);
    assert(lastIcConfigPolarity == TIM_ICPOLARITY_RISING);
    assert(gpioWriteCalls == 2U); /* HIGH 후 LOW */
    assert(lastGpioPort == &testTrigPortA);
    assert(lastGpioState == GPIO_PIN_RESET); /* 마지막 호출은 펄스를 끝내는 LOW */

    hc_sr04_abort(&sensorOnA);
    assert(sensorOnA.state == HC_SR04_STATE_IDLE);
}

static void test_trigger_rejects_while_armed(void) {
    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_OK);

    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_BUSY);
    assert(gpioWriteCalls == 0U);
    assert(icConfigCalls == 0U);

    hc_sr04_abort(&sensorOnA);
}

static void test_capture_sequence_computes_distance(void) {
    uint16_t distanceCm;

    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_OK);
    assert(hc_sr04_is_ready(&sensorOnA) == 0U);

    /* rising edge 캡처 */
    nextCapturedValue = 1000U;
    testTimerA.Channel = HAL_TIM_ACTIVE_CHANNEL_2;
    HAL_TIM_IC_CaptureCallback(&testTimerA);

    assert(sensorOnA.state == HC_SR04_STATE_ARMED_FALLING);
    assert(sensorOnA.risingCapture == 1000U);
    assert(icConfigCalls == 2U); /* trigger()에서 RISING 1회 + 여기서 FALLING 1회 */
    assert(lastIcConfigPolarity == TIM_ICPOLARITY_FALLING);
    assert(hc_sr04_is_ready(&sensorOnA) == 0U);

    /* falling edge 캡처: pulse = 21000us -> distance = 21000*343/20000 = 360cm */
    nextCapturedValue = 22000U;
    HAL_TIM_IC_CaptureCallback(&testTimerA);

    assert(sensorOnA.state == HC_SR04_STATE_DONE);
    assert(sensorOnA.pulseWidthUs == 21000U);
    assert(hc_sr04_is_ready(&sensorOnA) == 1U);

    assert(hc_sr04_read(&sensorOnA, &distanceCm) == HC_SR04_OK);
    assert(distanceCm == 360U);
    assert(sensorOnA.state == HC_SR04_STATE_IDLE);
    assert(hc_sr04_is_ready(&sensorOnA) == 0U);
}

static void test_out_of_range_pulse_still_resets_to_idle(void) {
    uint16_t distanceCm;

    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnB) == HC_SR04_OK);

    testTimerB.Channel = HAL_TIM_ACTIVE_CHANNEL_1;
    nextCapturedValue = 100U;
    HAL_TIM_IC_CaptureCallback(&testTimerB); /* rising */
    nextCapturedValue = 130U;                /* pulse = 30us < HC_SR04_MIN_PULSE_US */
    HAL_TIM_IC_CaptureCallback(&testTimerB); /* falling */

    assert(hc_sr04_read(&sensorOnB, &distanceCm) == HC_SR04_OUT_OF_RANGE);
    assert(sensorOnB.state == HC_SR04_STATE_IDLE);
}

static void test_read_before_ready_reports_timeout(void) {
    uint16_t distanceCm;

    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_OK);

    assert(hc_sr04_read(&sensorOnA, &distanceCm) == HC_SR04_TIMEOUT);
    assert(sensorOnA.state == HC_SR04_STATE_ARMED_RISING); /* read()는 미완료 상태를 건드리지 않는다 */

    hc_sr04_abort(&sensorOnA);
    assert(sensorOnA.state == HC_SR04_STATE_IDLE);
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_OK); /* abort 후 재사용 가능 */
    hc_sr04_abort(&sensorOnA);
}

static void test_capture_dispatch_ignores_unmatched_timer_and_channel(void) {
    reset_call_observations();
    assert(hc_sr04_trigger(&sensorOnA) == HC_SR04_OK);

    /* 다른 타이머의 캡처는 무시되어야 한다 (noise/타 센서 이벤트). */
    nextCapturedValue = 555U;
    testTimerB.Channel = HAL_TIM_ACTIVE_CHANNEL_2; /* sensorOnA와 채널 값은 같지만 타이머가 다름 */
    HAL_TIM_IC_CaptureCallback(&testTimerB);
    assert(sensorOnA.state == HC_SR04_STATE_ARMED_RISING);

    /* 같은 타이머라도 채널이 다르면(sensorOnB 몫이 아니라 미등록 채널) 무시되어야 한다. */
    testTimerA.Channel = HAL_TIM_ACTIVE_CHANNEL_1;
    HAL_TIM_IC_CaptureCallback(&testTimerA);
    assert(sensorOnA.state == HC_SR04_STATE_ARMED_RISING);

    /* 올바른 타이머/채널만 처리된다. */
    testTimerA.Channel = HAL_TIM_ACTIVE_CHANNEL_2;
    HAL_TIM_IC_CaptureCallback(&testTimerA);
    assert(sensorOnA.state == HC_SR04_STATE_ARMED_FALLING);

    hc_sr04_abort(&sensorOnA);
}

static void test_pulse_to_cm_conversion(void) {
    assert(hc_sr04_pulse_to_cm(0U) == 0U);
    assert(hc_sr04_pulse_to_cm(20000U) == 343U);
    assert(hc_sr04_pulse_to_cm(583U) == 9U); /* 10cm 문턱값 부근 */
}

int main(void) {
    test_init_registers_sensor_and_starts_capture();
    test_trigger_pulses_trig_and_arms_rising();
    test_trigger_rejects_while_armed();
    test_capture_sequence_computes_distance();
    test_out_of_range_pulse_still_resets_to_idle();
    test_read_before_ready_reports_timeout();
    test_capture_dispatch_ignores_unmatched_timer_and_channel();
    test_pulse_to_cm_conversion();
    return 0;
}
