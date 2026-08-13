#include "sensor_filter.h"

#include <stddef.h>

static uint16_t median_of_3(uint16_t a, uint16_t b, uint16_t c) {
    uint16_t temp;

    if (a > b) {
        temp = a;
        a = b;
        b = temp;
    }

    if (b > c) {
        temp = b;
        b = c;
        c = temp;
    }

    if (a > b) {
        temp = a;
        a = b;
        b = temp;
    }

    return b;
}

void sensor_filter_init(sensor_filter_t* filter) {
    if (filter == NULL) {
        return;
    }

    filter->samples[0] = 0U;
    filter->samples[1] = 0U;
    filter->samples[2] = 0U;
    filter->sampleCount = 0U;
    filter->sampleIndex = 0U;
    filter->faulted = 0U;
    filter->consecutiveFaults = 0U;
    filter->consecutiveValid = 0U;
    filter->lastDistanceCm = UART_SENSOR_DISTANCE_UNKNOWN;
}

void sensor_filter_record_sample(sensor_filter_t* filter, uint16_t distanceCm) {
    if (filter == NULL) {
        return;
    }

    filter->consecutiveFaults = 0U;

    if (filter->sampleCount == 0U) {
        /* 첫 표본은 3칸을 모두 채워 median이 즉시 의미를 갖게 한다. */
        filter->samples[0] = distanceCm;
        filter->samples[1] = distanceCm;
        filter->samples[2] = distanceCm;
        filter->sampleCount = 3U;
    } else {
        filter->samples[filter->sampleIndex] = distanceCm;
        filter->sampleIndex = (uint8_t)((filter->sampleIndex + 1U) % 3U);
    }

    filter->lastDistanceCm = median_of_3(filter->samples[0], filter->samples[1], filter->samples[2]);

    /*
     * FAULT 복구에만 연속 카운트를 요구한다. 한 번의 유효 표본으로 바로 풀면
     * 배선이 뜬 채로 잡히는 잡음 echo 하나에 FAULT가 풀렸다가 다시 걸리기를
     * 반복한다. 거리값 자체는 복구 대기 중에도 median으로 계속 갱신된다.
     */
    if (filter->faulted != 0U) {
        if (filter->consecutiveValid < 0xFFU) {
            filter->consecutiveValid++;
        }

        if (filter->consecutiveValid >= SENSOR_FILTER_RECOVERY_COUNT) {
            filter->faulted = 0U;
            filter->consecutiveValid = 0U;
        }
    }
}

void sensor_filter_record_fault(sensor_filter_t* filter) {
    if (filter == NULL) {
        return;
    }

    filter->consecutiveValid = 0U;

    if (filter->consecutiveFaults < 0xFFU) {
        filter->consecutiveFaults++;
    }

    if (filter->consecutiveFaults >= SENSOR_FILTER_FAULT_THRESHOLD) {
        filter->faulted = 1U;
        filter->sampleCount = 0U;
        filter->sampleIndex = 0U;
        filter->lastDistanceCm = UART_SENSOR_DISTANCE_UNKNOWN;
    }
}

uint8_t sensor_filter_get_state(const sensor_filter_t* filter) {
    if ((filter == NULL) || (filter->faulted != 0U)) {
        return (uint8_t)UART_SENSOR_FAULT;
    }

    return (uint8_t)UART_SENSOR_OK;
}

uint16_t sensor_filter_get_distance_cm(const sensor_filter_t* filter) {
    if ((filter == NULL) || (filter->faulted != 0U)) {
        return UART_SENSOR_DISTANCE_UNKNOWN;
    }

    return filter->lastDistanceCm;
}
