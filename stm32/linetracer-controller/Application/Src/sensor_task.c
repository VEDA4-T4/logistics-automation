#include "sensor_task.h"

#include <stddef.h>
#include <string.h>

#include "adc.h"
#include "app_queues.h"
#include "app_timing.h"
#include "cmsis_os2.h"
#include "main.h"
#include "sensor_config.h"
#include "tim.h"

typedef enum {
    SENSOR_ADC_IDLE = 0,
    SENSOR_ADC_BUSY,
    SENSOR_ADC_READY,
    SENSOR_ADC_ERROR
} sensor_adc_state_t;

typedef enum {
    ULTRASONIC_CAPTURE_IDLE = 0,
    ULTRASONIC_CAPTURE_WAIT_RISING,
    ULTRASONIC_CAPTURE_WAIT_FALLING,
    ULTRASONIC_CAPTURE_READY,
    ULTRASONIC_CAPTURE_ERROR
} ultrasonic_capture_state_t;

typedef struct {
    GPIO_TypeDef *trigger_port;
    uint16_t trigger_pin;
    uint32_t timer_channel;
    HAL_TIM_ActiveChannel active_channel;
    uint32_t valid_flag;
    uint32_t error_flag;
    uint8_t direction_flag;
} ultrasonic_sensor_descriptor_t;

typedef struct {
    uint8_t stable;
    uint8_t candidate;
    uint8_t count;
    uint8_t initialized;
} debounce_filter_t;

typedef struct {
    uint16_t samples[SENSOR_FSR_FILTER_SAMPLES];
    uint32_t sum;
    uint8_t count;
    uint8_t next_index;
} fsr_filter_t;

typedef struct {
    debounce_filter_t line_left_filter;
    debounce_filter_t line_right_filter;
    fsr_filter_t fsr_filter;
    app_sensor_snapshot_t snapshot;
    uint32_t last_fsr_sample_ms;
    uint32_t last_ultrasonic_start_ms;
    uint32_t ultrasonic_last_success_ms[4];
    uint32_t line_white_since_ms;
    uint32_t fsr_candidate_since_ms;
    uint32_t reported_error_flags;
    uint8_t ultrasonic_failure_count[4];
    uint8_t next_ultrasonic_index;
    uint8_t ultrasonic_started_mask;
    uint8_t line_white_active;
    uint8_t line_lost_active;
    uint8_t fsr_candidate_loaded;
    uint8_t load_lost_reported;
} sensor_task_context_t;

typedef struct {
    uint8_t sensor_index;
    uint32_t pulse_width_us;
    uint8_t valid;
} ultrasonic_result_t;

static const ultrasonic_sensor_descriptor_t s_ultrasonic_sensors[4] = {
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_0,
        .timer_channel = TIM_CHANNEL_1,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_1,
        .valid_flag = APP_SENSOR_VALID_ULTRASONIC_FRONT,
        .error_flag = APP_SENSOR_ERROR_ULTRASONIC_FRONT,
        .direction_flag = APP_SENSOR_DIRECTION_FRONT,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_1,
        .timer_channel = TIM_CHANNEL_2,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_2,
        .valid_flag = APP_SENSOR_VALID_ULTRASONIC_REAR,
        .error_flag = APP_SENSOR_ERROR_ULTRASONIC_REAR,
        .direction_flag = APP_SENSOR_DIRECTION_REAR,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_2,
        .timer_channel = TIM_CHANNEL_3,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_3,
        .valid_flag = APP_SENSOR_VALID_ULTRASONIC_LEFT,
        .error_flag = APP_SENSOR_ERROR_ULTRASONIC_LEFT,
        .direction_flag = APP_SENSOR_DIRECTION_LEFT,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_10,
        .timer_channel = TIM_CHANNEL_4,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_4,
        .valid_flag = APP_SENSOR_VALID_ULTRASONIC_RIGHT,
        .error_flag = APP_SENSOR_ERROR_ULTRASONIC_RIGHT,
        .direction_flag = APP_SENSOR_DIRECTION_RIGHT,
    },
};

static volatile sensor_adc_state_t s_adc_state = SENSOR_ADC_IDLE;
static volatile uint16_t s_adc_value;
static volatile uint32_t s_adc_started_at_ms;

static volatile ultrasonic_capture_state_t s_ultrasonic_capture_state =
    ULTRASONIC_CAPTURE_IDLE;
static volatile uint8_t s_ultrasonic_active_index;
static volatile uint32_t s_ultrasonic_rising_capture;
static volatile uint32_t s_ultrasonic_pulse_width_us;
static volatile uint32_t s_ultrasonic_started_at_ms;
static uint8_t s_ultrasonic_timer_ready;

static app_sensor_snapshot_t s_latest_snapshot;
static volatile uint8_t s_latest_snapshot_valid;

static uint8_t TimeElapsed(
    uint32_t now_ms,
    uint32_t since_ms,
    uint32_t duration_ms)
{
    return ((uint32_t)(now_ms - since_ms) >= duration_ms) ? 1U : 0U;
}

static uint32_t EnterShortCriticalSection(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void ExitShortCriticalSection(uint32_t primask)
{
    if (primask == 0U) {
        __enable_irq();
    }
}

static void StoreLatestSnapshot(const app_sensor_snapshot_t *snapshot)
{
    uint32_t primask;

    if (snapshot == NULL) {
        return;
    }

    primask = EnterShortCriticalSection();
    s_latest_snapshot = *snapshot;
    s_latest_snapshot_valid = 1U;
    ExitShortCriticalSection(primask);
}

bool SensorTask_GetLatest(app_sensor_snapshot_t *snapshot)
{
    uint32_t primask;

    if (snapshot == NULL) {
        return false;
    }

    primask = EnterShortCriticalSection();
    if (s_latest_snapshot_valid == 0U) {
        ExitShortCriticalSection(primask);
        return false;
    }

    *snapshot = s_latest_snapshot;
    ExitShortCriticalSection(primask);
    return true;
}

static void PublishHealthEvent(
    app_health_event_type_t type,
    uint32_t now_ms,
    uint32_t detail)
{
    app_health_event_t event;

    if (healthEventQueue == NULL) {
        return;
    }

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_SENSOR;
    (void)osMessageQueuePut(healthEventQueue, &event, 0U, 0U);
}

static void PublishSafetyEvent(
    app_safety_event_type_t type,
    linetracer_stop_reason_t reason,
    uint32_t now_ms,
    uint8_t active,
    uint8_t error_code)
{
    app_safety_event_t event;

    if (safetyEventQueue == NULL) {
        return;
    }

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.reason = reason;
    event.source_task = APP_TASK_SENSOR;
    event.error_code = error_code;
    event.active = active;

    if (osMessageQueuePut(safetyEventQueue, &event, 0U, 0U) != osOK) {
        PublishHealthEvent(
            APP_HEALTH_EVENT_QUEUE_FULL,
            now_ms,
            (uint32_t)APP_TASK_SAFETY);
    }
}

static void PublishSnapshot(
    const app_sensor_snapshot_t *snapshot,
    uint32_t now_ms)
{
    app_sensor_snapshot_t discarded;
    app_sensor_snapshot_t pending;

    if (snapshot == NULL) {
        return;
    }

    StoreLatestSnapshot(snapshot);

    if (sensorSnapshotQueue == NULL) {
        return;
    }

    if (osMessageQueuePut(sensorSnapshotQueue, snapshot, 0U, 0U) == osOK) {
        return;
    }

    /*
     * A sensor snapshot has latest-value semantics. Remove one stale item and
     * retry without ever waiting in SensorTask. Carry pending event bits into
     * the newest state so a slow consumer does not lose a one-shot event.
     */
    pending = *snapshot;
    if (osMessageQueueGet(
            sensorSnapshotQueue,
            &discarded,
            NULL,
            0U) == osOK) {
        pending.event_flags |= discarded.event_flags;
    }

    if (osMessageQueuePut(
            sensorSnapshotQueue,
            &pending,
            0U,
            0U) != osOK) {
        PublishHealthEvent(
            APP_HEALTH_EVENT_QUEUE_FULL,
            now_ms,
            (uint32_t)APP_TASK_CONTROL);
    }
}

static uint8_t NormalizeLineInput(GPIO_PinState pin_state)
{
#if SENSOR_LINE_ACTIVE_LOW
    return (pin_state == GPIO_PIN_RESET) ? 1U : 0U;
#else
    return (pin_state == GPIO_PIN_SET) ? 1U : 0U;
#endif
}

static uint8_t DebounceUpdate(
    debounce_filter_t *filter,
    uint8_t input,
    uint8_t required_samples)
{
    if ((filter == NULL) || (required_samples == 0U)) {
        return 0U;
    }

    if ((filter->count == 0U) || (filter->candidate != input)) {
        filter->candidate = input;
        filter->count = 1U;
    } else if (filter->count < required_samples) {
        filter->count++;
    }

    if (filter->count < required_samples) {
        return 0U;
    }

    if ((filter->initialized == 0U) ||
        (filter->stable != filter->candidate)) {
        filter->stable = filter->candidate;
        filter->initialized = 1U;
        return 1U;
    }

    return 0U;
}

static linetracer_line_state_t CalculateLineState(
    uint8_t left,
    uint8_t right)
{
    if ((left != 0U) && (right != 0U)) {
        return LINETRACER_LINE_CENTERED;
    }

    if ((left != 0U) && (right == 0U)) {
        return LINETRACER_LINE_LEFT_ONLY;
    }

    if ((left == 0U) && (right != 0U)) {
        return LINETRACER_LINE_RIGHT_ONLY;
    }

    return LINETRACER_LINE_WHITE_GAP;
}

static void UpdateLineSensors(
    sensor_task_context_t *context,
    uint32_t now_ms,
    uint32_t *event_flags)
{
    uint8_t raw_left;
    uint8_t raw_right;
    uint8_t left_changed;
    uint8_t right_changed;
    linetracer_line_state_t previous_state;
    linetracer_line_state_t next_state;
    uint32_t white_duration_ms;

    raw_left = NormalizeLineInput(
        HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4));
    raw_right = NormalizeLineInput(
        HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5));

    left_changed = DebounceUpdate(
        &context->line_left_filter,
        raw_left,
        APP_TIMING_LINE_DEBOUNCE_SAMPLES);
    right_changed = DebounceUpdate(
        &context->line_right_filter,
        raw_right,
        APP_TIMING_LINE_DEBOUNCE_SAMPLES);

    if ((context->line_left_filter.initialized == 0U) ||
        (context->line_right_filter.initialized == 0U)) {
        return;
    }

    context->snapshot.valid_flags |= APP_SENSOR_VALID_LINE;
    context->snapshot.line_left = context->line_left_filter.stable;
    context->snapshot.line_right = context->line_right_filter.stable;

    if ((left_changed != 0U) || (right_changed != 0U)) {
        previous_state = context->snapshot.line_state;
        next_state = CalculateLineState(
            context->snapshot.line_left,
            context->snapshot.line_right);

        if (next_state != previous_state) {
            context->snapshot.line_state = next_state;
            context->snapshot.line_changed_at_ms = now_ms;
            *event_flags |= APP_SENSOR_EVENT_LINE_CHANGED;

            if (next_state == LINETRACER_LINE_WHITE_GAP) {
                context->line_white_active = 1U;
                context->line_white_since_ms = now_ms;
            } else if (context->line_white_active != 0U) {
                white_duration_ms =
                    (uint32_t)(now_ms - context->line_white_since_ms);

                if ((context->line_lost_active == 0U) &&
                    (white_duration_ms >= APP_TIMING_MARKER_MIN_MS) &&
                    (white_duration_ms <= APP_TIMING_MARKER_MAX_MS)) {
                    *event_flags |= APP_SENSOR_EVENT_MARKER;
                }

                if (context->line_lost_active != 0U) {
                    context->line_lost_active = 0U;
                    PublishSafetyEvent(
                        APP_SAFETY_EVENT_LINE_LOST,
                        LINETRACER_STOP_REASON_LINE_LOST,
                        now_ms,
                        0U,
                        0U);
                }

                context->line_white_active = 0U;
            }
        }
    }

    if ((context->line_white_active != 0U) &&
        (context->line_lost_active == 0U) &&
        (TimeElapsed(
             now_ms,
             context->line_white_since_ms,
             APP_TIMING_LINE_LOST_MS) != 0U)) {
        context->line_lost_active = 1U;
        *event_flags |= APP_SENSOR_EVENT_LINE_LOST;
        PublishSafetyEvent(
            APP_SAFETY_EVENT_LINE_LOST,
            LINETRACER_STOP_REASON_LINE_LOST,
            now_ms,
            1U,
            0U);
    }
}

static uint8_t StartFsrConversion(uint32_t now_ms)
{
    if (s_adc_state != SENSOR_ADC_IDLE) {
        return 0U;
    }

    s_adc_started_at_ms = now_ms;
    s_adc_state = SENSOR_ADC_BUSY;
    if (HAL_ADC_Start_IT(&hadc1) != HAL_OK) {
        s_adc_state = SENSOR_ADC_ERROR;
        return 0U;
    }

    return 1U;
}

static sensor_adc_state_t PollFsrConversion(
    uint32_t now_ms,
    uint16_t *value)
{
    sensor_adc_state_t state = s_adc_state;

    if ((state == SENSOR_ADC_BUSY) &&
        (TimeElapsed(
             now_ms,
             s_adc_started_at_ms,
             APP_TIMING_FSR_ADC_TIMEOUT_MS) != 0U)) {
        (void)HAL_ADC_Stop_IT(&hadc1);
        s_adc_state = SENSOR_ADC_ERROR;
        state = SENSOR_ADC_ERROR;
    }

    if (state == SENSOR_ADC_READY) {
        if (value != NULL) {
            *value = s_adc_value;
        }
        s_adc_state = SENSOR_ADC_IDLE;
    } else if (state == SENSOR_ADC_ERROR) {
        s_adc_state = SENSOR_ADC_IDLE;
    }

    return state;
}

static uint16_t FilterFsrSample(
    fsr_filter_t *filter,
    uint16_t sample)
{
    if (filter->count < SENSOR_FSR_FILTER_SAMPLES) {
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
        filter->count++;
    } else {
        filter->sum -= filter->samples[filter->next_index];
        filter->samples[filter->next_index] = sample;
        filter->sum += sample;
    }

    filter->next_index =
        (uint8_t)((filter->next_index + 1U) %
                  SENSOR_FSR_FILTER_SAMPLES);

    return (uint16_t)(filter->sum / filter->count);
}

static void UpdateFsrState(
    sensor_task_context_t *context,
    uint16_t raw_value,
    uint32_t now_ms,
    uint32_t *event_flags)
{
    uint8_t currently_loaded;
    uint8_t desired_loaded;

    context->snapshot.fsr_raw =
        FilterFsrSample(&context->fsr_filter, raw_value);
    context->snapshot.valid_flags |= APP_SENSOR_VALID_FSR;
    context->snapshot.error_flags &=
        ~(APP_SENSOR_ERROR_FSR_ADC | APP_SENSOR_ERROR_FSR_TIMEOUT);
    context->last_fsr_sample_ms = now_ms;

    currently_loaded =
        (context->snapshot.load_state ==
         UART_LINETRACER_LOAD_PRESENT)
            ? 1U
            : 0U;
    desired_loaded = currently_loaded;

    if ((currently_loaded == 0U) &&
        (context->snapshot.fsr_raw >=
         SENSOR_FSR_LOAD_ON_THRESHOLD)) {
        desired_loaded = 1U;
    } else if ((currently_loaded != 0U) &&
               (context->snapshot.fsr_raw <=
                SENSOR_FSR_LOAD_OFF_THRESHOLD)) {
        desired_loaded = 0U;
    }

    if (desired_loaded == currently_loaded) {
        context->fsr_candidate_loaded = currently_loaded;
        context->fsr_candidate_since_ms = now_ms;
    } else if (desired_loaded != context->fsr_candidate_loaded) {
        context->fsr_candidate_loaded = desired_loaded;
        context->fsr_candidate_since_ms = now_ms;
    } else if (TimeElapsed(
                   now_ms,
                   context->fsr_candidate_since_ms,
                   APP_TIMING_FSR_STABLE_MS) != 0U) {
        context->snapshot.load_state =
            (desired_loaded != 0U)
                ? UART_LINETRACER_LOAD_PRESENT
                : UART_LINETRACER_LOAD_EMPTY;
        context->snapshot.load_changed_at_ms = now_ms;
        *event_flags |=
            (desired_loaded != 0U)
                ? APP_SENSOR_EVENT_LOAD_ON
                : APP_SENSOR_EVENT_LOAD_OFF;
        context->fsr_candidate_since_ms = now_ms;

        if (desired_loaded == 0U) {
            context->load_lost_reported = 1U;
            PublishSafetyEvent(
                APP_SAFETY_EVENT_LOAD_LOST,
                LINETRACER_STOP_REASON_LOAD_LOST,
                now_ms,
                1U,
                0U);
        } else if (context->load_lost_reported != 0U) {
            context->load_lost_reported = 0U;
            PublishSafetyEvent(
                APP_SAFETY_EVENT_LOAD_LOST,
                LINETRACER_STOP_REASON_LOAD_LOST,
                now_ms,
                0U,
                0U);
        }
    }

    if ((context->snapshot.overload_active == 0U) &&
        (context->snapshot.fsr_raw >=
         SENSOR_FSR_OVERLOAD_ON_THRESHOLD)) {
        context->snapshot.overload_active = 1U;
        *event_flags |= APP_SENSOR_EVENT_OVERLOAD;
        PublishSafetyEvent(
            APP_SAFETY_EVENT_OVERLOAD,
            LINETRACER_STOP_REASON_OVERLOAD,
            now_ms,
            1U,
            0U);
    } else if ((context->snapshot.overload_active != 0U) &&
               (context->snapshot.fsr_raw <=
                SENSOR_FSR_OVERLOAD_OFF_THRESHOLD)) {
        context->snapshot.overload_active = 0U;
        *event_flags |= APP_SENSOR_EVENT_OVERLOAD;
        PublishSafetyEvent(
            APP_SAFETY_EVENT_OVERLOAD,
            LINETRACER_STOP_REASON_OVERLOAD,
            now_ms,
            0U,
            0U);
    }
}

static uint32_t GetTim1ClockHz(void)
{
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK2Freq();

    if ((RCC->CFGR & RCC_CFGR_PPRE2) != RCC_HCLK_DIV1) {
        timer_clock_hz *= 2U;
    }

    return timer_clock_hz;
}

static uint8_t ConfigureUltrasonicTimer(void)
{
    uint32_t timer_clock_hz = GetTim1ClockHz();
    uint32_t prescaler;

    if ((timer_clock_hz < SENSOR_ULTRASONIC_TIMER_HZ) ||
        ((timer_clock_hz % SENSOR_ULTRASONIC_TIMER_HZ) != 0U)) {
        return 0U;
    }

    prescaler =
        (timer_clock_hz / SENSOR_ULTRASONIC_TIMER_HZ) - 1U;

    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_3);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_4);

    __HAL_TIM_DISABLE(&htim1);
    htim1.Init.Prescaler = prescaler;
    htim1.Init.Period = UINT16_MAX;
    __HAL_TIM_SET_PRESCALER(&htim1, prescaler);
    __HAL_TIM_SET_AUTORELOAD(&htim1, UINT16_MAX);
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    htim1.Instance->EGR = TIM_EGR_UG;

    HAL_NVIC_SetPriority(TIM1_CC_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
    return 1U;
}

static void InitializeCycleCounter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void DelayMicroseconds(uint32_t delay_us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    uint32_t wait_cycles = cycles_per_us * delay_us;

    while ((uint32_t)(DWT->CYCCNT - start) < wait_cycles) {
        __NOP();
    }
}

static uint8_t StartUltrasonicMeasurement(
    uint8_t sensor_index,
    uint32_t now_ms)
{
    const ultrasonic_sensor_descriptor_t *sensor;

    if ((sensor_index >= 4U) ||
        (s_ultrasonic_capture_state !=
         ULTRASONIC_CAPTURE_IDLE)) {
        return 0U;
    }

    sensor = &s_ultrasonic_sensors[sensor_index];
    s_ultrasonic_active_index = sensor_index;
    s_ultrasonic_started_at_ms = now_ms;
    s_ultrasonic_rising_capture = 0U;
    s_ultrasonic_pulse_width_us = 0U;
    s_ultrasonic_capture_state =
        ULTRASONIC_CAPTURE_WAIT_RISING;

    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    __HAL_TIM_SET_CAPTUREPOLARITY(
        &htim1,
        sensor->timer_channel,
        TIM_INPUTCHANNELPOLARITY_RISING);

    if (HAL_TIM_IC_Start_IT(
            &htim1,
            sensor->timer_channel) != HAL_OK) {
        s_ultrasonic_capture_state =
            ULTRASONIC_CAPTURE_ERROR;
        return 0U;
    }

    HAL_GPIO_WritePin(
        sensor->trigger_port,
        sensor->trigger_pin,
        GPIO_PIN_SET);
    DelayMicroseconds(SENSOR_ULTRASONIC_TRIGGER_PULSE_US);
    HAL_GPIO_WritePin(
        sensor->trigger_port,
        sensor->trigger_pin,
        GPIO_PIN_RESET);

    return 1U;
}

static void CheckUltrasonicCaptureTimeout(uint32_t now_ms)
{
    const ultrasonic_sensor_descriptor_t *sensor;
    ultrasonic_capture_state_t state =
        s_ultrasonic_capture_state;

    if ((state != ULTRASONIC_CAPTURE_WAIT_RISING) &&
        (state != ULTRASONIC_CAPTURE_WAIT_FALLING)) {
        return;
    }

    if (TimeElapsed(
            now_ms,
            s_ultrasonic_started_at_ms,
            APP_TIMING_ULTRASONIC_ECHO_TIMEOUT_MS) == 0U) {
        return;
    }

    sensor =
        &s_ultrasonic_sensors[s_ultrasonic_active_index];
    (void)HAL_TIM_IC_Stop_IT(
        &htim1,
        sensor->timer_channel);
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_ERROR;
}

static uint8_t TakeUltrasonicResult(
    ultrasonic_result_t *result)
{
    ultrasonic_capture_state_t state =
        s_ultrasonic_capture_state;

    if ((result == NULL) ||
        ((state != ULTRASONIC_CAPTURE_READY) &&
         (state != ULTRASONIC_CAPTURE_ERROR))) {
        return 0U;
    }

    result->sensor_index = s_ultrasonic_active_index;
    result->pulse_width_us = s_ultrasonic_pulse_width_us;
    result->valid =
        (state == ULTRASONIC_CAPTURE_READY) ? 1U : 0U;
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_IDLE;
    return 1U;
}

static uint16_t PulseWidthToMillimeters(uint32_t pulse_width_us)
{
    return (uint16_t)(
        ((pulse_width_us * 10U) + 29U) / 58U);
}

static uint16_t *UltrasonicDistanceField(
    app_sensor_snapshot_t *snapshot,
    uint8_t sensor_index)
{
    switch (sensor_index) {
        case 0U:
            return &snapshot->ultrasonic_front_mm;
        case 1U:
            return &snapshot->ultrasonic_rear_mm;
        case 2U:
            return &snapshot->ultrasonic_left_mm;
        case 3U:
            return &snapshot->ultrasonic_right_mm;
        default:
            return NULL;
    }
}

static void UpdateObstacleState(
    sensor_task_context_t *context,
    const ultrasonic_sensor_descriptor_t *sensor,
    uint16_t distance_mm,
    uint32_t now_ms,
    uint32_t *event_flags)
{
    uint8_t previous_mask = context->snapshot.obstacle_mask;

    if (((previous_mask & sensor->direction_flag) == 0U) &&
        (distance_mm <= SENSOR_OBSTACLE_ON_MM)) {
        context->snapshot.obstacle_mask |=
            sensor->direction_flag;
    } else if (((previous_mask & sensor->direction_flag) != 0U) &&
               (distance_mm >= SENSOR_OBSTACLE_OFF_MM)) {
        context->snapshot.obstacle_mask &=
            (uint8_t)~sensor->direction_flag;
    }

    if (context->snapshot.obstacle_mask == previous_mask) {
        return;
    }

    context->snapshot.obstacle_changed_at_ms = now_ms;
    *event_flags |= APP_SENSOR_EVENT_OBSTACLE;
    PublishSafetyEvent(
        APP_SAFETY_EVENT_OBSTACLE,
        LINETRACER_STOP_REASON_OBSTACLE,
        now_ms,
        (context->snapshot.obstacle_mask != 0U) ? 1U : 0U,
        context->snapshot.obstacle_mask);
}

static void RegisterUltrasonicFailure(
    sensor_task_context_t *context,
    uint8_t sensor_index)
{
    const ultrasonic_sensor_descriptor_t *sensor;

    if (sensor_index >= 4U) {
        return;
    }

    sensor = &s_ultrasonic_sensors[sensor_index];
    context->snapshot.valid_flags &= ~sensor->valid_flag;

    if (context->ultrasonic_failure_count[sensor_index] <
        UINT8_MAX) {
        context->ultrasonic_failure_count[sensor_index]++;
    }

    if (context->ultrasonic_failure_count[sensor_index] >=
        SENSOR_ULTRASONIC_MAX_CONSECUTIVE_FAILURES) {
        context->snapshot.error_flags |= sensor->error_flag;
    }
}

static void ProcessUltrasonicResult(
    sensor_task_context_t *context,
    const ultrasonic_result_t *result,
    uint32_t now_ms,
    uint32_t *event_flags)
{
    const ultrasonic_sensor_descriptor_t *sensor;
    uint16_t *distance_field;
    uint16_t distance_mm;

    if ((result == NULL) || (result->sensor_index >= 4U)) {
        return;
    }

    sensor = &s_ultrasonic_sensors[result->sensor_index];
    distance_field = UltrasonicDistanceField(
        &context->snapshot,
        result->sensor_index);

    if ((result->valid == 0U) || (distance_field == NULL)) {
        RegisterUltrasonicFailure(
            context,
            result->sensor_index);
        return;
    }

    distance_mm =
        PulseWidthToMillimeters(result->pulse_width_us);
    if ((distance_mm < SENSOR_ULTRASONIC_MIN_MM) ||
        (distance_mm > SENSOR_ULTRASONIC_MAX_MM)) {
        RegisterUltrasonicFailure(
            context,
            result->sensor_index);
        return;
    }

    *distance_field = distance_mm;
    context->ultrasonic_failure_count[result->sensor_index] = 0U;
    context->ultrasonic_last_success_ms[result->sensor_index] =
        now_ms;
    context->snapshot.valid_flags |= sensor->valid_flag;
    context->snapshot.error_flags &= ~sensor->error_flag;

    UpdateObstacleState(
        context,
        sensor,
        distance_mm,
        now_ms,
        event_flags);
}

static void CheckSensorStaleness(
    sensor_task_context_t *context,
    uint32_t now_ms)
{
    uint8_t index;

    if (TimeElapsed(
            now_ms,
            context->last_fsr_sample_ms,
            APP_TIMING_FSR_ADC_TIMEOUT_MS) != 0U) {
        context->snapshot.valid_flags &= ~APP_SENSOR_VALID_FSR;
        context->snapshot.error_flags |=
            APP_SENSOR_ERROR_FSR_TIMEOUT;
    }

    for (index = 0U; index < 4U; index++) {
        const ultrasonic_sensor_descriptor_t *sensor =
            &s_ultrasonic_sensors[index];

        if ((context->ultrasonic_started_mask &
             sensor->direction_flag) == 0U) {
            continue;
        }

        if (TimeElapsed(
                now_ms,
                context->ultrasonic_last_success_ms[index],
                APP_TIMING_ULTRASONIC_STALE_MS) != 0U) {
            context->snapshot.valid_flags &=
                ~sensor->valid_flag;
            context->snapshot.error_flags |=
                sensor->error_flag;
        }
    }
}

static void UpdateCommonSensorError(
    sensor_task_context_t *context,
    uint32_t now_ms,
    uint32_t *event_flags)
{
    uint32_t current_errors = context->snapshot.error_flags;

    if (current_errors == context->reported_error_flags) {
        return;
    }

    context->snapshot.error_changed_at_ms = now_ms;

    if (current_errors == APP_SENSOR_ERROR_NONE) {
        *event_flags |= APP_SENSOR_EVENT_SENSOR_RECOVERED;
        PublishSafetyEvent(
            APP_SAFETY_EVENT_SENSOR_FAULT,
            LINETRACER_STOP_REASON_SENSOR_FAULT,
            now_ms,
            0U,
            0U);
    } else {
        *event_flags |= APP_SENSOR_EVENT_SENSOR_FAULT;
        PublishSafetyEvent(
            APP_SAFETY_EVENT_SENSOR_FAULT,
            LINETRACER_STOP_REASON_SENSOR_FAULT,
            now_ms,
            1U,
            (uint8_t)current_errors);
        PublishHealthEvent(
            APP_HEALTH_EVENT_INTERNAL_ERROR,
            now_ms,
            current_errors);
    }

    context->reported_error_flags = current_errors;
}

static uint8_t InitializeSensorHardware(void)
{
    HAL_GPIO_WritePin(
        GPIOB,
        GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10,
        GPIO_PIN_RESET);

    HAL_NVIC_SetPriority(ADC_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(ADC_IRQn);

    InitializeCycleCounter();
    s_ultrasonic_timer_ready = ConfigureUltrasonicTimer();
    return s_ultrasonic_timer_ready;
}

static void InitializeContext(
    sensor_task_context_t *context,
    uint32_t now_ms)
{
    uint8_t index;

    (void)memset(context, 0, sizeof(*context));
    context->snapshot.sampled_at_ms = now_ms;
    context->snapshot.line_state = LINETRACER_LINE_UNKNOWN;
    context->snapshot.load_state =
        UART_LINETRACER_LOAD_EMPTY;
    context->snapshot.ultrasonic_front_mm = UINT16_MAX;
    context->snapshot.ultrasonic_rear_mm = UINT16_MAX;
    context->snapshot.ultrasonic_left_mm = UINT16_MAX;
    context->snapshot.ultrasonic_right_mm = UINT16_MAX;
    context->last_fsr_sample_ms = now_ms;
    context->last_ultrasonic_start_ms =
        now_ms - APP_TIMING_ULTRASONIC_PERIOD_MS;
    context->fsr_candidate_since_ms = now_ms;

    for (index = 0U; index < 4U; index++) {
        context->ultrasonic_last_success_ms[index] = now_ms;
    }

    StoreLatestSnapshot(&context->snapshot);
}

void StartSensorTask(void *argument)
{
    sensor_task_context_t context;
    ultrasonic_result_t ultrasonic_result;
    sensor_adc_state_t adc_state;
    uint32_t next_wake;
    uint32_t now_ms;
    uint32_t event_flags;
    uint16_t fsr_value = 0U;
    uint8_t ultrasonic_timer_ready;

    (void)argument;

    now_ms = osKernelGetTickCount();
    InitializeContext(&context, now_ms);
    ultrasonic_timer_ready = InitializeSensorHardware();
    if (ultrasonic_timer_ready == 0U) {
        context.snapshot.error_flags |=
            APP_SENSOR_ERROR_ULTRASONIC_FRONT |
            APP_SENSOR_ERROR_ULTRASONIC_REAR |
            APP_SENSOR_ERROR_ULTRASONIC_LEFT |
            APP_SENSOR_ERROR_ULTRASONIC_RIGHT;
    }
    (void)StartFsrConversion(now_ms);

    next_wake = osKernelGetTickCount();

    for (;;) {
        now_ms = osKernelGetTickCount();
        event_flags = APP_SENSOR_EVENT_NONE;

        UpdateLineSensors(&context, now_ms, &event_flags);

        adc_state = PollFsrConversion(now_ms, &fsr_value);
        if (adc_state == SENSOR_ADC_READY) {
            UpdateFsrState(
                &context,
                fsr_value,
                now_ms,
                &event_flags);
            (void)StartFsrConversion(now_ms);
        } else if (adc_state == SENSOR_ADC_ERROR) {
            context.snapshot.valid_flags &=
                ~APP_SENSOR_VALID_FSR;
            context.snapshot.error_flags |=
                APP_SENSOR_ERROR_FSR_ADC;
            (void)StartFsrConversion(now_ms);
        } else if (adc_state == SENSOR_ADC_IDLE) {
            (void)StartFsrConversion(now_ms);
        }

        CheckUltrasonicCaptureTimeout(now_ms);
        if (TakeUltrasonicResult(&ultrasonic_result) != 0U) {
            ProcessUltrasonicResult(
                &context,
                &ultrasonic_result,
                now_ms,
                &event_flags);
            context.next_ultrasonic_index =
                (uint8_t)((ultrasonic_result.sensor_index + 1U) %
                          4U);
        }

        if ((ultrasonic_timer_ready != 0U) &&
            (s_ultrasonic_capture_state ==
             ULTRASONIC_CAPTURE_IDLE) &&
            (TimeElapsed(
                 now_ms,
                 context.last_ultrasonic_start_ms,
                 APP_TIMING_ULTRASONIC_PERIOD_MS) != 0U)) {
            uint8_t sensor_index =
                context.next_ultrasonic_index;
            const ultrasonic_sensor_descriptor_t *sensor =
                &s_ultrasonic_sensors[sensor_index];

            context.last_ultrasonic_start_ms = now_ms;
            context.ultrasonic_started_mask |=
                sensor->direction_flag;
            (void)StartUltrasonicMeasurement(
                sensor_index,
                now_ms);
        }

        CheckSensorStaleness(&context, now_ms);
        UpdateCommonSensorError(
            &context,
            now_ms,
            &event_flags);

        context.snapshot.sampled_at_ms = now_ms;
        context.snapshot.event_flags = event_flags;
        PublishSnapshot(&context.snapshot, now_ms);

        next_wake += APP_TIMING_SENSOR_PERIOD_MS;
        if (osDelayUntil(next_wake) != osOK) {
            next_wake = osKernelGetTickCount();
        }
    }
}

void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}

void TIM1_CC_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc != &hadc1) {
        return;
    }

    s_adc_value = (uint16_t)HAL_ADC_GetValue(hadc);
    (void)HAL_ADC_Stop_IT(hadc);
    s_adc_state = SENSOR_ADC_READY;
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc != &hadc1) {
        return;
    }

    (void)HAL_ADC_Stop_IT(hadc);
    s_adc_state = SENSOR_ADC_ERROR;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    const ultrasonic_sensor_descriptor_t *sensor;
    uint32_t falling_capture;
    uint32_t timer_period;

    if ((htim != &htim1) ||
        (s_ultrasonic_active_index >= 4U)) {
        return;
    }

    sensor =
        &s_ultrasonic_sensors[s_ultrasonic_active_index];
    if (htim->Channel != sensor->active_channel) {
        return;
    }

    if (s_ultrasonic_capture_state ==
        ULTRASONIC_CAPTURE_WAIT_RISING) {
        s_ultrasonic_rising_capture =
            HAL_TIM_ReadCapturedValue(
                htim,
                sensor->timer_channel);
        __HAL_TIM_SET_CAPTUREPOLARITY(
            htim,
            sensor->timer_channel,
            TIM_INPUTCHANNELPOLARITY_FALLING);
        s_ultrasonic_capture_state =
            ULTRASONIC_CAPTURE_WAIT_FALLING;
        return;
    }

    if (s_ultrasonic_capture_state !=
        ULTRASONIC_CAPTURE_WAIT_FALLING) {
        return;
    }

    falling_capture = HAL_TIM_ReadCapturedValue(
        htim,
        sensor->timer_channel);
    timer_period = __HAL_TIM_GET_AUTORELOAD(htim) + 1U;

    if (falling_capture >= s_ultrasonic_rising_capture) {
        s_ultrasonic_pulse_width_us =
            falling_capture - s_ultrasonic_rising_capture;
    } else {
        s_ultrasonic_pulse_width_us =
            (timer_period - s_ultrasonic_rising_capture) +
            falling_capture;
    }

    (void)HAL_TIM_IC_Stop_IT(
        htim,
        sensor->timer_channel);
    s_ultrasonic_capture_state =
        ULTRASONIC_CAPTURE_READY;
}
