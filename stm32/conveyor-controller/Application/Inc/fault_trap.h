#ifndef FAULT_TRAP_H
#define FAULT_TRAP_H

#include <stdint.h>

/*
 * ============================================================================
 * 무음 정지(silent hang) 블랙박스
 * ============================================================================
 *
 * 이 펌웨어에는 "아무 흔적도 남기지 않고 영원히 멈추는" 경로가 여럿 있다.
 *
 *   - HardFault/MemManage/BusFault/UsageFault 핸들러의 while(1)
 *   - Error_Handler()의 __disable_irq() + while(1)
 *   - configASSERT() 실패 시 taskDISABLE_INTERRUPTS() + for(;;)
 *   - 스택 오버플로(검사가 꺼져 있으면 메모리만 조용히 손상됨)
 *
 * 이 경로에 빠지면 모든 태스크가 멈추므로 HealthTask도 IWDG를 갱신하지 못하고,
 * 약 2초 뒤 IWDG가 MCU 전체를 리셋한다. 리셋되면 RAM이 날아가기 때문에
 * "왜 멈췄는지"를 알려줄 근거가 아무것도 남지 않는다 - 밖에서 보면 그저
 * 두 UART가 동시에 죽었다가 2초 뒤 되살아난 것처럼만 보인다.
 * (2026-08-07 실기기에서 healthResetCause=WATCHDOG, FATAL 기록 없음으로 확인)
 *
 * 그래서 멈추기 직전에 원인을 .noinit에 적어두고 정지한다. .noinit은 IWDG/
 * 소프트/핀 리셋을 넘어 값이 유지되므로, 재부팅 후 이 기록을 읽으면 어느
 * 덫에 걸렸는지 즉시 알 수 있다. health_hw.c의 healthPersistedRecord와 같은
 * 규약을 쓴다(magic을 마지막에 기록해 중간에 리셋되면 무효로 남게 한다).
 *
 * 읽는 법: 디버거 Live Expressions에 faultTrapRecord를 넣거나,
 *          FaultTrap_GetLastRecord()로 복사해 확인한다.
 */

typedef enum {
    FAULT_TRAP_NONE = 0U,
    FAULT_TRAP_HARDFAULT = 1U,
    FAULT_TRAP_MEMMANAGE = 2U,
    FAULT_TRAP_BUSFAULT = 3U,
    FAULT_TRAP_USAGEFAULT = 4U,
    FAULT_TRAP_ERROR_HANDLER = 5U, /* HAL Error_Handler() */
    FAULT_TRAP_ASSERT = 6U,        /* configASSERT() 실패 */
    FAULT_TRAP_STACK_OVERFLOW = 7U,
    FAULT_TRAP_MALLOC_FAILED = 8U
} fault_trap_kind_t;

/* context에 담는 문자열(태스크명/파일명) 최대 길이(널 포함). */
#define FAULT_TRAP_CONTEXT_SIZE 20U

/* 스택 프레임을 어느 스택에서 읽었는지. */
typedef enum {
    FAULT_TRAP_STACK_NONE = 0U,
    FAULT_TRAP_STACK_PSP = 1U, /* 태스크 컨텍스트에서 발생 */
    FAULT_TRAP_STACK_MSP = 2U  /* 인터럽트/커널 컨텍스트에서 발생 */
} fault_trap_stack_t;

typedef struct {
    uint32_t magic;
    uint32_t checksum;
    uint32_t kind; /* fault_trap_kind_t */
    uint32_t tick; /* 기록 시점의 HAL_GetTick() */

    /* Cortex-M 폴트 상태 레지스터(폴트 계열에서만 유효). */
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;
    uint32_t mmfar;

    /* 예외 스택 프레임에서 뽑은 값. 폴트가 아니면 pc에 호출자 주소가 들어간다. */
    uint32_t pc;
    uint32_t lr;
    uint32_t psr;

    /* 프레임 판정에 쓴 원본 스택 포인터(휴리스틱이 빗나가도 손으로 볼 수 있게). */
    uint32_t msp;
    uint32_t psp;
    uint32_t stackUsed; /* fault_trap_stack_t */

    uint32_t line;                          /* configASSERT의 __LINE__ */
    char context[FAULT_TRAP_CONTEXT_SIZE];  /* 태스크명 또는 파일명(뒤쪽 우선) */
} fault_trap_record_t;

/*
 * 폴트 예외 핸들러에서 호출한다. SCB의 폴트 상태와 예외 스택 프레임을 읽어
 * 기록한다. 호출 후 반환되므로 호출자가 기존처럼 while(1)로 정지하면 된다.
 *
 * 주의: 핸들러가 naked가 아니라 프롤로그가 이미 MSP를 밀어놓았을 수 있어
 * MSP 기반 프레임은 어긋날 수 있다. 반면 태스크에서 발생한 폴트의 PSP는
 * 영향을 받지 않으므로 그대로 정확하다(대부분의 경우가 여기 해당한다).
 * 판정이 애매하면 msp/psp 원본 값을 함께 기록해 두었으니 그것으로 본다.
 */
void FaultTrap_CaptureFault(fault_trap_kind_t kind);

/* Error_Handler처럼 호출자 주소만 남기면 되는 경로. */
void FaultTrap_CaptureCaller(fault_trap_kind_t kind, uint32_t caller);

/* configASSERT 실패 경로. file은 경로 뒤쪽(파일명)만 보관한다. */
void FaultTrap_CaptureAssert(const char* file, uint32_t line);

/* FreeRTOS 훅에서 호출한다. name이 NULL이면 context를 비운다. */
void FaultTrap_CaptureTask(fault_trap_kind_t kind, const char* name);

/*
 * 마지막 기록을 복사한다. magic/checksum이 맞을 때만 1을 돌려주고,
 * 그렇지 않으면 record를 0으로 채우고 kind=FAULT_TRAP_NONE으로 두고 0을 돌려준다.
 */
uint8_t FaultTrap_GetLastRecord(fault_trap_record_t* record);

/* 기록을 무효화한다(확인 후 다음 발생만 보고 싶을 때). */
void FaultTrap_Clear(void);

#endif /* FAULT_TRAP_H */
