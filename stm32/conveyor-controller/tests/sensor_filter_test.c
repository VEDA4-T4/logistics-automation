#ifdef NDEBUG
#undef NDEBUG
#endif

#include "sensor_filter.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void feed(sensor_filter_t* filter, uint16_t distanceCm, uint32_t times) {
    uint32_t i;

    for (i = 0U; i < times; i++) {
        sensor_filter_record_sample(filter, distanceCm);
    }
}

static void test_initial_state_is_clear_with_unknown_distance(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);

    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
    assert(sensor_filter_get_distance_cm(&filter) == UART_SENSOR_DISTANCE_UNKNOWN);
}

static void test_first_sample_primes_median_immediately(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    sensor_filter_record_sample(&filter, 200U);

    assert(sensor_filter_get_distance_cm(&filter) == 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
}

static void test_detection_requires_three_consecutive_close_samples(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);

    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);

    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);

    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);
    assert(sensor_filter_get_distance_cm(&filter) == 5U);
}

static void test_single_transient_sample_is_rejected_by_median(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U); /* 안정적으로 CLEAR, samples=[200,200,200] */

    /* 노이즈성 단발 근접 판독 1회: median이 여전히 200 근처라 상태가 안 바뀐다. */
    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);

    feed(&filter, 200U, 2U); /* 창을 다시 200으로 채워 원상 복귀 확인 */
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);
}

static void test_hysteresis_band_holds_detected_state(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    feed(&filter, 5U, 3U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);

    /* 11cm은 진입(10) 임계값은 넘지만 이탈(12) 임계값 안이라 DETECTED를 유지해야 한다. */
    feed(&filter, 11U, 3U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);

    /* 13cm은 이탈 임계값을 넘지만, median 창이 이전 11cm 표본으로 채워져 있어
     * 후보가 CLEAR로 굳어지는 데 몇 회가 더 걸린다(median 지연 + 3회 디바운스). */
    sensor_filter_record_sample(&filter, 13U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);
    sensor_filter_record_sample(&filter, 13U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);
    sensor_filter_record_sample(&filter, 13U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_DETECTED);
    sensor_filter_record_sample(&filter, 13U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
}

static void test_consecutive_faults_latch_fault_state(void) {
    sensor_filter_t filter;
    uint32_t i;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U);

    for (i = 0U; i < (SENSOR_FILTER_FAULT_THRESHOLD - 1U); i++) {
        sensor_filter_record_fault(&filter);
        assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
    }

    sensor_filter_record_fault(&filter);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);
    assert(sensor_filter_get_distance_cm(&filter) == UART_SENSOR_DISTANCE_UNKNOWN);
}

static void test_recovery_from_fault_requires_debounce_again(void) {
    sensor_filter_t filter;
    uint32_t i;

    sensor_filter_init(&filter);
    for (i = 0U; i < SENSOR_FILTER_FAULT_THRESHOLD; i++) {
        sensor_filter_record_fault(&filter);
    }
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);

    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT); /* 1회로는 아직 부족 */

    sensor_filter_record_sample(&filter, 200U);
    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);
}

static void test_single_fault_does_not_affect_clear_state(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U);

    sensor_filter_record_fault(&filter);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);

    /* 이후 정상 표본이 오면 fault 카운트가 리셋된다. */
    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_CLEAR);
}

int main(void) {
    test_initial_state_is_clear_with_unknown_distance();
    test_first_sample_primes_median_immediately();
    test_detection_requires_three_consecutive_close_samples();
    test_single_transient_sample_is_rejected_by_median();
    test_hysteresis_band_holds_detected_state();
    test_consecutive_faults_latch_fault_state();
    test_recovery_from_fault_requires_debounce_again();
    test_single_fault_does_not_affect_clear_state();
    return 0;
}
