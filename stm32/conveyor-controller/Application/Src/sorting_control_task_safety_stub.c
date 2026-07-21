#include "sorting_control_task.h"

/*
 * ============================================================================
 * SortingControlTask 안전 seam의 임시 weak 기본 구현
 * ============================================================================
 *
 * SortingControlTask 소스가 아직 없으므로, 분류 공정을 "이상적으로 즉시 정지/
 * 해제 완료"하는 것으로 간주하는 weak 기본 구현을 제공한다. 이를 통해
 * SafetyTask가 SortingControlTask 없이도 링크되고 해제 핸드셰이크가 끝까지
 * 진행된다.
 *
 * SortingControlTask 구현자가 동일한 시그니처의 strong 정의를 제공하면 이
 * 파일의 구현은 링커에 의해 대체된다.
 */

static safety_sync_state_t sortingStubSyncState = SAFETY_SYNC_RELEASED;

__attribute__((weak)) uint8_t sorting_control_task_notify_safety_stop(void) {
    /* 채널 정지를 즉시 확정한 것으로 간주한다. */
    sortingStubSyncState = SAFETY_SYNC_STOPPED;
    return 1U;
}

__attribute__((weak)) uint8_t sorting_control_task_notify_safety_release(void) {
    if (sortingStubSyncState != SAFETY_SYNC_STOPPED) {
        return 0U;
    }

    sortingStubSyncState = SAFETY_SYNC_RELEASED;
    return 1U;
}

__attribute__((weak)) safety_sync_state_t sorting_control_task_get_safety_sync_state(void) {
    return sortingStubSyncState;
}

__attribute__((weak)) void sorting_control_gate_enter_safe_state(void) {
    /* 실제 게이트 GPIO는 SortingControlTask 파트 소유이다. 스텁은 동작 없음. */
}
