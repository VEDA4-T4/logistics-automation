#ifndef SORTING_CONTROL_TASK_H
#define SORTING_CONTROL_TASK_H

#include <stdint.h>

#include "safety_task.h"

/*
 * ============================================================================
 * SortingControlTask 안전 연동 seam
 * ============================================================================
 *
 * SortingControlTask의 실제 소스는 아직 없다(현재 freertos.c의 weak stub만
 * 존재). SafetyTask는 InputControlTask와 대칭인 아래 인터페이스를 통해 분류
 * 공정을 정지/해제하고 동기화 상태를 확인하며, 게이트를 안전 위치로 이동한다.
 *
 * SafetyTask 브랜치는 아래 함수들의 weak 기본 구현
 * (sorting_control_task_safety_stub.c: 분류 공정을 이상적으로 즉시 정지/해제
 * 완료하는 것으로 간주)을 제공하므로, SortingControlTask가 없어도 SafetyTask가
 * 단독으로 링크·동작한다.
 *
 * SortingControlTask 구현자는 InputControlTask와 동일한 방식으로 strong 정의를
 * 제공해야 한다:
 *   - notify_safety_stop: STBY latch는 SafetyTask가 이미 내렸다는 전제로 채널
 *     정지를 확정하고, 분류 명령 safety epoch을 증가시켜 대기 명령을 무효화한다.
 *   - get_safety_sync_state: RELEASED -> STOP_REQUESTED -> STOPPED ->
 *     RELEASE_REQUESTED -> RELEASED 순으로 진행한다.
 *   - notify_safety_release: STOPPED에서만 접수한다.
 * ==========================================================================*/

/* 분류 공정에 정지를 통지한다. 정상 접수 시 1. */
uint8_t sorting_control_task_notify_safety_stop(void);

/* 분류 공정에 해제를 요청한다. STOPPED 상태에서만 접수하며, 접수 시 1. */
uint8_t sorting_control_task_notify_safety_release(void);

/* 분류 공정의 현재 안전 동기화 상태. */
safety_sync_state_t sorting_control_task_get_safety_sync_state(void);

/* 분류 게이트를 안전 위치로 즉시 이동한다(출력 차단 계열). */
void sorting_control_gate_enter_safe_state(void);

#endif /* SORTING_CONTROL_TASK_H */
