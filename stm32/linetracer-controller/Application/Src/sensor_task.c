#include "sensor_task.h"

#include <stddef.h>
#include <string.h>

#include "adc.h"
#include "app_queues.h"
#include "app_timing.h"
#include "cmsis_os2.h"
#include "control_task.h"
#include "main.h"
#include "sensor_config.h"
#include "sensor_logic.h"
#include "tim.h"

typedef enum { SENSOR_ADC_IDLE = 0, SENSOR_ADC_BUSY, SENSOR_ADC_READY, SENSOR_ADC_ERROR } sensor_adc_state_t;

typedef enum {
    SENSOR_ADC_FSR_INDEX = 0,
    SENSOR_ADC_LINE_LEFT_INDEX,
    SENSOR_ADC_LINE_RIGHT_INDEX,
    SENSOR_ADC_LINE_CENTER_INDEX,
    SENSOR_ADC_CHANNEL_COUNT
} sensor_adc_channel_index_t;

typedef struct {
    uint16_t fsr_raw;
    uint16_t line_left_raw;
    uint16_t line_right_raw;
    uint16_t line_center_raw;
} sensor_adc_sample_t;

typedef enum {
    ULTRASONIC_CAPTURE_IDLE = 0,
    ULTRASONIC_CAPTURE_WAIT_RISING,
    ULTRASONIC_CAPTURE_WAIT_FALLING,
    ULTRASONIC_CAPTURE_READY,
    ULTRASONIC_CAPTURE_ERROR
} ultrasonic_capture_state_t;

typedef struct {
    GPIO_TypeDef* trigger_port;
    uint16_t trigger_pin;
    uint32_t timer_channel;
    HAL_TIM_ActiveChannel active_channel;
} ultrasonic_sensor_descriptor_t;

typedef struct {
    uint8_t sensor_index;
    uint32_t pulse_width_us;
    uint8_t valid;
} ultrasonic_result_t;

typedef struct {
    sensor_logic_context_t logic;
    sensor_event_latch_t event_latch;
    uint32_t last_ultrasonic_start_ms;
    uint32_t reported_safety_error_flags;
    uint8_t reported_safety_obstacle_mask;
    uint8_t next_ultrasonic_slot;
} sensor_task_context_t;

enum {
    SENSOR_ULTRASONIC_FRONT_INDEX = 0U,
    SENSOR_ULTRASONIC_REAR_INDEX,
    SENSOR_ULTRASONIC_LEFT_INDEX,
    SENSOR_ULTRASONIC_RIGHT_INDEX
};

static const uint8_t s_ultrasonic_measurement_schedule[] = {
    SENSOR_ULTRASONIC_FRONT_INDEX,
    SENSOR_ULTRASONIC_REAR_INDEX,
    SENSOR_ULTRASONIC_LEFT_INDEX,
    SENSOR_ULTRASONIC_RIGHT_INDEX,
};

static const ultrasonic_sensor_descriptor_t s_ultrasonic_sensors[SENSOR_LOGIC_ULTRASONIC_COUNT] = {
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_0,
        .timer_channel = TIM_CHANNEL_1,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_1,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_1,
        .timer_channel = TIM_CHANNEL_2,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_2,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_2,
        .timer_channel = TIM_CHANNEL_3,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_3,
    },
    {
        .trigger_port = GPIOB,
        .trigger_pin = GPIO_PIN_10,
        .timer_channel = TIM_CHANNEL_4,
        .active_channel = HAL_TIM_ACTIVE_CHANNEL_4,
    },
};

static volatile sensor_adc_state_t s_adc_state = SENSOR_ADC_IDLE;
static volatile uint32_t s_adc_started_at_ms;
static uint16_t s_adc_dma_values[SENSOR_ADC_CHANNEL_COUNT];

static volatile ultrasonic_capture_state_t s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_IDLE;
static volatile uint8_t s_ultrasonic_active_index;
static volatile uint32_t s_ultrasonic_rising_capture;
static volatile uint32_t s_ultrasonic_pulse_width_us;
static volatile uint32_t s_ultrasonic_started_at_ms;
static volatile uint8_t s_ultrasonic_trigger_pulse_active;
static uint8_t s_ultrasonic_timer_ready;

/* File-local Debug expressions; these are not part of the ControlTask contract. */
static app_sensor_snapshot_t s_latest_snapshot;
static volatile uint8_t s_latest_snapshot_valid;
static sensor_logic_diagnostics_t s_latest_diagnostics;
static sensor_marker_event_t s_latest_marker_event;
static volatile uint8_t s_latest_marker_event_valid;
static volatile uint32_t s_marker_event_count;
static volatile uint8_t s_fsr_baseline_capture_requested;
static volatile sensor_task_fsr_baseline_mode_t s_fsr_baseline_capture_mode;

static uint8_t TimeElapsed(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms) {
    return ((uint32_t)(now_ms - since_ms) >= duration_ms) ? 1U : 0U;
}

static uint32_t EnterShortCriticalSection(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void ExitShortCriticalSection(uint32_t primask) {
    if (primask == 0U) {
        __enable_irq();
    }
}

static void StoreLatestState(const sensor_logic_context_t* logic, const app_sensor_snapshot_t* published_snapshot) {
    const sensor_logic_diagnostics_t* diagnostics;
    sensor_marker_event_t marker_event;
    uint32_t primask;

    if ((logic == NULL) || (published_snapshot == NULL)) {
        return;
    }

    diagnostics = SensorLogic_GetDiagnostics(logic);
    if (diagnostics == NULL) {
        return;
    }

    primask = EnterShortCriticalSection();
    s_latest_snapshot = *published_snapshot;
    s_latest_diagnostics = *diagnostics;
    s_latest_snapshot_valid = 1U;

    if (SensorLogic_GetLatestMarker(logic, &marker_event) != 0U) {
        if ((s_latest_marker_event_valid == 0U) ||
            (s_latest_marker_event.detected_at_ms != marker_event.detected_at_ms) ||
            (s_latest_marker_event.type != marker_event.type)) {
            s_latest_marker_event = marker_event;
            s_latest_marker_event_valid = 1U;
            ++s_marker_event_count;
        }
    }
    ExitShortCriticalSection(primask);
}

bool SensorTask_GetLatest(app_sensor_snapshot_t* snapshot) {
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

void SensorTask_RequestFsrBaselineCapture(sensor_task_fsr_baseline_mode_t mode) {
    s_fsr_baseline_capture_mode = mode;
    s_fsr_baseline_capture_requested = 1U;
}

static void PublishHealthEvent(app_health_event_type_t type, uint32_t now_ms, uint32_t detail) {
    app_health_event_t event = { 0 };

    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    event.source_task = APP_TASK_SENSOR;
    (void)AppQueues_TryPutHealth(&event);
}

static void PublishSafetyEvent(app_safety_event_type_t type, linetracer_stop_reason_t reason, uint32_t now_ms,
                               uint8_t active, uint8_t error_code) {
    app_safety_event_t event = { 0 };

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
        PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, now_ms, (uint32_t)APP_TASK_SAFETY);
    }
}

static uint16_t SensorTask_GetMinimumObstacleDistance(const app_sensor_snapshot_t* snapshot, uint8_t direction_mask) {
    const uint16_t distances[SENSOR_LOGIC_ULTRASONIC_COUNT] = {
        snapshot != NULL ? snapshot->ultrasonic_front_mm : UINT16_MAX,
        snapshot != NULL ? snapshot->ultrasonic_rear_mm : UINT16_MAX,
        snapshot != NULL ? snapshot->ultrasonic_left_mm : UINT16_MAX,
        snapshot != NULL ? snapshot->ultrasonic_right_mm : UINT16_MAX,
    };
    uint16_t minimum_distance_mm = UINT16_MAX;
    uint8_t index;

    for (index = 0U; index < SENSOR_LOGIC_ULTRASONIC_COUNT; ++index) {
        uint8_t direction = (uint8_t)(1U << index);

        if ((direction_mask & direction) != 0U && distances[index] < minimum_distance_mm) {
            minimum_distance_mm = distances[index];
        }
    }

    return (minimum_distance_mm != UINT16_MAX) ? minimum_distance_mm : 0U;
}

static void PublishObstacleSafetyEvent(const sensor_task_context_t* context, uint8_t direction_mask, uint32_t now_ms) {
    const app_sensor_snapshot_t* snapshot;
    app_safety_event_t event = { 0 };

    if (context == NULL || safetyEventQueue == NULL) {
        return;
    }

    snapshot = SensorLogic_GetSnapshot(&context->logic);
    event.type = APP_SAFETY_EVENT_OBSTACLE;
    event.occurred_at_ms = now_ms;
    event.reason = (direction_mask != 0U) ? LINETRACER_STOP_REASON_OBSTACLE : LINETRACER_STOP_REASON_NONE;
    event.minimum_distance_mm =
        (direction_mask != 0U) ? SensorTask_GetMinimumObstacleDistance(snapshot, direction_mask) : 0U;
    event.source_task = APP_TASK_SENSOR;
    event.error_code = (direction_mask != 0U) ? (uint8_t)UART_ERROR_SENSOR : (uint8_t)UART_ERROR_NONE;
    event.active = (direction_mask != 0U) ? 1U : 0U;
    event.obstacle_direction_mask = direction_mask;

    if (osMessageQueuePut(safetyEventQueue, &event, 0U, 0U) != osOK) {
        PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, now_ms, (uint32_t)APP_TASK_SAFETY);
    }
}

static uint32_t SensorTask_GetEffectiveSafetyErrors(uint32_t raw_error_flags) {
    return SensorLogic_GetEffectiveSafetyErrorFlags(raw_error_flags);
}

static uint8_t SensorTask_GetEffectiveObstacleMask(uint8_t raw_obstacle_mask) {
    return SensorLogic_GetEffectiveSafetyObstacleMask(raw_obstacle_mask);
}

static void PublishLogicSafetyChanges(sensor_task_context_t* context, const sensor_logic_update_t* update,
                                      uint32_t now_ms) {
    const sensor_logic_diagnostics_t* diagnostics;
    uint32_t activated;
    uint32_t cleared;
    uint8_t effective_obstacle_mask;

    if ((context == NULL) || (update == NULL)) {
        return;
    }

    diagnostics = SensorLogic_GetDiagnostics(&context->logic);
    if (diagnostics == NULL) {
        return;
    }

    activated = update->safety_activated_flags;
    cleared = update->safety_cleared_flags;

    if ((activated & SENSOR_LOGIC_SAFETY_LINE_LOST) != 0U) {
        PublishSafetyEvent(APP_SAFETY_EVENT_LINE_LOST, LINETRACER_STOP_REASON_LINE_LOST, now_ms, 1U, 0U);
    }
    if ((cleared & SENSOR_LOGIC_SAFETY_LINE_LOST) != 0U) {
        PublishSafetyEvent(APP_SAFETY_EVENT_LINE_LOST, LINETRACER_STOP_REASON_LINE_LOST, now_ms, 0U, 0U);
    }

    effective_obstacle_mask = SensorTask_GetEffectiveObstacleMask(diagnostics->obstacle_mask);
    if (context->reported_safety_obstacle_mask != effective_obstacle_mask) {
        PublishObstacleSafetyEvent(context, effective_obstacle_mask, now_ms);
    }
    context->reported_safety_obstacle_mask = effective_obstacle_mask;

    if ((activated & SENSOR_LOGIC_SAFETY_OVERLOAD) != 0U) {
        PublishSafetyEvent(APP_SAFETY_EVENT_OVERLOAD, LINETRACER_STOP_REASON_OVERLOAD, now_ms, 1U, 0U);
    }
    if ((cleared & SENSOR_LOGIC_SAFETY_OVERLOAD) != 0U) {
        PublishSafetyEvent(APP_SAFETY_EVENT_OVERLOAD, LINETRACER_STOP_REASON_OVERLOAD, now_ms, 0U, 0U);
    }
}

static void PublishSnapshot(sensor_task_context_t* context, uint32_t new_event_flags, uint32_t now_ms) {
    const app_sensor_snapshot_t* latest;
    app_sensor_snapshot_t discarded;
    app_sensor_snapshot_t pending;

    if (context == NULL) {
        return;
    }

    latest = SensorLogic_GetSnapshot(&context->logic);
    if (latest == NULL) {
        return;
    }

    pending = *latest;
    pending.event_flags = SensorEventLatch_Pend(&context->event_latch, new_event_flags);
    StoreLatestState(&context->logic, &pending);

    if (sensorSnapshotQueue == NULL) {
        return;
    }

    if (osMessageQueuePut(sensorSnapshotQueue, &pending, 0U, 0U) == osOK) {
        SensorEventLatch_Acknowledge(&context->event_latch, pending.event_flags);
        return;
    }

    /* Latest-state queue: discard one stale state, but retain all event bits. */
    if (osMessageQueueGet(sensorSnapshotQueue, &discarded, NULL, 0U) == osOK) {
        (void)SensorEventLatch_Pend(&context->event_latch, discarded.event_flags);
    }

    pending = *latest;
    pending.event_flags = SensorEventLatch_Pend(&context->event_latch, APP_SENSOR_EVENT_NONE);
    StoreLatestState(&context->logic, &pending);

    if (osMessageQueuePut(sensorSnapshotQueue, &pending, 0U, 0U) == osOK) {
        SensorEventLatch_Acknowledge(&context->event_latch, pending.event_flags);
    } else {
        PublishHealthEvent(APP_HEALTH_EVENT_QUEUE_FULL, now_ms, (uint32_t)APP_TASK_CONTROL);
    }
}

static uint8_t NormalizeLineInput(GPIO_PinState pin_state) {
#if SENSOR_LINE_ACTIVE_LOW
    return (pin_state == GPIO_PIN_RESET) ? 1U : 0U;
#else
    return (pin_state == GPIO_PIN_SET) ? 1U : 0U;
#endif
}

static uint8_t StartSensorAdcScan(uint32_t now_ms) {
    if (s_adc_state != SENSOR_ADC_IDLE) {
        return 0U;
    }

    s_adc_started_at_ms = now_ms;
    s_adc_state = SENSOR_ADC_BUSY;
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t*)s_adc_dma_values, (uint32_t)SENSOR_ADC_CHANNEL_COUNT) != HAL_OK) {
        s_adc_state = SENSOR_ADC_ERROR;
        return 0U;
    }

    return 1U;
}

static sensor_adc_state_t PollSensorAdcScan(uint32_t now_ms, sensor_adc_sample_t* sample) {
    sensor_adc_state_t state = s_adc_state;

    if ((state == SENSOR_ADC_BUSY) && (TimeElapsed(now_ms, s_adc_started_at_ms, SENSOR_FSR_ADC_TIMEOUT_MS) != 0U)) {
        (void)HAL_ADC_Stop_DMA(&hadc1);
        s_adc_state = SENSOR_ADC_ERROR;
        state = SENSOR_ADC_ERROR;
    }

    if (state == SENSOR_ADC_READY) {
        (void)HAL_ADC_Stop_DMA(&hadc1);
        if (sample != NULL) {
            sample->fsr_raw = s_adc_dma_values[SENSOR_ADC_FSR_INDEX];
            sample->line_left_raw = s_adc_dma_values[SENSOR_ADC_LINE_LEFT_INDEX];
            sample->line_right_raw = s_adc_dma_values[SENSOR_ADC_LINE_RIGHT_INDEX];
            sample->line_center_raw = s_adc_dma_values[SENSOR_ADC_LINE_CENTER_INDEX];
        }
        s_adc_state = SENSOR_ADC_IDLE;
    } else if (state == SENSOR_ADC_ERROR) {
        (void)HAL_ADC_Stop_DMA(&hadc1);
        s_adc_state = SENSOR_ADC_IDLE;
    }

    return state;
}

static uint32_t GetTim1ClockHz(void) {
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK2Freq();

    if ((RCC->CFGR & RCC_CFGR_PPRE2) != RCC_HCLK_DIV1) {
        timer_clock_hz *= 2U;
    }

    return timer_clock_hz;
}

static uint8_t ConfigureUltrasonicTimer(void) {
    uint32_t timer_clock_hz = GetTim1ClockHz();
    uint32_t prescaler;

    if ((timer_clock_hz < SENSOR_ULTRASONIC_TIMER_HZ) || ((timer_clock_hz % SENSOR_ULTRASONIC_TIMER_HZ) != 0U) ||
        (SENSOR_ULTRASONIC_TRIGGER_PULSE_US == 0U)) {
        return 0U;
    }

    prescaler = (timer_clock_hz / SENSOR_ULTRASONIC_TIMER_HZ) - 1U;

    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_3);
    (void)HAL_TIM_IC_Stop_IT(&htim1, TIM_CHANNEL_4);

    __HAL_TIM_DISABLE(&htim1);
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
    htim1.Init.Prescaler = prescaler;
    htim1.Init.Period = UINT16_MAX;
    __HAL_TIM_SET_PRESCALER(&htim1, prescaler);
    __HAL_TIM_SET_AUTORELOAD(&htim1, UINT16_MAX);
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    htim1.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);

    HAL_NVIC_SetPriority(TIM1_CC_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
    return 1U;
}

static void StopUltrasonicTriggerPulse(void) {
    const ultrasonic_sensor_descriptor_t* sensor;

    if ((s_ultrasonic_trigger_pulse_active == 0U) || (s_ultrasonic_active_index >= SENSOR_LOGIC_ULTRASONIC_COUNT)) {
        return;
    }

    sensor = &s_ultrasonic_sensors[s_ultrasonic_active_index];
    HAL_GPIO_WritePin(sensor->trigger_port, sensor->trigger_pin, GPIO_PIN_RESET);
    s_ultrasonic_trigger_pulse_active = 0U;
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
    __HAL_TIM_SET_AUTORELOAD(&htim1, UINT16_MAX);
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
}

static void CancelUltrasonicMeasurement(void) {
    const ultrasonic_sensor_descriptor_t* sensor;

    if (s_ultrasonic_capture_state == ULTRASONIC_CAPTURE_IDLE) {
        return;
    }

    StopUltrasonicTriggerPulse();
    if (s_ultrasonic_active_index < SENSOR_LOGIC_ULTRASONIC_COUNT) {
        sensor = &s_ultrasonic_sensors[s_ultrasonic_active_index];
        (void)HAL_TIM_IC_Stop_IT(&htim1, sensor->timer_channel);
        HAL_GPIO_WritePin(sensor->trigger_port, sensor->trigger_pin, GPIO_PIN_RESET);
    }
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_IDLE;
}

static uint8_t StartUltrasonicMeasurement(uint8_t sensor_index, uint32_t now_ms) {
    const ultrasonic_sensor_descriptor_t* sensor;

    if ((sensor_index >= SENSOR_LOGIC_ULTRASONIC_COUNT) || (s_ultrasonic_capture_state != ULTRASONIC_CAPTURE_IDLE)) {
        return 0U;
    }

    sensor = &s_ultrasonic_sensors[sensor_index];
    s_ultrasonic_active_index = sensor_index;
    s_ultrasonic_started_at_ms = now_ms;
    s_ultrasonic_rising_capture = 0U;
    s_ultrasonic_pulse_width_us = 0U;
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_WAIT_RISING;

    __HAL_TIM_DISABLE(&htim1);
    __HAL_TIM_SET_AUTORELOAD(&htim1, SENSOR_ULTRASONIC_TRIGGER_PULSE_US - 1U);
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);
    __HAL_TIM_SET_CAPTUREPOLARITY(&htim1, sensor->timer_channel, TIM_INPUTCHANNELPOLARITY_RISING);
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);

    HAL_GPIO_WritePin(sensor->trigger_port, sensor->trigger_pin, GPIO_PIN_SET);
    s_ultrasonic_trigger_pulse_active = 1U;
    if (HAL_TIM_IC_Start_IT(&htim1, sensor->timer_channel) != HAL_OK) {
        StopUltrasonicTriggerPulse();
        s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_ERROR;
        return 0U;
    }

    return 1U;
}

static void CheckUltrasonicCaptureTimeout(uint32_t now_ms) {
    const ultrasonic_sensor_descriptor_t* sensor;
    ultrasonic_capture_state_t state = s_ultrasonic_capture_state;

    if ((state != ULTRASONIC_CAPTURE_WAIT_RISING) && (state != ULTRASONIC_CAPTURE_WAIT_FALLING)) {
        return;
    }

    if (TimeElapsed(now_ms, s_ultrasonic_started_at_ms, SENSOR_ULTRASONIC_ECHO_TIMEOUT_MS) == 0U) {
        return;
    }

    StopUltrasonicTriggerPulse();
    sensor = &s_ultrasonic_sensors[s_ultrasonic_active_index];
    (void)HAL_TIM_IC_Stop_IT(&htim1, sensor->timer_channel);
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_ERROR;
}

static uint8_t TakeUltrasonicResult(ultrasonic_result_t* result) {
    ultrasonic_capture_state_t state = s_ultrasonic_capture_state;

    if ((result == NULL) || ((state != ULTRASONIC_CAPTURE_READY) && (state != ULTRASONIC_CAPTURE_ERROR))) {
        return 0U;
    }

    result->sensor_index = s_ultrasonic_active_index;
    result->pulse_width_us = s_ultrasonic_pulse_width_us;
    result->valid = (state == ULTRASONIC_CAPTURE_READY) ? 1U : 0U;
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_IDLE;
    return 1U;
}

static uint16_t PulseWidthToMillimeters(uint32_t pulse_width_us) {
    return (uint16_t)(((pulse_width_us * 10U) + 29U) / 58U);
}

static void UpdateCommonSensorError(sensor_task_context_t* context, uint32_t now_ms) {
    const sensor_logic_diagnostics_t* diagnostics;
    uint32_t current_errors;

    diagnostics = SensorLogic_GetDiagnostics(&context->logic);
    if (diagnostics == NULL) {
        return;
    }

    current_errors = SensorTask_GetEffectiveSafetyErrors(diagnostics->error_flags);
    if (current_errors == context->reported_safety_error_flags) {
        return;
    }

    if (current_errors == SENSOR_LOGIC_ERROR_NONE) {
        PublishSafetyEvent(APP_SAFETY_EVENT_SENSOR_FAULT, LINETRACER_STOP_REASON_SENSOR_FAULT, now_ms, 0U, 0U);
    } else {
        PublishSafetyEvent(APP_SAFETY_EVENT_SENSOR_FAULT, LINETRACER_STOP_REASON_SENSOR_FAULT, now_ms, 1U,
                           (uint8_t)current_errors);
    }

    context->reported_safety_error_flags = current_errors;
}

static uint8_t InitializeSensorHardware(void) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10, GPIO_PIN_RESET);

    HAL_NVIC_SetPriority(ADC_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(ADC_IRQn);

    s_adc_state = SENSOR_ADC_IDLE;
    (void)memset(s_adc_dma_values, 0, sizeof(s_adc_dma_values));
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_IDLE;
    s_ultrasonic_trigger_pulse_active = 0U;
    s_ultrasonic_timer_ready = ConfigureUltrasonicTimer();
    return s_ultrasonic_timer_ready;
}

static void InitializeContext(sensor_task_context_t* context, uint32_t now_ms) {
    app_sensor_snapshot_t initial_snapshot;

    (void)memset(context, 0, sizeof(*context));
    (void)memset(&s_latest_snapshot, 0, sizeof(s_latest_snapshot));
    (void)memset(&s_latest_diagnostics, 0, sizeof(s_latest_diagnostics));
    (void)memset(&s_latest_marker_event, 0, sizeof(s_latest_marker_event));
    s_latest_snapshot_valid = 0U;
    s_latest_marker_event_valid = 0U;
    s_marker_event_count = 0U;

    SensorLogic_Init(&context->logic, now_ms);
    SensorEventLatch_Init(&context->event_latch);
    context->last_ultrasonic_start_ms = now_ms - APP_TIMING_ULTRASONIC_PERIOD_MS;

    initial_snapshot = *SensorLogic_GetSnapshot(&context->logic);
    StoreLatestState(&context->logic, &initial_snapshot);
}

void StartSensorTask(void* argument) {
    sensor_task_context_t context;
    sensor_logic_update_t update;
    ultrasonic_result_t ultrasonic_result;
    sensor_adc_sample_t adc_sample;
    sensor_adc_state_t adc_state;
    uint32_t next_wake;
    uint32_t last_alive_ms;
    uint32_t now_ms;
    const sensor_logic_diagnostics_t* diagnostics;
    uint8_t ultrasonic_monitoring_required;
    uint8_t ultrasonic_timer_ready;

    (void)argument;

    now_ms = osKernelGetTickCount();
    InitializeContext(&context, now_ms);
    ultrasonic_timer_ready = InitializeSensorHardware();
    if (ultrasonic_timer_ready == 0U) {
        SensorLogic_MarkAllUltrasonicUnavailable(&context.logic, now_ms);
    }
    (void)memset(&adc_sample, 0, sizeof(adc_sample));
    (void)StartSensorAdcScan(now_ms);

    next_wake = osKernelGetTickCount();
    last_alive_ms = now_ms;

    for (;;) {
        now_ms = osKernelGetTickCount();
        (void)memset(&update, 0, sizeof(update));

        if (s_fsr_baseline_capture_requested != 0U) {
            SensorLogic_StartFsrBaselineCapture(&context.logic,
                                                (s_fsr_baseline_capture_mode == SENSOR_TASK_FSR_BASELINE_FOR_LOAD_OFF)
                                                    ? SENSOR_FSR_BASELINE_FOR_LOAD_OFF
                                                    : SENSOR_FSR_BASELINE_FOR_LOAD_ON);
            s_fsr_baseline_capture_requested = 0U;
        }

        adc_state = PollSensorAdcScan(now_ms, &adc_sample);
        if (adc_state == SENSOR_ADC_READY) {
            SensorLogic_UpdateFsr(&context.logic, adc_sample.fsr_raw, now_ms, &update);
            SensorLogic_UpdateLineAnalogRaw(&context.logic, adc_sample.line_left_raw, adc_sample.line_right_raw);
            SensorLogic_UpdateLineCenter(&context.logic, NormalizeLineInput(HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8)),
                                         adc_sample.line_center_raw);
            (void)StartSensorAdcScan(now_ms);
        } else if (adc_state == SENSOR_ADC_ERROR) {
            SensorLogic_MarkFsrError(&context.logic, SENSOR_LOGIC_ERROR_FSR_ADC, now_ms);
            (void)StartSensorAdcScan(now_ms);
        } else if (adc_state == SENSOR_ADC_IDLE) {
            (void)StartSensorAdcScan(now_ms);
        }

        /* Apply the newest AO samples before deriving outer tracking and the transverse marker candidate. */
        SensorLogic_UpdateLine(&context.logic, NormalizeLineInput(HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4)),
                               context.logic.line_center_black, NormalizeLineInput(HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5)),
                               now_ms, &update);

        diagnostics = SensorLogic_GetDiagnostics(&context.logic);
        ultrasonic_monitoring_required =
            (ControlTask_UltrasonicMonitoringRequired() || (diagnostics != NULL && diagnostics->obstacle_mask != 0U))
                ? 1U
                : 0U;
        SensorLogic_SetUltrasonicMonitoringEnabled(&context.logic, ultrasonic_monitoring_required, now_ms);

        if (ultrasonic_monitoring_required == 0U) {
            CancelUltrasonicMeasurement();
            context.last_ultrasonic_start_ms = now_ms;
        } else {
            CheckUltrasonicCaptureTimeout(now_ms);
            if (TakeUltrasonicResult(&ultrasonic_result) != 0U) {
                SensorLogic_UpdateUltrasonic(&context.logic, ultrasonic_result.sensor_index,
                                             PulseWidthToMillimeters(ultrasonic_result.pulse_width_us),
                                             ultrasonic_result.valid, now_ms, &update);
                context.next_ultrasonic_slot =
                    (uint8_t)((context.next_ultrasonic_slot + 1U) % sizeof(s_ultrasonic_measurement_schedule));
            }

            if ((ultrasonic_timer_ready != 0U) && (s_ultrasonic_capture_state == ULTRASONIC_CAPTURE_IDLE) &&
                (TimeElapsed(now_ms, context.last_ultrasonic_start_ms, APP_TIMING_ULTRASONIC_PERIOD_MS) != 0U)) {
                uint8_t sensor_index = s_ultrasonic_measurement_schedule[context.next_ultrasonic_slot];

                context.last_ultrasonic_start_ms = now_ms;
                if (StartUltrasonicMeasurement(sensor_index, now_ms) != 0U) {
                    SensorLogic_MarkUltrasonicStarted(&context.logic, sensor_index, now_ms);
                }
            }
        }

        SensorLogic_CheckStaleness(&context.logic, now_ms);
        UpdateCommonSensorError(&context, now_ms);
        PublishLogicSafetyChanges(&context, &update, now_ms);

        context.logic.snapshot.sampled_at_ms = now_ms;
        PublishSnapshot(&context, update.event_flags, now_ms);

        if (TimeElapsed(now_ms, last_alive_ms, APP_TIMING_HEALTH_PERIOD_MS) != 0U) {
            const sensor_logic_diagnostics_t* diagnostics = SensorLogic_GetDiagnostics(&context.logic);
            uint32_t raw_error_flags =
                (diagnostics != NULL) ? diagnostics->error_flags : (uint32_t)SENSOR_LOGIC_ERROR_NONE;

            PublishHealthEvent(APP_HEALTH_EVENT_TASK_ALIVE, now_ms, raw_error_flags);
            last_alive_ms = now_ms;
        }

        next_wake += APP_TIMING_SENSOR_PERIOD_MS;
        if (osDelayUntil(next_wake) != osOK) {
            next_wake = osKernelGetTickCount();
        }
    }
}

void ADC_IRQHandler(void) {
    HAL_ADC_IRQHandler(&hadc1);
}

void TIM1_CC_IRQHandler(void) {
    HAL_TIM_IRQHandler(&htim1);
}

void TIM1_UP_TIM10_IRQHandler(void) {
    if ((__HAL_TIM_GET_FLAG(&htim1, TIM_FLAG_UPDATE) != RESET) &&
        (__HAL_TIM_GET_IT_SOURCE(&htim1, TIM_IT_UPDATE) != RESET)) {
        __HAL_TIM_CLEAR_IT(&htim1, TIM_IT_UPDATE);
        StopUltrasonicTriggerPulse();
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
    if (hadc != &hadc1) {
        return;
    }

    s_adc_state = SENSOR_ADC_READY;
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef* hadc) {
    if (hadc != &hadc1) {
        return;
    }

    s_adc_state = SENSOR_ADC_ERROR;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef* htim) {
    const ultrasonic_sensor_descriptor_t* sensor;
    uint32_t falling_capture;
    uint32_t timer_period;

    if ((htim != &htim1) || (s_ultrasonic_active_index >= SENSOR_LOGIC_ULTRASONIC_COUNT)) {
        return;
    }

    sensor = &s_ultrasonic_sensors[s_ultrasonic_active_index];
    if (htim->Channel != sensor->active_channel) {
        return;
    }

    if (s_ultrasonic_capture_state == ULTRASONIC_CAPTURE_WAIT_RISING) {
        s_ultrasonic_rising_capture = HAL_TIM_ReadCapturedValue(htim, sensor->timer_channel);
        __HAL_TIM_SET_CAPTUREPOLARITY(htim, sensor->timer_channel, TIM_INPUTCHANNELPOLARITY_FALLING);
        s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_WAIT_FALLING;
        return;
    }

    if (s_ultrasonic_capture_state != ULTRASONIC_CAPTURE_WAIT_FALLING) {
        return;
    }

    falling_capture = HAL_TIM_ReadCapturedValue(htim, sensor->timer_channel);
    timer_period = __HAL_TIM_GET_AUTORELOAD(htim) + 1U;

    if (falling_capture >= s_ultrasonic_rising_capture) {
        s_ultrasonic_pulse_width_us = falling_capture - s_ultrasonic_rising_capture;
    } else {
        s_ultrasonic_pulse_width_us = (timer_period - s_ultrasonic_rising_capture) + falling_capture;
    }

    (void)HAL_TIM_IC_Stop_IT(htim, sensor->timer_channel);
    s_ultrasonic_capture_state = ULTRASONIC_CAPTURE_READY;
}
