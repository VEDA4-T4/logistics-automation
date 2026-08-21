#ifdef NDEBUG
#undef NDEBUG
#endif

#include "fault_trap.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "stm32f4xx_hal.h"

/*
 * ============================================================================
 * FaultTrap(.noinit 블랙박스) 테스트
 * ============================================================================
 *
 * 이 기록은 무음 정지 직전에 남기고 IWDG 리셋 뒤에야 읽는다. 그래서 영속
 * 규약(magic을 마지막에, checksum은 나머지 전체)이 틀리면 정작 사고가 났을 때
 * 아무것도 못 건지거나, 더 나쁘게는 쓰레기 값을 진짜 기록으로 오독한다.
 * 여기서는 그 규약만 검증한다.
 *
 * 예외 스택 프레임 선택(PSP/MSP 판정, PC가 FLASH 범위인지)은 실제 스택
 * 포인터가 있어야 의미가 있고, 호스트는 64비트 포인터라 기록 구조의
 * uint32_t로 왕복시킬 수 없다. 그 부분은 실기기 동작으로 확인한다.
 */

/* ---- fake: CMSIS/HAL ---- */
SCB_FakeTypeDef fakeScb;
uint32_t fakeMsp;
uint32_t fakePsp;

static uint32_t fakeTick;

uint32_t HAL_GetTick(void) {
    return fakeTick;
}

static void reset_all(void) {
    memset(&fakeScb, 0, sizeof(fakeScb));
    fakeMsp = 0U;
    fakePsp = 0U;
    fakeTick = 0U;
    FaultTrap_Clear();
}

/*
 * 기록이 없으면 유효하다고 답하면 안 된다. 전원이 완전히 끊겼다 들어오면
 * .noinit이 쓰레기로 차는데, 그걸 사고 기록으로 오독하면 엉뚱한 곳을 판다.
 * (2026-08-07 실기기에서 healthPersistedRecord가 정확히 이 상태였다)
 */
static void test_no_record_reports_none(void) {
    fault_trap_record_t record;

    reset_all();

    assert(FaultTrap_GetLastRecord(&record) == 0U);
    assert(record.kind == (uint32_t)FAULT_TRAP_NONE);
}

static void test_capture_caller_roundtrips(void) {
    fault_trap_record_t record;

    reset_all();
    fakeTick = 12345U;
    fakeMsp = 0x20001000U;
    fakePsp = 0x20001200U;

    FaultTrap_CaptureCaller(FAULT_TRAP_ERROR_HANDLER, 0x08009ABCU);

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.kind == (uint32_t)FAULT_TRAP_ERROR_HANDLER);
    assert(record.pc == 0x08009ABCU);
    assert(record.tick == 12345U);
    assert(record.msp == 0x20001000U);
    assert(record.psp == 0x20001200U);
}

/*
 * __FILE__은 빌드 경로가 통째로 붙어 20바이트를 훌쩍 넘는다. 앞을 버리고
 * 파일명 쪽을 남겨야 어느 파일인지 알 수 있다.
 */
static void test_assert_keeps_file_tail_and_line(void) {
    fault_trap_record_t record;

    reset_all();

    FaultTrap_CaptureAssert("C:/very/long/build/path/queue.c", 815U);

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.kind == (uint32_t)FAULT_TRAP_ASSERT);
    assert(record.line == 815U);
    assert(strlen(record.context) < FAULT_TRAP_CONTEXT_SIZE);
    assert(strstr(record.context, "queue.c") != NULL);
}

/* 짧은 이름은 잘리지 않고 그대로 들어가야 한다. */
static void test_short_context_is_not_truncated(void) {
    fault_trap_record_t record;

    reset_all();

    FaultTrap_CaptureTask(FAULT_TRAP_STACK_OVERFLOW, "SensorTask");

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.kind == (uint32_t)FAULT_TRAP_STACK_OVERFLOW);
    assert(strcmp(record.context, "SensorTask") == 0);
}

static void test_null_context_is_empty(void) {
    fault_trap_record_t record;

    reset_all();

    FaultTrap_CaptureTask(FAULT_TRAP_MALLOC_FAILED, NULL);

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.context[0] == '\0');
}

/* 확인한 뒤 지우면 다음 사고만 깨끗하게 볼 수 있어야 한다. */
static void test_clear_invalidates(void) {
    fault_trap_record_t record;

    reset_all();
    FaultTrap_CaptureCaller(FAULT_TRAP_ERROR_HANDLER, 0x08001000U);
    assert(FaultTrap_GetLastRecord(&record) == 1U);

    FaultTrap_Clear();

    assert(FaultTrap_GetLastRecord(&record) == 0U);
    assert(record.kind == (uint32_t)FAULT_TRAP_NONE);
}

/* 연달아 걸리면 마지막 것이 진짜 원인에 가깝다. */
static void test_latest_record_wins(void) {
    fault_trap_record_t record;

    reset_all();
    FaultTrap_CaptureCaller(FAULT_TRAP_ERROR_HANDLER, 0x08001000U);
    FaultTrap_CaptureTask(FAULT_TRAP_MALLOC_FAILED, "CommTxTask");

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.kind == (uint32_t)FAULT_TRAP_MALLOC_FAILED);
    assert(strcmp(record.context, "CommTxTask") == 0);
}

/*
 * 이전 기록이 남아 있는 상태에서 새로 기록해도 앞 기록의 잔재가 섞이면 안 된다.
 * (kind만 바꾸고 나머지를 안 지우면 엉뚱한 파일명/라인이 따라붙는다)
 */
static void test_new_record_does_not_inherit_previous_fields(void) {
    fault_trap_record_t record;

    reset_all();
    FaultTrap_CaptureAssert("queue.c", 815U);

    FaultTrap_CaptureCaller(FAULT_TRAP_ERROR_HANDLER, 0x08002000U);

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.line == 0U);
    assert(record.context[0] == '\0');
}

/* 폴트 상태 레지스터는 폴트 계열이 아니면 0으로 남아야 오독하지 않는다. */
static void test_non_fault_kinds_leave_status_zero(void) {
    fault_trap_record_t record;

    reset_all();
    fakeScb.CFSR = 0x00008200U;
    fakeScb.HFSR = 0x40000000U;

    FaultTrap_CaptureCaller(FAULT_TRAP_ERROR_HANDLER, 0x08003000U);

    assert(FaultTrap_GetLastRecord(&record) == 1U);
    assert(record.cfsr == 0U);
    assert(record.hfsr == 0U);
    assert(record.bfar == 0U);
    assert(record.mmfar == 0U);
}

int main(void) {
    test_no_record_reports_none();
    test_capture_caller_roundtrips();
    test_assert_keeps_file_tail_and_line();
    test_short_context_is_not_truncated();
    test_null_context_is_empty();
    test_clear_invalidates();
    test_latest_record_wins();
    test_new_record_does_not_inherit_previous_fields();
    test_non_fault_kinds_leave_status_zero();
    return 0;
}
