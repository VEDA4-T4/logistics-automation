#include "fault_trap.h"

#include <string.h>

#include "stm32f4xx_hal.h"

/* IWDG/소프트/핀 리셋에도 값이 유지되는 영역. 링커스크립트의 .noinit 참고.
 * health_hw.c의 healthPersistedRecord와 같은 영역을 쓴다. */
static fault_trap_record_t faultTrapRecord __attribute__((section(".noinit")));

#define FAULT_TRAP_MAGIC 0x46545231U /* "FTR1" */

/* 링커스크립트의 MEMORY와 같은 값. 스택 프레임이 진짜인지 거르는 데만 쓴다. */
#define FAULT_TRAP_RAM_START 0x20000000U
#define FAULT_TRAP_RAM_END 0x20018000U /* 96K */
#define FAULT_TRAP_FLASH_START 0x08000000U
#define FAULT_TRAP_FLASH_END 0x08080000U /* 512K */

/* 예외 스택 프레임(부동소수점 미사용): R0 R1 R2 R3 R12 LR PC xPSR. */
#define FAULT_TRAP_FRAME_WORDS 8U
#define FAULT_TRAP_FRAME_LR_INDEX 5U
#define FAULT_TRAP_FRAME_PC_INDEX 6U
#define FAULT_TRAP_FRAME_PSR_INDEX 7U

/*
 * magic/checksum을 뺀 나머지 전체를 섞는다. 전원이 끊겼다 들어오면 .noinit이
 * 쓰레기 값으로 차는데, magic만으로는 우연히 일치할 수 있어 함께 검사한다.
 */
static uint32_t fault_trap_checksum(const fault_trap_record_t* record) {
    const uint32_t* words = (const uint32_t*)(const void*)&record->kind;
    uint32_t count = (uint32_t)((sizeof(*record) - (2U * sizeof(uint32_t))) / sizeof(uint32_t));
    uint32_t sum = 0xA5A5A5A5U;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        sum ^= words[i] * 0x9E3779B1U;
        sum = (sum << 1U) | (sum >> 31U);
    }

    return sum;
}

/*
 * 프레임 포인터가 진짜 예외 스택 프레임을 가리키는지 본다.
 *
 * 폴트 핸들러가 naked가 아니라 프롤로그가 MSP를 이미 밀어놓았을 수 있어서,
 * MSP를 그대로 프레임으로 믿으면 엉뚱한 값을 읽는다. 스택된 PC가 FLASH
 * 범위 안인지 확인하면 그런 경우를 대부분 걸러낼 수 있다.
 */
static uint8_t fault_trap_frame_ok(const uint32_t* frame) {
    /* 타겟은 32비트라 폭이 같지만, 호스트 테스트 빌드(64비트)에서 경고가
     * 나지 않도록 uintptr_t를 거친다. */
    uint32_t address = (uint32_t)(uintptr_t)frame;
    uint32_t pc;

    if ((address & 3U) != 0U) {
        return 0U;
    }

    /* 프레임 8워드가 전부 RAM 안에 들어와야 한다. */
    if ((address < FAULT_TRAP_RAM_START) || (address > (FAULT_TRAP_RAM_END - (FAULT_TRAP_FRAME_WORDS * 4U)))) {
        return 0U;
    }

    pc = frame[FAULT_TRAP_FRAME_PC_INDEX];
    return ((pc >= FAULT_TRAP_FLASH_START) && (pc < FAULT_TRAP_FLASH_END)) ? 1U : 0U;
}

/*
 * 기록을 시작한다. magic을 먼저 지워서, 쓰는 도중에 리셋되더라도 반쯤 쓰인
 * 기록이 유효한 것처럼 보이지 않게 한다(health_persist_fatal과 같은 규약).
 *
 * 지역 변수에 모았다가 한 번에 복사하지 않고 직접 쓰는 이유는, 스택 오버플로로
 * 들어온 경우에도 스택을 더 쓰지 않기 위해서다.
 */
static void fault_trap_begin(fault_trap_kind_t kind) {
    faultTrapRecord.magic = 0U;

    faultTrapRecord.kind = (uint32_t)kind;
    faultTrapRecord.tick = HAL_GetTick();
    faultTrapRecord.cfsr = 0U;
    faultTrapRecord.hfsr = 0U;
    faultTrapRecord.bfar = 0U;
    faultTrapRecord.mmfar = 0U;
    faultTrapRecord.pc = 0U;
    faultTrapRecord.lr = 0U;
    faultTrapRecord.psr = 0U;
    faultTrapRecord.msp = 0U;
    faultTrapRecord.psp = 0U;
    faultTrapRecord.stackUsed = (uint32_t)FAULT_TRAP_STACK_NONE;
    faultTrapRecord.line = 0U;
    memset(faultTrapRecord.context, 0, sizeof(faultTrapRecord.context));
}

/* checksum을 채우고 마지막에 magic을 세운다. */
static void fault_trap_commit(void) {
    faultTrapRecord.checksum = fault_trap_checksum(&faultTrapRecord);
    faultTrapRecord.magic = FAULT_TRAP_MAGIC;
}

/* 경로가 길면 앞쪽(디렉터리)보다 뒤쪽(파일명)이 쓸모 있으므로 뒤를 남긴다. */
static void fault_trap_set_context(const char* text) {
    uint32_t length = 0U;
    uint32_t start;
    uint32_t i;

    if (text == NULL) {
        return;
    }

    while ((length < 255U) && (text[length] != '\0')) {
        length++;
    }

    start = (length >= (FAULT_TRAP_CONTEXT_SIZE - 1U)) ? (length - (FAULT_TRAP_CONTEXT_SIZE - 1U)) : 0U;

    for (i = 0U; i < (FAULT_TRAP_CONTEXT_SIZE - 1U); i++) {
        char character = text[start + i];

        if (character == '\0') {
            break;
        }

        faultTrapRecord.context[i] = character;
    }
}

void FaultTrap_CaptureFault(fault_trap_kind_t kind) {
    const uint32_t* frame;

    fault_trap_begin(kind);

    /* BFAR/MMFAR은 CFSR의 BFARVALID/MMARVALID가 설 때만 의미가 있다.
     * 판단은 읽는 사람이 하도록 원본을 그대로 남긴다. */
    faultTrapRecord.cfsr = SCB->CFSR;
    faultTrapRecord.hfsr = SCB->HFSR;
    faultTrapRecord.bfar = SCB->BFAR;
    faultTrapRecord.mmfar = SCB->MMFAR;
    faultTrapRecord.msp = __get_MSP();
    faultTrapRecord.psp = __get_PSP();

    /* 태스크에서 난 폴트는 PSP에 프레임이 그대로 남아 있어 정확하다.
     * PSP가 아니면 MSP를 시도하되, 프롤로그 때문에 어긋날 수 있다. */
    frame = (const uint32_t*)(uintptr_t)faultTrapRecord.psp;

    if (fault_trap_frame_ok(frame) != 0U) {
        faultTrapRecord.stackUsed = (uint32_t)FAULT_TRAP_STACK_PSP;
    } else {
        frame = (const uint32_t*)(uintptr_t)faultTrapRecord.msp;

        if (fault_trap_frame_ok(frame) != 0U) {
            faultTrapRecord.stackUsed = (uint32_t)FAULT_TRAP_STACK_MSP;
        } else {
            frame = (const uint32_t*)0;
        }
    }

    if (frame != (const uint32_t*)0) {
        faultTrapRecord.lr = frame[FAULT_TRAP_FRAME_LR_INDEX];
        faultTrapRecord.pc = frame[FAULT_TRAP_FRAME_PC_INDEX];
        faultTrapRecord.psr = frame[FAULT_TRAP_FRAME_PSR_INDEX];
    }

    fault_trap_commit();
}

void FaultTrap_CaptureCaller(fault_trap_kind_t kind, uint32_t caller) {
    fault_trap_begin(kind);
    faultTrapRecord.pc = caller;
    faultTrapRecord.msp = __get_MSP();
    faultTrapRecord.psp = __get_PSP();
    fault_trap_commit();
}

void FaultTrap_CaptureAssert(const char* file, uint32_t line) {
    fault_trap_begin(FAULT_TRAP_ASSERT);
    faultTrapRecord.line = line;
    faultTrapRecord.msp = __get_MSP();
    faultTrapRecord.psp = __get_PSP();
    fault_trap_set_context(file);
    fault_trap_commit();
}

void FaultTrap_CaptureTask(fault_trap_kind_t kind, const char* name) {
    fault_trap_begin(kind);
    faultTrapRecord.msp = __get_MSP();
    faultTrapRecord.psp = __get_PSP();
    fault_trap_set_context(name);
    fault_trap_commit();
}

uint8_t FaultTrap_GetLastRecord(fault_trap_record_t* record) {
    if (record == NULL) {
        return 0U;
    }

    if ((faultTrapRecord.magic == FAULT_TRAP_MAGIC) &&
        (faultTrapRecord.checksum == fault_trap_checksum(&faultTrapRecord))) {
        *record = faultTrapRecord;
        return 1U;
    }

    memset(record, 0, sizeof(*record));
    record->kind = (uint32_t)FAULT_TRAP_NONE;
    return 0U;
}

void FaultTrap_Clear(void) {
    faultTrapRecord.magic = 0U;
}
