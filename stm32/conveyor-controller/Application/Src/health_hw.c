#include "health_hw.h"

#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include "task.h"

/*
 * freertos.c(CubeMX 생성)가 정의하는 태스크 핸들. 별도 헤더로 공개하지 않고
 * 여기서만 심볼 이름으로 참조한다(freertos.c는 Core 소유라 직접 손대지 않음).
 */
extern osThreadId_t CommRxTaskHandle;
extern osThreadId_t InputControlTaskHandle;
extern osThreadId_t SortingControlTaskHandle;
extern osThreadId_t SafetyTaskHandle;
extern osThreadId_t CommTxTaskHandle;
extern osThreadId_t SensorTaskHandle;

/* IWDG/소프트/핀 리셋에도 값이 유지되는 영역. 링커스크립트의 .noinit 참고. */
static health_persisted_record_t healthPersistedRecord __attribute__((section(".noinit")));

static osThreadId_t* health_hw_task_handle(health_task_id_t id) {
    switch (id) {
        case HEALTH_TASK_COMM_RX:
            return &CommRxTaskHandle;
        case HEALTH_TASK_INPUT_CONTROL:
            return &InputControlTaskHandle;
        case HEALTH_TASK_SORTING_CONTROL:
            return &SortingControlTaskHandle;
        case HEALTH_TASK_SAFETY:
            return &SafetyTaskHandle;
        case HEALTH_TASK_COMM_TX:
            return &CommTxTaskHandle;
        case HEALTH_TASK_SENSOR:
            return &SensorTaskHandle;
        default:
            return (osThreadId_t*)0;
    }
}

void HealthHw_IwdgStart(uint32_t prescalerReg, uint32_t reload) {
    uint32_t guard;

    /* 반드시 시작(0xCCCC)이 먼저다 - LSI 클럭 도메인이 이때부터 PR/RLR 갱신을
     * 실제로 처리하기 시작한다. 순서를 바꾸면(잠금해제/쓰기를 먼저 하면) 아래
     * PVU/RVU 대기가 절대 안 풀리는 무한 대기가 된다(ST HAL의 HAL_IWDG_Init과
     * 동일한 순서를 따른다). */
    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U; /* PR/RLR 쓰기 잠금 해제 */
    IWDG->PR = prescalerReg;
    IWDG->RLR = reload;

    guard = 0U;
    while (((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U) && (guard < 100000U)) {
        /* PR/RLR가 실제로 반영될 때까지 대기(수 LSI 클럭 주기, 매우 짧음).
         * guard는 만에 하나 LSI가 못 도는 상황에서도 여기 갇히지 않기 위한
         * 방어적 상한이다(정상이면 훨씬 전에 빠져나온다). */
        guard++;
    }

    IWDG->KR = 0xAAAAU; /* 카운터를 reload로 초기화 */
}

void HealthHw_IwdgRefresh(void) {
    IWDG->KR = 0xAAAAU;
}

health_reset_cause_t HealthHw_CaptureResetCause(void) {
    uint32_t csr = RCC->CSR;
    health_reset_cause_t cause;

    if ((csr & RCC_CSR_IWDGRSTF) != 0U) {
        cause = HEALTH_RESET_WATCHDOG;
    } else if ((csr & RCC_CSR_SFTRSTF) != 0U) {
        cause = HEALTH_RESET_SOFTWARE;
    } else if ((csr & RCC_CSR_PORRSTF) != 0U) {
        cause = HEALTH_RESET_POWER_ON;
    } else if ((csr & RCC_CSR_PINRSTF) != 0U) {
        cause = HEALTH_RESET_PIN;
    } else if ((csr & RCC_CSR_BORRSTF) != 0U) {
        cause = HEALTH_RESET_BROWNOUT;
    } else {
        cause = HEALTH_RESET_UNKNOWN;
    }

    __HAL_RCC_CLEAR_RESET_FLAGS();
    return cause;
}

uint32_t HealthHw_GetStackHighWaterMark(health_task_id_t id) {
    osThreadId_t* handle = health_hw_task_handle(id);

    if ((handle == (osThreadId_t*)0) || (*handle == (osThreadId_t)0)) {
        return 0xFFFFFFFFU;
    }

    return (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)(*handle));
}

health_persisted_record_t* HealthHw_GetPersistedRecord(void) {
    return &healthPersistedRecord;
}
