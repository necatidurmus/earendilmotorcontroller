/* ============================================================
 * App/Src/safety_watchdog.c
 * Command watchdog and host-disconnect detection.
 * Extracted from app_main.c — behaviour must be identical.
 * ============================================================ */
#include "safety_watchdog.h"
#include "app_state.h"
#include "app_config.h"
#include "motion_control.h"
#include "fault_manager.h"
#include "uart_protocol.h"
#include "stm32f4xx_hal.h"

void SafetyWatchdog_Service(void)
{
    AppState *s = AppState_Get();

    if (s->phase != PHASE_RUNNING && s->phase != PHASE_NEUTRAL) {
        return;
    }

    uint32_t now = HAL_GetTick();
    if (s->last_motor_cmd_ms == 0U) return;
    if ((now - s->last_motor_cmd_ms) > CMD_WATCHDOG_MS) {
        MotionControl_StopImmediate();
        FaultManager_Raise(FAULT_WATCHDOG);
        UartProtocol_Print("\r\n[WARN] WD stop");
        s->last_motor_cmd_ms = 0U;
        return;
    }
    if (s->phase == PHASE_RUNNING &&
        !UartProtocol_HasRecentActivity(now, HOST_DISCONNECT_TIMEOUT_MS)) {
        MotionControl_StopImmediate();
        FaultManager_Raise(FAULT_HOST_LOST);
        UartProtocol_Print("\r\n[WARN] Host disconnect stop");
        s->last_motor_cmd_ms = 0U;
    }
}
