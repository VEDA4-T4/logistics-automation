#ifndef SAFETY_TASK_H
#define SAFETY_TASK_H

#include <stdint.h>

#include "app_messages.h"

/*
 * ============================================================================
 * SafetyTask 공용 인터페이스
 * ============================================================================
 *
 * SafetyTask는 비상정지(E-Stop) 입력과 치명 오류를 최우선(osPriorityHigh)으로
 * 처리한다. E-Stop 입력은 CommRxTask가 양쪽 UART(투입/분류 Pi)에서 수신해
 * safetyCommandQueueHandle로 전달하며, 내부 치명 오류는
 * Safety_TriggerEmergencyStop()으로 진입한다.
 *
 * E-Stop 시퀀스:
 *   1) conveyor_motor_power_latch_disable()로 공통 모터 STBY를 즉시 LOW로 latch.
 *      (SafetyTask만 최종 차단 권한을 가진다. 두 ControlTask 상태와 무관하게 동작)
 *   2) 분류 게이트를 안전 위치로 이동(sorting_control_gate_enter_safe_state).
 *   3) 투입/분류 공정에 정지를 통지(notify_safety_stop). 이때 각 공정의 safety
 *      epoch이 증가해 대기 중인 Pi 구동 명령이 무효화(STALE)된다.
 *   4) 장치 상태를 EMERGENCY_STOP으로 게시하고 안전 EVENT를 양쪽 채널에 보고.
 *
 * Reset 시퀀스(UART_CMD_RESET_DEVICE, 양쪽 UART 수신 가능):
 *   E-Stop latch 상태에서만 유효하다. 두 공정이 모두 STOPPED로 동기화된 뒤에만
 *   해제를 요청하고, 두 공정이 모두 RELEASED로 확인된 뒤에만 STBY latch를 푼다.
 *   어느 단계든 시간 예산을 초과하면 latch를 유지한 채(fail-safe) reset을 거부
 *   보고한다.
 *
 * 논블로킹:
 *   보고는 CommTx_SendUrgent(비블로킹)를 사용하고, 해제 핸드셰이크는 큐 대기
 *   timeout으로 pacing하여 하위 우선순위 ControlTask가 실행될 수 있게 한다.
 */

/* E-Stop 원인 분류. 안전 EVENT payload의 cause 필드로 그대로 사용한다. */
typedef enum {
    SAFETY_CAUSE_NONE = 0U,
    SAFETY_CAUSE_ESTOP_INPUT_PI = 1U,   /* USART1(투입 Pi)에서 수신한 E-Stop */
    SAFETY_CAUSE_ESTOP_SORTING_PI = 2U, /* USART6(분류 Pi)에서 수신한 E-Stop */
    SAFETY_CAUSE_FATAL_ERROR = 3U       /* 내부 치명 오류(HealthTask 등) */
} safety_cause_t;

/*
 * 각 공정 ControlTask의 안전 동기화 상태.
 * input_control_task.h의 input_control_safety_sync_state_t와 값이 일치하며,
 * SortingControlTask도 동일한 값 체계를 따른다.
 */
typedef enum {
    SAFETY_SYNC_RELEASED = 0U,
    SAFETY_SYNC_STOP_REQUESTED = 1U,
    SAFETY_SYNC_STOPPED = 2U,
    SAFETY_SYNC_RELEASE_REQUESTED = 3U
} safety_sync_state_t;

/*
 * ============================================================================
 * 안전 상태 변경 EVENT (UART_CMD_EVENT)
 * ============================================================================
 *
 * UART 계약의 UART_CMD_EVENT payload [0] = event_id. heartbeat(0x01)와 같은
 * 애플리케이션 확장이며, 값은 Raspberry Pi 측과 공유해야 한다(변경 시 통보).
 *
 * payload 구조:
 *   [0]    event_id = APP_EVENT_SAFETY
 *   [1]    safety_event_kind_t
 *   [2]    safety_cause_t
 *   [3..6] timestamp_ms (Little-endian uint32, HAL_GetTick)
 *   [7]    result (RESET_COMPLETE/REJECTED에서 safety_reset_result_t, 그 외 0)
 */
#define APP_EVENT_SAFETY 0x02U

#define APP_SAFETY_EVENT_KIND_INDEX 1U
#define APP_SAFETY_EVENT_CAUSE_INDEX 2U
#define APP_SAFETY_EVENT_TIMESTAMP_INDEX 3U
#define APP_SAFETY_EVENT_RESULT_INDEX 7U
#define APP_SAFETY_EVENT_PAYLOAD_SIZE 8U

typedef enum {
    SAFETY_EVENT_ESTOP_LATCHED = 1U,
    SAFETY_EVENT_RESET_COMPLETE = 2U,
    SAFETY_EVENT_RESET_REJECTED = 3U
} safety_event_kind_t;

typedef enum {
    SAFETY_RESET_OK = 0U,
    SAFETY_RESET_INPUT_NOT_READY = 1U,
    SAFETY_RESET_SORTING_NOT_READY = 2U,
    SAFETY_RESET_TIMEOUT = 3U
} safety_reset_result_t;

/* 통계. 디버거 Live Expressions로 관찰한다. */
typedef struct {
    uint32_t estopEvents;         /* E-Stop 진입 횟수 */
    uint32_t resetCompleted;      /* 정상 해제 완료 횟수 */
    uint32_t resetRejected;       /* 해제 실패(예산 초과 등)로 latch 유지 */
    uint32_t resetIgnored;        /* E-Stop 미발생 상태의 reset 무시 */
    uint32_t reportDrops;         /* CommTx_SendUrgent 실패(양쪽 합산) */
    uint32_t controlStopFailures; /* notify_safety_stop 접수 실패 */
} safety_task_stats_t;

/* freertos.c의 __weak StartSafetyTask를 덮는 실제 구현. */
void StartSafetyTask(void* argument);

/*
 * 내부 치명 오류로 E-Stop을 요청하는 진입점. 태스크/콜백에서 호출 가능하며
 * 비블로킹이다(플래그만 설정, 실제 차단은 SafetyTask 컨텍스트에서 수행).
 */
void Safety_TriggerEmergencyStop(safety_cause_t cause);

void Safety_GetStats(safety_task_stats_t* stats);

/*
 * ============================================================================
 * 내부 처리 진입점 (StartSafetyTask 루프 및 호스트 단위 테스트에서 사용)
 * ============================================================================
 */
void SafetyTask_Init(void);
void SafetyTask_HandleSafetyCommand(const control_command_t* message);
void SafetyTask_ServicePending(void);
uint8_t SafetyTask_IsReleasing(void);

#endif /* SAFETY_TASK_H */
