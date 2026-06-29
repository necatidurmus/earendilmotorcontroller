/* ============================================================
 * App/Src/command/command_handlers_service.c
 * Service command handlers: arm, disarm, identify, scan,
 * test, gatetest.
 * ============================================================ */
#include "command_handlers_service.h"
#include "app_state.h"
#include "app_config.h"
#include "app_utils.h"
#include "service_task.h"
#include "gate_test.h"
#include "motor_driver.h"
#include "motion_control.h"
#include "fault_manager.h"
#include "uart_protocol.h"
#include "stm32f4xx_hal.h"

#include "stm32f4xx_hal_gpio.h"
#include <string.h>
#include <stdlib.h>

bool CommandHandlers_Service_Handle(char *cmd)
{
    AppState *s = AppState_Get();

    /* --- identify --- */
    if (strcmp(cmd, "identify") == 0) {
        if (!s->service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return true;
        }
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        FaultManager_Clear();
        MotorDriver_SetSafetyLock(false);
        MotorDriver_AllOff();
        MotionControl_StopImmediate();
        s->identify_was_run = true;
        s->identify_last_result = 0U;
        ServiceTask_Request(SVC_IDENTIFY);
        return true;
    }

    /* --- scan --- */
    if (strcmp(cmd, "scan") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT && s->phase != PHASE_BRAKE) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        ServiceTask_Request(SVC_SCAN);
        return true;
    }

    /* --- test --- */
    if (strcmp(cmd, "test") == 0) {
        if (!s->service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return true;
        }
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT && s->phase != PHASE_BRAKE) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        ServiceTask_Request(SVC_TEST);
        return true;
    }

    /* Direct GPIO/TIM1 debug commands were intentionally removed.
     * Gate outputs must only be exercised through the armed, timed
     * gatetest/service paths below. */

    /* --- gatetest <sector> <duty> --- */
    if (AppUtils_StartsWith(cmd, "gatetest ")) {
        if (!s->gate_test_armed) {
            UartProtocol_Print("\r\n[ERR] Gate test not armed. Use: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
            return true;
        }
        FaultManager_Clear();
        MotorDriver_SetSafetyLock(false);
        MotorDriver_AllOff();
        MotionControl_StopImmediate();
        ServiceTask_Cancel();
        const char *p = cmd + 9;
        while (*p == ' ') p++;
        char *ge1 = NULL;
        long sector = strtol(p, &ge1, 10);
        if (ge1 == p) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>"); return true; }
        while (*ge1 == ' ') ge1++;
        char *ge2 = NULL;
        long duty = strtol(ge1, &ge2, 10);
        if (ge2 == ge1) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>"); return true; }
        if (sector < 0 || sector > 5 || duty < 1 || duty > PWM_MAX_DUTY) {
            UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>");
            return true;
        }
        s->gatetest_active   = true;
        s->gatetest_sector   = (uint8_t)sector;
        s->gatetest_duty     = (uint16_t)duty;
        s->gatetest_start_ms = HAL_GetTick();
        s->gatetest_timeout_ms = 100U;
        MotorDriver_ApplyStep((uint8_t)sector, +1, (uint16_t)duty);
        UartProtocol_Printf("\r\n[OK] Gate test sector=%lu duty=%lu timeout=100ms",
                            (unsigned long)sector, (unsigned long)duty);
        return true;
    }

    /* --- arm gatetest --- */
    if (AppUtils_StartsWith(cmd, "arm gatetest ")) {
        const char *token = cmd + 13;
        while (*token == ' ') token++;
        if (strcmp(token, "motor_disconnected_i_understand") == 0) {
            s->gate_test_armed = true;
            s->gate_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Gate test armed for 30s. Motor must be disconnected!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
        }
        return true;
    }

    /* --- arm service --- */
    if (AppUtils_StartsWith(cmd, "arm service ")) {
        const char *token = cmd + 12;
        while (*token == ' ') token++;
        if (strcmp(token, "current_limited_bench_supply") == 0) {
            s->service_armed = true;
            s->service_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Service armed for 30s. Use current-limited PSU!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm service CURRENT_LIMITED_BENCH_SUPPLY");
        }
        return true;
    }

    /* --- disarm --- */
    if (strcmp(cmd, "disarm gatetest") == 0) {
        s->gate_test_armed = false;
        UartProtocol_Print("\r\n[OK] Gate test disarmed");
        return true;
    }
    if (strcmp(cmd, "disarm service") == 0) {
        s->service_armed = false;
        UartProtocol_Print("\r\n[OK] Service disarmed");
        return true;
    }

    return false;
}
