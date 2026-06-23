/* ============================================================
 * App/Src/fault_manager.c
 * Simple fault state + latched "raise" tracking.
 * ============================================================ */

#include "fault_manager.h"
#include "motor_driver.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>

static volatile FaultCode s_last        = FAULT_NONE;
static volatile uint32_t  s_last_time_ms = 0U;

void FaultManager_Init(void)
{
    s_last         = FAULT_NONE;
    s_last_time_ms = 0U;
}

bool FaultManager_Tick(uint32_t nowMs)
{
    (void)nowMs;
    /* Nothing automatic here for now — faults are handled in the
     * command parser and the speed controller.  Reserved for future
     * use. */
    return false;
}

void FaultManager_Raise(FaultCode code)
{
    if (code == FAULT_NONE) return;
    s_last         = code;
    s_last_time_ms = HAL_GetTick();
    MotorDriver_FaultOff();
}

void FaultManager_Clear(void)
{
    s_last         = FAULT_NONE;
    s_last_time_ms = 0U;
}

FaultCode FaultManager_GetLast(void)         { return s_last; }
uint32_t   FaultManager_GetLastTimeMs(void)  { return s_last_time_ms; }

const char *FaultManager_GetName(FaultCode code)
{
    switch (code) {
    case FAULT_NONE:              return "NONE";
    case FAULT_NO_HALL:           return "NO_HALL";
    case FAULT_INVALID_HALL:      return "INVALID_HALL";
    case FAULT_ILLEGAL_TRANSITION:return "ILLEGAL_TRANS";
    case FAULT_HOST_LOST:         return "HOST_LOST";
    case FAULT_WATCHDOG:          return "WATCHDOG";
    case FAULT_HW_BREAK:          return "HW_BREAK";
    }
    return "?";
}
