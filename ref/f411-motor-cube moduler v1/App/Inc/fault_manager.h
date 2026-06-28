/* ============================================================
 * App/Inc/fault_manager.h
 * Centralised fault state and motor-protection logic.
 * ============================================================ */
#ifndef FAULT_MANAGER_H
#define FAULT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FAULT_NONE = 0,
    FAULT_NO_HALL = 1,
    FAULT_INVALID_HALL = 2,
    FAULT_ILLEGAL_TRANSITION = 3,
    FAULT_HOST_LOST = 4,
    FAULT_WATCHDOG = 5,
    FAULT_HW_BREAK = 6,
    FAULT_ESTOP = 7,
    FAULT_OVERCURRENT = 8,
    FAULT_OVERVOLTAGE = 9,
    FAULT_UNDERVOLTAGE = 10,
    FAULT_OVERTEMP = 11,
    FAULT_GATE_DRIVER = 12,
    FAULT_UART_RX_OVERFLOW = 13
} FaultCode;

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
