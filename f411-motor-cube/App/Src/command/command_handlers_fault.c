/* ============================================================
 * App/Src/command/command_handlers_fault.c
 * Fault command handlers: clrerr.
 * ============================================================ */
#include "command_handlers_fault.h"
#include "app_state.h"
#include "fault_manager.h"
#include "hall_sensor.h"
#include "motor_driver.h"
#include "speed_pi.h"
#include "service_task.h"
#include "uart_protocol.h"

#include <string.h>

bool CommandHandlers_Fault_Handle(char *cmd)
{
    AppState *s = AppState_Get();

    if (strcmp(cmd, "clrerr") == 0) {
        FaultCode prev_fault = FaultManager_GetLast();
        UartProtocol_Print("\r\n[WARN] clrerr clears displayed fault and forces STOPPED");
        UartProtocol_Print("\r\n[WARN] No current sense — battery operation is UNSAFE");
        if (prev_fault != FAULT_NONE) {
            UartProtocol_Printf("\r\n[WARN] Clearing fault: %s",
                FaultManager_GetName(prev_fault));
        }
        FaultManager_Clear();
        HallSensor_ClearFault();
        s->queue_overflow = false;
        UartProtocol_ResetTxDropCount();
        UartProtocol_ResetCmdDropCount();
        UartProtocol_ResetRxDropCount();
        UartProtocol_ResetRxByteCount();
        UartProtocol_ResetEmergencyPreemptCount();
        UartProtocol_ResetUartErrorCount();
        s->phase = PHASE_STOPPED;
        s->direction = (Direction)0;
        s->target_duty = 0U;
        s->current_duty = 0U;
        s->run_request = false;
        s->stop_request = false;
        s->duty_update_request = false;
        s->last_motor_cmd_ms = 0U;
        s->has_ever_run = false;
        s->last_edge_count = HallSensor_GetEdgeCounter();
        s->last_edge_ms = 0U;
        s->kick_active = false;
        s->kick_start_ms = 0U;
        s->ramp_current_duty = 0U;
        s->last_ramp_update_ms = 0U;
        s->gatetest_active = false;
        s->reversal_waiting = false;
        s->reversal_pending_dir = 0;
        if (ServiceTask_IsActive()) ServiceTask_Cancel();
        MotorDriver_AllOff();
        MotorDriver_SetSafetyLock(false);
        SpeedPI_Disable();
        UartProtocol_Print("\r\n[OK] Errors cleared, motor stopped, safety unlocked");
        return true;
    }

    return false;
}
