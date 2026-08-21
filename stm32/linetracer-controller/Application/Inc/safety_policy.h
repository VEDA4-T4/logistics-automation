#ifndef SAFETY_POLICY_H
#define SAFETY_POLICY_H

#include <stdint.h>

#include "app_messages.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t SafetyPolicy_LineLossApplies(const app_control_snapshot_t* snapshot);
uint8_t SafetyPolicy_IsMomentaryRemoteEstop(const app_safety_event_t* event);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_POLICY_H */
