#ifndef HEALTH_TASK_H
#define HEALTH_TASK_H

#include <stdint.h>

#include "app_comm_tx.h"

/*
 * ============================================================================
 * HealthTask 공용 인터페이스
 * ============================================================================
 *
 * HealthTask는 SensorTask/CommRxTask/InputControlTask/SortingControlTask/
 * SafetyTask/CommTxTask 6개 태스크의 생존 신호, stack high-water mark, Queue
 * overflow, 센서 갱신 지연, UART 오류 복구, 두 UART 채널의 단절을 감시하고 모든
 * 조건이 정상일 때만 hardware IWDG를 갱신한다(우선순위 osPriorityBelowNormal,
 * freertos.c 기존 설정 그대로).
 *
 * 태스크 생존 신호:
 *   모든 감시대상 태스크 루프가 bounded wait(<=100ms)라, 각 태스크가 루프
 *   1회마다 Health_TaskAlive(id)를 호출하면(비블로킹, atomic 증가) HealthTask가
 *   HEALTH_TASK_STALE_MS 안에 카운터 변화가 없는 태스크를 hang으로 판정할 수
 *   있다. eTaskGetState()만으로는 "정상 대기 중"과 "멈춤"을 구분 못 하므로
 *   (둘 다 Blocked) 이 협조적 heartbeat가 필요하다.
 *
 * 치명(FATAL) 판정 시:
 *   Safety_TriggerEmergencyStop(SAFETY_CAUSE_FATAL_ERROR)을 호출해 기존
 *   SafetyTask E-Stop 경로(STBY latch, 양쪽 공정 정지, EVENT 보고)를 그대로
 *   재사용한다. 동시에 .noinit 영역에 원인을 기록해 재부팅 후에도 확인
 *   가능하게 하고, 이후 해당 조건은 latch되어 IWDG 갱신도 함께 멈춘다 -
 *   그레이스풀 E-Stop이 원인을 해소하지 못하면(예: HealthTask 자신이나
 *   SafetyTask까지 응답 불능) 결국 IWDG reset이 최종 backstop이 된다.
 *
 * 공정별(비치명) 저하:
 *   한쪽 UART 채널 단절이나 센서 갱신 지연, 일시적 Queue drop, UART 오류 복구는
 *   E-Stop 없이 APP_EVENT_HEALTH로만 보고한다(완료조건 "한 공정 채널이 끊겨도
 *   영향받지 않는 공정은 계속 보고"). 지속적(연속 N사이클) Queue overflow만
 *   치명으로 승격한다.
 *
 * 유실(Queue overflow) vs 복구(UART recovery):
 *   치명 승격 판정에는 "최종적으로 버려진" 카운터만 넣는다. tx_error/tx_timeout/
 *   uartRestarts는 오류를 감지해 재시도/복구를 시작한 횟수라, 재시도가 성공하면
 *   유실이 아니다(실패해야 dropped_retry_exhausted로 잡힌다). 이걸 유실 합계에
 *   섞으면 매번 재시도 후 성공하더라도 UART 오류가 연속 N사이클 이어지기만 하면
 *   지속적 포화로 오판해 E-Stop이 걸린다. 그래서 복구 카운터는 별도 진단 지표
 *   (HEALTH_ISSUE_UART_RECOVERY)로만 보고하고 치명 승격에는 관여시키지 않는다.
 *
 * heartbeat 누락 vs IWDG 정책:
 *   HEALTH_TASK_STALE_MS(태스크 하나라도 응답 없다고 보는 기준, 300ms)는
 *   HealthHw_IwdgStart()의 타임아웃(~2s)보다 훨씬 짧다. 그래서 정상 경로에서는
 *   소프트웨어가 먼저 감지해 SafetyTask로 그레이스풀하게 넘기고, IWDG는 그
 *   소프트웨어 경로 자체가 죽었을 때만 늦게 발동하는 2단 backstop 구조다.
 */

/* 감시 대상 태스크. HEALTH_TASK_MONITORED_COUNT는 목록 끝 표식. */
typedef enum {
    HEALTH_TASK_COMM_RX = 0U,
    HEALTH_TASK_INPUT_CONTROL,
    HEALTH_TASK_SORTING_CONTROL,
    HEALTH_TASK_SAFETY,
    HEALTH_TASK_COMM_TX,
    HEALTH_TASK_SENSOR,
    HEALTH_TASK_MONITORED_COUNT
} health_task_id_t;

/* 감시 대상 태스크가 루프 1회마다 호출하는 생존 신호(비블로킹). */
void Health_TaskAlive(health_task_id_t id);

/* freertos.c의 __weak StartHealthTask를 덮는 실제 구현. */
void StartHealthTask(void* argument);

/*
 * ============================================================================
 * 치명 오류 원인 / 리셋 원인 (통계 및 .noinit 영속 기록에서 공용)
 * ============================================================================
 */
typedef enum {
    HEALTH_FATAL_NONE = 0U,
    HEALTH_FATAL_TASK_HANG = 1U,
    HEALTH_FATAL_QUEUE_OVERFLOW = 2U,
    HEALTH_FATAL_STACK_MARGIN = 3U,
    HEALTH_FATAL_MANUAL = 4U
} health_fatal_reason_t;

typedef enum {
    HEALTH_RESET_UNKNOWN = 0U,
    HEALTH_RESET_POWER_ON = 1U,
    HEALTH_RESET_PIN = 2U,
    HEALTH_RESET_BROWNOUT = 3U,
    HEALTH_RESET_SOFTWARE = 4U,
    HEALTH_RESET_WATCHDOG = 5U
} health_reset_cause_t;

/* .noinit(전원 유지되는 리셋에서만 생존)에 저장되는 마지막 치명 기록. */
typedef struct {
    uint32_t magic;
    uint32_t checksum;
    uint32_t reason;        /* health_fatal_reason_t */
    uint32_t offendingTask; /* health_task_id_t, 해당 없으면 HEALTH_TASK_MONITORED_COUNT */
    uint32_t tick;
} health_persisted_record_t;

/*
 * ============================================================================
 * 헬스 EVENT (UART_CMD_EVENT) - 공정별(비치명) 저하 보고
 * ============================================================================
 *
 * event_id 네임스페이스는 heartbeat(0x01)/CYCLE_COMPLETE(0x02)/SAFETY(0x03)와
 * 공유하므로 0x04를 쓴다(Raspberry Pi 측 공유 필요 - 아직 미공유).
 *
 * payload 구조:
 *   [0]    event_id = APP_EVENT_HEALTH
 *   [1]    health_issue_kind_t
 *   [2]    cause: comm_tx_channel_t(공정별) 또는 HEALTH_ISSUE_CAUSE_DEVICE_WIDE
 *   [3]    sensorId: HEALTH_ISSUE_SENSOR_STALE일 때만 SensorTask 채널의
 *          sensorId(UART_INPUT_SENSOR_ID_1/UART_SORTING_SENSOR_ID_1..3), 그 외
 *          kind에서는 HEALTH_ISSUE_SENSOR_ID_NONE. cause는 공정(INPUT/SORTING)
 *          단위라 분류 쪽 센서 3개(US2/US3/US4)를 구분 못 해서 추가함.
 *   [4..7] timestamp_ms (Little-endian uint32, HAL_GetTick)
 */
#define APP_EVENT_HEALTH 0x04U

#define APP_HEALTH_EVENT_KIND_INDEX 1U
#define APP_HEALTH_EVENT_CAUSE_INDEX 2U
#define APP_HEALTH_EVENT_SENSOR_ID_INDEX 3U
#define APP_HEALTH_EVENT_TIMESTAMP_INDEX 4U
#define APP_HEALTH_EVENT_PAYLOAD_SIZE 8U

#define HEALTH_ISSUE_CAUSE_DEVICE_WIDE 0xFFU
#define HEALTH_ISSUE_SENSOR_ID_NONE 0xFFU

typedef enum {
    HEALTH_ISSUE_UART_CHANNEL_TIMEOUT = 1U,
    HEALTH_ISSUE_QUEUE_OVERFLOW_TRANSIENT = 2U,
    HEALTH_ISSUE_SENSOR_STALE = 3U,
    /* UART 오류를 감지해 복구(재시도/RX 재시작)를 시작했다는 진단 보고.
     * 유실 여부와 무관하고 치명으로 승격되지 않는다. */
    HEALTH_ISSUE_UART_RECOVERY = 4U
} health_issue_kind_t;

/* 통계. 디버거 Live Expressions로 관찰한다. */
typedef struct {
    uint32_t taskHangEvents[HEALTH_TASK_MONITORED_COUNT];
    uint32_t stackMarginEvents[HEALTH_TASK_MONITORED_COUNT];
    uint32_t uartChannelTimeouts[COMM_TX_CH_COUNT];
    uint32_t queueOverflowTransient;
    uint32_t queueOverflowFatal;
    uint32_t sensorStaleEvents;
    uint32_t uartRecoveryEvents;
    uint32_t watchdogRefreshes;
    uint32_t watchdogRefreshSkips;
    uint32_t fatalTriggers;
    uint32_t reportDrops;
} health_task_stats_t;

void Health_GetStats(health_task_stats_t* stats);

/* 이번 부팅의 리셋 원인(부팅 직후 1회 캡처, 이후 고정값). */
health_reset_cause_t Health_GetResetCause(void);

/* 직전 부팅(또는 그 이전 어느 부팅)에서 남긴 마지막 치명 기록을 복사한다.
 * 유효한 기록이 없으면 reason=HEALTH_FATAL_NONE으로 채운다. */
void Health_GetLastPersistedRecord(health_persisted_record_t* record);

/*
 * ============================================================================
 * 내부 처리 진입점 (StartHealthTask 루프 및 호스트 테스트에서 사용)
 * ============================================================================
 */
void HealthTask_Init(void);
void HealthTask_RunCycle(void);

#endif /* HEALTH_TASK_H */
