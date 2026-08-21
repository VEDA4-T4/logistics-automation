#ifndef UNLOAD_HW_H
#define UNLOAD_HW_H

#include <stdint.h>

#include "unload_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t UnloadHw_Init(void);
uint8_t UnloadHw_Apply(unload_servo_output_t output);
void UnloadHw_SetSafetyInhibit(uint8_t inhibited);
uint8_t UnloadHw_IsSafetyInhibited(void);
uint32_t UnloadHw_GetSafetyInhibitGeneration(void);
uint8_t UnloadHw_ReleaseSafetyInhibit(uint32_t expected_generation);

#ifdef __cplusplus
}
#endif

#endif /* UNLOAD_HW_H */
