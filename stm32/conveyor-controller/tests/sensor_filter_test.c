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

static void test_initial_state_is_ok_with_unknown_distance(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);

    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == UART_SENSOR_DISTANCE_UNKNOWN);
}

static void test_first_sample_primes_median_immediately(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    sensor_filter_record_sample(&filter, 200U);

    assert(sensor_filter_get_distance_cm(&filter) == 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
}

/*
 * 근접 거리든 원거리든 상태는 항상 OK다. 상자 유무 판정은 중앙 서버 몫이므로
 * 필터는 거리값만 그대로 통과시킨다(임계값/히스테리시스/디바운스 없음).
 */
static void test_close_distance_is_reported_verbatim_without_judgement(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);

    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 5U);

    feed(&filter, 5U, 2U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 5U);

    /* 예전 진입(10cm)/이탈(12cm) 임계값 근처에서도 상태 변화가 없어야 한다. */
    feed(&filter, 11U, 3U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 11U);

    feed(&filter, 13U, 3U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 13U);
}

/*
 * median은 남는다. 측정값 자체를 다듬는 잡음 제거이지 업무 판단이 아니라서,
 * 서버가 임계값을 적용할 때 단발 스파이크에 흔들리지 않게 해준다.
 */
static void test_single_transient_sample_is_rejected_by_median(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U); /* samples=[200,200,200] */

    sensor_filter_record_sample(&filter, 5U);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);

    feed(&filter, 200U, 2U);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);
}

static void test_consecutive_faults_latch_fault_state(void) {
    sensor_filter_t filter;
    uint32_t i;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U);

    for (i = 0U; i < (SENSOR_FILTER_FAULT_THRESHOLD - 1U); i++) {
        sensor_filter_record_fault(&filter);
        assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    }

    sensor_filter_record_fault(&filter);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);
    assert(sensor_filter_get_distance_cm(&filter) == UART_SENSOR_DISTANCE_UNKNOWN);
}

static void test_recovery_from_fault_requires_consecutive_valid_samples(void) {
    sensor_filter_t filter;
    uint32_t i;

    sensor_filter_init(&filter);
    for (i = 0U; i < SENSOR_FILTER_FAULT_THRESHOLD; i++) {
        sensor_filter_record_fault(&filter);
    }
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);

    for (i = 0U; i < (SENSOR_FILTER_RECOVERY_COUNT - 1U); i++) {
        sensor_filter_record_sample(&filter, 200U);
        assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);
        assert(sensor_filter_get_distance_cm(&filter) == UART_SENSOR_DISTANCE_UNKNOWN);
    }

    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);
}

/* 복구 도중 무효 측정이 한 번 끼면 복구 카운트가 처음부터 다시 시작한다. */
static void test_fault_during_recovery_restarts_the_recovery_count(void) {
    sensor_filter_t filter;
    uint32_t i;

    sensor_filter_init(&filter);
    for (i = 0U; i < SENSOR_FILTER_FAULT_THRESHOLD; i++) {
        sensor_filter_record_fault(&filter);
    }

    for (i = 0U; i < (SENSOR_FILTER_RECOVERY_COUNT - 1U); i++) {
        sensor_filter_record_sample(&filter, 200U);
    }
    sensor_filter_record_fault(&filter);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);

    for (i = 0U; i < (SENSOR_FILTER_RECOVERY_COUNT - 1U); i++) {
        sensor_filter_record_sample(&filter, 200U);
        assert(sensor_filter_get_state(&filter) == UART_SENSOR_FAULT);
    }

    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
}

static void test_single_fault_does_not_affect_ok_state(void) {
    sensor_filter_t filter;

    sensor_filter_init(&filter);
    feed(&filter, 200U, 3U);

    sensor_filter_record_fault(&filter);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
    assert(sensor_filter_get_distance_cm(&filter) == 200U);

    /* 이후 정상 표본이 오면 fault 카운트가 리셋된다. */
    sensor_filter_record_sample(&filter, 200U);
    assert(sensor_filter_get_state(&filter) == UART_SENSOR_OK);
}

int main(void) {
    test_initial_state_is_ok_with_unknown_distance();
    test_first_sample_primes_median_immediately();
    test_close_distance_is_reported_verbatim_without_judgement();
    test_single_transient_sample_is_rejected_by_median();
    test_consecutive_faults_latch_fault_state();
    test_recovery_from_fault_requires_consecutive_valid_samples();
    test_fault_during_recovery_restarts_the_recovery_count();
    test_single_fault_does_not_affect_ok_state();
    return 0;
}
