#include "health_hw.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include "task.h"

#define HEALTH_BACKUP_MAGIC 0x484C5448UL /* "HLTH" */
#define HEALTH_BACKUP_VERSION 1UL

#define HEALTH_BACKUP_INDEX_MAGIC 0U
#define HEALTH_BACKUP_INDEX_VERSION 1U
#define HEALTH_BACKUP_INDEX_REASON 2U
#define HEALTH_BACKUP_INDEX_SOURCE 3U
#define HEALTH_BACKUP_INDEX_DETAIL 4U
#define HEALTH_BACKUP_INDEX_OCCURRED_AT 5U
#define HEALTH_BACKUP_INDEX_RESET_CAUSE 6U
#define HEALTH_BACKUP_INDEX_CHECKSUM 7U

extern osThreadId_t SensorTaskHandle;
extern osThreadId_t CommRxTaskHandle;
extern osThreadId_t ControlTaskHandle;
extern osThreadId_t SafetyTaskHandle;
extern osThreadId_t CommTxTaskHandle;
extern osThreadId_t HealthTaskHandle;
extern osThreadId_t UnloadTaskHandle;

static volatile uint32_t* HealthHw_BackupRegister(uint32_t index) {
    switch (index) {
        case 0U:
            return &RTC->BKP0R;
        case 1U:
            return &RTC->BKP1R;
        case 2U:
            return &RTC->BKP2R;
        case 3U:
            return &RTC->BKP3R;
        case 4U:
            return &RTC->BKP4R;
        case 5U:
            return &RTC->BKP5R;
        case 6U:
            return &RTC->BKP6R;
        case 7U:
            return &RTC->BKP7R;
        default:
            return NULL;
    }
}

static void HealthHw_EnableBackupAccess(void) {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_DBP;
}

static uint32_t HealthHw_BackupRead(uint32_t index) {
    volatile uint32_t* reg = HealthHw_BackupRegister(index);
    return (reg != NULL) ? *reg : 0U;
}

static void HealthHw_BackupWrite(uint32_t index, uint32_t value) {
    volatile uint32_t* reg = HealthHw_BackupRegister(index);

    if (reg != NULL) {
        *reg = value;
    }
}

static uint32_t HealthHw_RecordChecksum(uint32_t reason, uint32_t source, uint32_t detail, uint32_t occurred_at_ms) {
    return 0xA5C39E71UL ^ (reason * 0x9E3779B1UL) ^ (source * 0x85EBCA77UL) ^ (detail * 0xC2B2AE3DUL) ^
           (occurred_at_ms * 0x27D4EB2FUL);
}

static osThreadId_t HealthHw_TaskHandle(app_task_id_t task) {
    switch (task) {
        case APP_TASK_SENSOR:
            return SensorTaskHandle;
        case APP_TASK_COMM_RX:
            return CommRxTaskHandle;
        case APP_TASK_CONTROL:
            return ControlTaskHandle;
        case APP_TASK_SAFETY:
            return SafetyTaskHandle;
        case APP_TASK_COMM_TX:
            return CommTxTaskHandle;
        case APP_TASK_HEALTH:
            return HealthTaskHandle;
        case APP_TASK_UNLOAD:
            return UnloadTaskHandle;
        case APP_TASK_COUNT:
        default:
            return NULL;
    }
}

void HealthHw_StartWatchdog(uint32_t prescaler_reg, uint32_t reload) {
    uint32_t guard = 0U;

#if defined(DEBUG)
    /* Keep breakpoint-based CubeIDE inspection from causing an accidental reset. */
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
#endif

    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U;
    IWDG->PR = prescaler_reg;
    IWDG->RLR = reload;

    while (((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U) && (guard < 100000U)) {
        ++guard;
    }

    IWDG->KR = 0xAAAAU;
}

void HealthHw_RefreshWatchdog(void) {
    IWDG->KR = 0xAAAAU;
}

health_reset_cause_t HealthHw_CaptureResetCause(void) {
    const uint32_t reset_flags = RCC->CSR;
    health_reset_cause_t cause;

    if ((reset_flags & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) != 0U) {
        cause = HEALTH_RESET_WATCHDOG;
    } else if ((reset_flags & RCC_CSR_SFTRSTF) != 0U) {
        cause = HEALTH_RESET_SOFTWARE;
    } else if ((reset_flags & RCC_CSR_BORRSTF) != 0U) {
        cause = HEALTH_RESET_BROWNOUT;
    } else if ((reset_flags & RCC_CSR_PORRSTF) != 0U) {
        cause = HEALTH_RESET_POWER_ON;
    } else if ((reset_flags & RCC_CSR_PINRSTF) != 0U) {
        cause = HEALTH_RESET_PIN;
    } else {
        cause = HEALTH_RESET_UNKNOWN;
    }

    HealthHw_EnableBackupAccess();
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_RESET_CAUSE, (uint32_t)cause);
    RCC->CSR |= RCC_CSR_RMVF;
    return cause;
}

void HealthHw_StoreFault(const health_fault_record_t* fault) {
    uint32_t checksum;

    if ((fault == NULL) || (fault->reason <= HEALTH_FAULT_NONE) || (fault->reason >= HEALTH_FAULT_COUNT) ||
        ((uint32_t)fault->source_task >= (uint32_t)APP_TASK_COUNT)) {
        return;
    }

    HealthHw_EnableBackupAccess();
    checksum = HealthHw_RecordChecksum((uint32_t)fault->reason, (uint32_t)fault->source_task, fault->detail,
                                       fault->occurred_at_ms);

    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_MAGIC, 0U);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_VERSION, HEALTH_BACKUP_VERSION);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_REASON, (uint32_t)fault->reason);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_SOURCE, (uint32_t)fault->source_task);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_DETAIL, fault->detail);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_OCCURRED_AT, fault->occurred_at_ms);
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_CHECKSUM, checksum);
    __DSB();
    HealthHw_BackupWrite(HEALTH_BACKUP_INDEX_MAGIC, HEALTH_BACKUP_MAGIC);
    __DSB();
}

uint8_t HealthHw_LoadPersistedRecord(health_persisted_record_t* record) {
    uint32_t reason;
    uint32_t source;
    uint32_t detail;
    uint32_t occurred_at_ms;
    uint32_t reset_cause;
    uint32_t expected_checksum;

    if (record == NULL) {
        return 0U;
    }

    (void)memset(record, 0, sizeof(*record));
    HealthHw_EnableBackupAccess();
    if ((HealthHw_BackupRead(HEALTH_BACKUP_INDEX_MAGIC) != HEALTH_BACKUP_MAGIC) ||
        (HealthHw_BackupRead(HEALTH_BACKUP_INDEX_VERSION) != HEALTH_BACKUP_VERSION)) {
        return 0U;
    }

    reason = HealthHw_BackupRead(HEALTH_BACKUP_INDEX_REASON);
    source = HealthHw_BackupRead(HEALTH_BACKUP_INDEX_SOURCE);
    detail = HealthHw_BackupRead(HEALTH_BACKUP_INDEX_DETAIL);
    occurred_at_ms = HealthHw_BackupRead(HEALTH_BACKUP_INDEX_OCCURRED_AT);
    reset_cause = HealthHw_BackupRead(HEALTH_BACKUP_INDEX_RESET_CAUSE);
    expected_checksum = HealthHw_RecordChecksum(reason, source, detail, occurred_at_ms);

    if ((reason <= (uint32_t)HEALTH_FAULT_NONE) || (reason >= (uint32_t)HEALTH_FAULT_COUNT) ||
        (source >= (uint32_t)APP_TASK_COUNT) ||
        (HealthHw_BackupRead(HEALTH_BACKUP_INDEX_CHECKSUM) != expected_checksum)) {
        return 0U;
    }

    record->fault.reason = (health_fault_reason_t)reason;
    record->fault.source_task = (app_task_id_t)source;
    record->fault.occurred_at_ms = occurred_at_ms;
    record->fault.detail = detail;
    switch ((health_fault_reason_t)reason) {
        case HEALTH_FAULT_QUEUE_OVERFLOW:
            record->fault.error_code = (uint8_t)UART_ERROR_BUSY;
            record->fault.watchdog_blocking = 0U;
            break;
        case HEALTH_FAULT_UART_RX_TIMEOUT:
        case HEALTH_FAULT_UART_TX_TIMEOUT:
            record->fault.error_code = (uint8_t)UART_ERROR_TIMEOUT;
            record->fault.watchdog_blocking = 0U;
            break;
        case HEALTH_FAULT_TASK_STALLED:
        case HEALTH_FAULT_STACK_LOW:
        case HEALTH_FAULT_INTERNAL_ERROR:
        default:
            record->fault.error_code = (uint8_t)UART_ERROR_INTERNAL;
            record->fault.watchdog_blocking = 1U;
            break;
    }
    if (reset_cause > (uint32_t)HEALTH_RESET_WATCHDOG) {
        reset_cause = (uint32_t)HEALTH_RESET_UNKNOWN;
    }
    record->reset_cause = (health_reset_cause_t)reset_cause;
    record->valid = 1U;
    return 1U;
}

uint32_t HealthHw_GetStackHighWaterMark(app_task_id_t task) {
    const osThreadId_t handle = HealthHw_TaskHandle(task);

    if (handle == NULL) {
        return HEALTH_STACK_WATERMARK_UNKNOWN;
    }

    return (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)handle);
}
