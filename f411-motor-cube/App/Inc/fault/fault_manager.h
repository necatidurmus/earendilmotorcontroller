/* ============================================================
 * App/Inc/fault_manager.h
 * Centralised fault state and motor-protection logic.
 * ============================================================ */
#ifndef FAULT_MANAGER_H
#define FAULT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "fault_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

void FaultManager_Init(void);

/* Returns true and applies safety (gates off) if the state warrants it.
 * Caller may also inspect the code via FaultManager_GetLast(). */
bool FaultManager_Tick(uint32_t nowMs);

void FaultManager_Raise(FaultCode code);
void FaultManager_Clear(void);
FaultCode FaultManager_GetLast(void);
const char *FaultManager_GetName(FaultCode code);

/* Returns the timestamp of the most recent fault raise (0 if never). */
uint32_t FaultManager_GetLastTimeMs(void);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_MANAGER_H */
