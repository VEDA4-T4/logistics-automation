#include "safety_task.h"

#include <stdatomic.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "cmsis_os2.h"
#include "conveyor_motor_power.h"
#include "health_task.h"
#include "input_control_task.h"
#include "sorting_control_task.h"

/* 평상시 안전 큐 대기(ms). E-Stop/Reset 명령을 블로킹 없이 기다린다. */
#define SAFETY_QUEUE_IDLE_WAIT_MS 50U

/* 해제 핸드셰이크 진행 중 폴링 간격(ms). 하위 ControlTask에 CPU를 양보한다. */
#define SAFETY_RELEASE_POLL_MS 2U

/* 해제 각 단계의 시간 예산(폴링 횟수). 약 0.5초(2ms * 250). */
#define SAFETY_RELEASE_MAX_ATTEMPTS 250U

typedef enum { SAFETY_STATE_NORMAL = 0, SAFETY_STATE_ESTOP } safety_state_t;

typedef enum {
    SAFETY_RELEASE_IDLE = 0,
    SAFETY_RELEASE_WAIT_STOPPED,
    SAFETY_RELEASE_WAIT_RELEASED
} safety_release_phase_t;

typedef struct {
    safety_state_t state;
    safety_release_phase_t releasePhase;
    safety_cause_t cause;
    uint32_t eventTick;
    uint32_t releaseAttempts;
} safety_runtime_t;

static safety_runtime_t safety;
static safety_task_stats_t safetyStats;

static _Atomic uint8_t safetyPendingFatalFlag;
static _Atomic uint32_t safetyPendingFatalCause;

static safety_sync_state_t safety_input_sync(void) {
    /* input_control_safety_sync_state_t와 safety_sync_state_t는 값이 일치한다. */
    return (safety_sync_state_t)input_control_task_get_safety_sync_state();
}

static safety_sync_state_t safety_sorting_sync(void) {
    /* sorting_control_safety_sync_state_t와 safety_sync_state_t는 값이 일치한다. */
    return (safety_sync_state_t)sorting_control_task_get_safety_sync_state();
}

static void safety_report_event(safety_event_kind_t kind, safety_cause_t cause, uint32_t tick, uint8_t result) {
    uint8_t payload[APP_SAFETY_EVENT_PAYLOAD_SIZE];

    payload[UART_EVENT_ID_INDEX] = APP_EVENT_SAFETY;
    payload[APP_SAFETY_EVENT_KIND_INDEX] = (uint8_t)kind;
    payload[APP_SAFETY_EVENT_CAUSE_INDEX] = (uint8_t)cause;
    payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX] = (uint8_t)(tick & 0xFFU);
    payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 1U] = (uint8_t)((tick >> 8U) & 0xFFU);
    payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 2U] = (uint8_t)((tick >> 16U) & 0xFFU);
    payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 3U] = (uint8_t)((tick >> 24U) & 0xFFU);
    payload[APP_SAFETY_EVENT_RESULT_INDEX] = result;

    /* 안전 상태는 장치 전체에 영향을 주므로 항상 양쪽 채널에 보고한다. */
    if (CommTx_SendUrgent(COMM_TX_CH_INPUT, UART_CMD_EVENT, payload, APP_SAFETY_EVENT_PAYLOAD_SIZE) != 0) {
        safetyStats.reportDrops++;
    }

    if (CommTx_SendUrgent(COMM_TX_CH_SORTING, UART_CMD_EVENT, payload, APP_SAFETY_EVENT_PAYLOAD_SIZE) != 0) {
        safetyStats.reportDrops++;
    }
}

static void safety_enter_estop(safety_cause_t cause) {
    /*
     * 1) 공통 모터 STBY를 즉시 LOW로 latch한다. 두 ControlTask 상태와 무관하게
     *    가장 먼저 실제 출력을 차단한다.
     */
    conveyor_motor_power_latch_disable();

    /*
     * 2) 두 공정에 정지를 통지한다. notify는 각 공정의 safety epoch을 증가시켜
     *    대기 중인 Pi 구동 명령을 무효화한다. 분류 쪽은
     *    sorting_control_handle_safety_stop()이 모터 정지와 게이트 Home 복귀를
     *    함께 처리하므로 별도 게이트 호출이 필요 없다. 큐 접수 실패 시에도 각
     *    공정은 내부 atomic 상태로 정지를 인지하므로 통계만 기록한다.
     */
    if (input_control_task_notify_safety_stop() == 0U) {
        safetyStats.controlStopFailures++;
    }

    if (sorting_control_task_notify_safety_stop() == 0U) {
        safetyStats.controlStopFailures++;
    }

    /* E-Stop은 진행 중인 해제를 무효화한다(E-Stop이 항상 우선). */
    safety.state = SAFETY_STATE_ESTOP;
    safety.releasePhase = SAFETY_RELEASE_IDLE;
    safety.releaseAttempts = 0U;
    safety.cause = cause;
    safety.eventTick = HAL_GetTick();
    safetyStats.estopEvents++;

    /* 3) 장치 상태 게시 + 양쪽 채널 EVENT 보고. */
    CommTx_SetDeviceStatus(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_EMERGENCY_STOP);
    safety_report_event(SAFETY_EVENT_ESTOP_LATCHED, cause, safety.eventTick, 0U);
}

static void safety_begin_reset(void) {
    if (safety.state != SAFETY_STATE_ESTOP) {
        /* 걸린 E-Stop이 없으면 reset은 무의미하다. */
        safetyStats.resetIgnored++;
        return;
    }

    /* 해제 핸드셰이크 시작. STBY latch는 계속 LOW로 유지된다. */
    safety.releasePhase = SAFETY_RELEASE_WAIT_STOPPED;
    safety.releaseAttempts = 0U;
}

static void safety_reject_reset(safety_reset_result_t result) {
    /* STBY latch를 유지한 채(fail-safe) 해제를 포기한다. Pi가 재시도할 수 있다. */
    safety.releasePhase = SAFETY_RELEASE_IDLE;
    safety.releaseAttempts = 0U;
    safetyStats.resetRejected++;
    safety_report_event(SAFETY_EVENT_RESET_REJECTED, safety.cause, HAL_GetTick(), (uint8_t)result);
}

static void safety_service_release(void) {
    switch (safety.releasePhase) {
        case SAFETY_RELEASE_WAIT_STOPPED:
            if ((safety_input_sync() == SAFETY_SYNC_STOPPED) && (safety_sorting_sync() == SAFETY_SYNC_STOPPED)) {
                /* 두 공정 모두 정지 확인. 이제 해제를 요청한다. */
                (void)input_control_task_notify_safety_release();
                (void)sorting_control_task_notify_safety_release();
                safety.releasePhase = SAFETY_RELEASE_WAIT_RELEASED;
                safety.releaseAttempts = 0U;
            } else {
                safety.releaseAttempts++;
                if (safety.releaseAttempts >= SAFETY_RELEASE_MAX_ATTEMPTS) {
                    safety_reject_reset((safety_input_sync() != SAFETY_SYNC_STOPPED) ? SAFETY_RESET_INPUT_NOT_READY
                                                                                     : SAFETY_RESET_SORTING_NOT_READY);
                }
            }
            break;

        case SAFETY_RELEASE_WAIT_RELEASED:
            if ((safety_input_sync() == SAFETY_SYNC_RELEASED) && (safety_sorting_sync() == SAFETY_SYNC_RELEASED)) {
                /* 두 공정 모두 해제 확인 후에만 STBY latch를 푼다. */
                conveyor_motor_power_release_latch();
                safety.state = SAFETY_STATE_NORMAL;
                safety.releasePhase = SAFETY_RELEASE_IDLE;
                safety.releaseAttempts = 0U;
                safetyStats.resetCompleted++;

                CommTx_SetDeviceStatus(UART_DEVICE_READY, UART_ERROR_NONE);
                safety_report_event(SAFETY_EVENT_RESET_COMPLETE, safety.cause, HAL_GetTick(), (uint8_t)SAFETY_RESET_OK);
            } else {
                safety.releaseAttempts++;
                if (safety.releaseAttempts >= SAFETY_RELEASE_MAX_ATTEMPTS) {
                    safety_reject_reset(SAFETY_RESET_TIMEOUT);
                }
            }
            break;

        case SAFETY_RELEASE_IDLE:
        default:
            break;
    }
}

void SafetyTask_Init(void) {
    memset(&safety, 0, sizeof(safety));
    memset(&safetyStats, 0, sizeof(safetyStats));
    atomic_store_explicit(&safetyPendingFatalFlag, 0U, memory_order_release);
    atomic_store_explicit(&safetyPendingFatalCause, (uint32_t)SAFETY_CAUSE_FATAL_ERROR, memory_order_release);
}

void SafetyTask_HandleSafetyCommand(const control_command_t* message) {
    if (message == NULL) {
        return;
    }

    if (message->frame.command == UART_CMD_EMERGENCY_STOP) {
        safety_cause_t cause =
            (message->source == APP_UART_CHANNEL_6) ? SAFETY_CAUSE_ESTOP_SORTING_PI : SAFETY_CAUSE_ESTOP_INPUT_PI;
        safety_enter_estop(cause);
    } else if (message->frame.command == UART_CMD_RESET_DEVICE) {
        safety_begin_reset();
    } else {
        /* comm_rx가 E-Stop/Reset만 이 큐로 라우팅하므로 그 외는 무시한다. */
    }
}

void SafetyTask_ServicePending(void) {
    if (atomic_exchange_explicit(&safetyPendingFatalFlag, 0U, memory_order_acq_rel) != 0U) {
        safety_enter_estop((safety_cause_t)atomic_load_explicit(&safetyPendingFatalCause, memory_order_acquire));
    }

    if ((safety.state == SAFETY_STATE_ESTOP) && (safety.releasePhase != SAFETY_RELEASE_IDLE)) {
        safety_service_release();
    }
}

uint8_t SafetyTask_IsReleasing(void) {
    return ((safety.state == SAFETY_STATE_ESTOP) && (safety.releasePhase != SAFETY_RELEASE_IDLE)) ? 1U : 0U;
}

void Safety_TriggerEmergencyStop(safety_cause_t cause) {
    atomic_store_explicit(&safetyPendingFatalCause, (uint32_t)cause, memory_order_release);
    atomic_store_explicit(&safetyPendingFatalFlag, 1U, memory_order_release);
}

void Safety_GetStats(safety_task_stats_t* stats) {
    if (stats != NULL) {
        *stats = safetyStats;
    }
}

/*
 * freertos.c의 __weak StartSafetyTask를 덮는 실제 구현.
 */
void StartSafetyTask(void* argument) {
    control_command_t message;

    (void)argument;

    SafetyTask_Init();

    for (;;) {
        uint32_t waitMs = (SafetyTask_IsReleasing() != 0U) ? SAFETY_RELEASE_POLL_MS : SAFETY_QUEUE_IDLE_WAIT_MS;

        Health_TaskAlive(HEALTH_TASK_SAFETY);

        if ((safetyCommandQueueHandle != NULL) &&
            (osMessageQueueGet(safetyCommandQueueHandle, &message, NULL, waitMs) == osOK)) {
            SafetyTask_HandleSafetyCommand(&message);
        }

        SafetyTask_ServicePending();
    }
}
