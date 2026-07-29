#ifndef SAFETY_POLICY_H
#define SAFETY_POLICY_H

#include <stdint.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_LINE_RECOVERY_STABLE_MS 200U
#define SAFETY_LINE_AUTO_RECOVERY_WINDOW_MS 3000U
#define SAFETY_SENSOR_SNAPSHOT_MAX_AGE_MS 100U

uint8_t SafetyPolicy_LineLossApplies(const app_control_snapshot_t* snapshot);
uint8_t SafetyPolicy_IsMomentaryRemoteEstop(const app_safety_event_t* event);
uint8_t SafetyPolicy_SensorSupportsRecovery(const app_sensor_snapshot_t* snapshot, uint32_t now_ms);
uint8_t SafetyPolicy_ControlSupportsRecovery(const app_control_snapshot_t* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_POLICY_H */
