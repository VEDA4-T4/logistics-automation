#ifndef HEALTH_HW_H
#define HEALTH_HW_H

#include <stdint.h>

#include "health_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEALTH_RESET_UNKNOWN = 0,
    HEALTH_RESET_POWER_ON,
    HEALTH_RESET_PIN,
    HEALTH_RESET_BROWNOUT,
    HEALTH_RESET_SOFTWARE,
    HEALTH_RESET_WATCHDOG
} health_reset_cause_t;

typedef struct {
    health_fault_record_t fault;
    health_reset_cause_t reset_cause;
    uint8_t valid;
} health_persisted_record_t;

void HealthHw_StartWatchdog(uint32_t prescaler_reg, uint32_t reload);
void HealthHw_RefreshWatchdog(void);

health_reset_cause_t HealthHw_CaptureResetCause(void);
void HealthHw_StoreFault(const health_fault_record_t* fault);
uint8_t HealthHw_LoadPersistedRecord(health_persisted_record_t* record);

uint32_t HealthHw_GetStackHighWaterMark(app_task_id_t task);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_HW_H */
