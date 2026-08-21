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
    filter->confirmedState = UART_SENSOR_CLEAR;
    filter->pendingCandidate = UART_SENSOR_CLEAR;
    filter->consecutiveMatches = 0U;
    filter->consecutiveFaults = 0U;
    filter->lastDistanceCm = UART_SENSOR_DISTANCE_UNKNOWN;
}

void sensor_filter_record_sample(sensor_filter_t* filter, uint16_t distanceCm) {
    uint16_t medianCm;
    uint8_t candidate;

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

    medianCm = median_of_3(filter->samples[0], filter->samples[1], filter->samples[2]);
    filter->lastDistanceCm = medianCm;

    if (filter->confirmedState == UART_SENSOR_DETECTED) {
        candidate = (medianCm <= SENSOR_FILTER_EXIT_THRESHOLD_CM) ? UART_SENSOR_DETECTED : UART_SENSOR_CLEAR;
    } else {
        candidate = (medianCm <= SENSOR_FILTER_ENTER_THRESHOLD_CM) ? UART_SENSOR_DETECTED : UART_SENSOR_CLEAR;
    }

    if (candidate == filter->pendingCandidate) {
        if (filter->consecutiveMatches < 0xFFU) {
            filter->consecutiveMatches++;
        }
    } else {
        filter->pendingCandidate = candidate;
        filter->consecutiveMatches = 1U;
    }

    if ((filter->consecutiveMatches >= SENSOR_FILTER_DEBOUNCE_COUNT) && (filter->confirmedState != candidate)) {
        filter->confirmedState = candidate;
    }
}

void sensor_filter_record_fault(sensor_filter_t* filter) {
    if (filter == NULL) {
        return;
    }

    if (filter->consecutiveFaults < 0xFFU) {
        filter->consecutiveFaults++;
    }

    if (filter->consecutiveFaults >= SENSOR_FILTER_FAULT_THRESHOLD) {
        filter->confirmedState = UART_SENSOR_FAULT;
        filter->pendingCandidate = UART_SENSOR_CLEAR;
        filter->consecutiveMatches = 0U;
        filter->sampleCount = 0U;
        filter->lastDistanceCm = UART_SENSOR_DISTANCE_UNKNOWN;
    }
}

uint8_t sensor_filter_get_state(const sensor_filter_t* filter) {
    return (filter != NULL) ? filter->confirmedState : (uint8_t)UART_SENSOR_FAULT;
}

uint16_t sensor_filter_get_distance_cm(const sensor_filter_t* filter) {
    if ((filter == NULL) || (filter->confirmedState == UART_SENSOR_FAULT)) {
        return UART_SENSOR_DISTANCE_UNKNOWN;
    }

    return filter->lastDistanceCm;
}
