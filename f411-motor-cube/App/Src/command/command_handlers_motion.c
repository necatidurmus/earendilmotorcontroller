/* ============================================================
 * App/Src/command/command_handlers_motion.c
 * Motion command handlers: f, b, f<n>, b<n>, stop, brake,
 * estop, safe, alloff, pwm, mode, rpm.
 * ============================================================ */
#include "command_handlers_motion.h"
#include "app_state.h"
#include "app_types.h"
#include "app_config.h"
#include "app_utils.h"
#include "motion_control.h"
#include "motor_driver.h"
#include "speed_pi.h"
#include "hall_sensor.h"
#include "uart_protocol.h"
#include "fault_manager.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdlib.h>

static void clear_fault_and_unlock(void)
{
    FaultManager_Clear();
    MotorDriver_SetSafetyLock(false);
}

bool CommandHandlers_Motion_Handle(char *cmd)
{
    AppState *s = AppState_Get();
    long v;
    bool ok;

    /* --- forward / backward (default duty) --- */
    if (strcmp(cmd, "f") == 0 || strcmp(cmd, "forward") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        clear_fault_and_unlock();
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            MotionControl_BeginNeutralSwitch(+1);
            UartProtocol_Print("\r\n[OK] FWD (neutral switch)");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD &&
            s->target_duty == s->default_pwm) {
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            s->target_duty = s->default_pwm;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = s->default_pwm;
                MotorDriver_SetDuty(s->default_pwm);
            }
            UartProtocol_Printf("\r\n[OK] FWD duty=%u", (unsigned)s->default_pwm);
            return true;
        }
        MotionControl_RequestRun(DIR_FWD, s->default_pwm);
        UartProtocol_Printf("\r\n[OK] Run FWD D=%u", (unsigned)s->default_pwm);
        return true;
    }
    if (strcmp(cmd, "b") == 0 || strcmp(cmd, "backward") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        clear_fault_and_unlock();
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            MotionControl_BeginNeutralSwitch(-1);
            UartProtocol_Print("\r\n[OK] REV (neutral switch)");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV &&
            s->target_duty == s->default_pwm) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            s->target_duty = s->default_pwm;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = s->default_pwm;
                MotorDriver_SetDuty(s->default_pwm);
            }
            UartProtocol_Printf("\r\n[OK] REV duty=%u", (unsigned)s->default_pwm);
            return true;
        }
        MotionControl_RequestRun(DIR_REV, s->default_pwm);
        UartProtocol_Printf("\r\n[OK] Run REV D=%u", (unsigned)s->default_pwm);
        return true;
    }

    /* --- f<n> / b<n> --- */
    if (cmd[0] == 'f' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        clear_fault_and_unlock();
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return true; }
        if (d < 0)   d = 0;
        if (d > PWM_MAX_DUTY) d = PWM_MAX_DUTY;
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            s->target_duty = (uint16_t)d;
            MotionControl_BeginNeutralSwitch(+1);
            UartProtocol_Printf("\r\n[OK] FWD D=%lu (neutral switch)", (unsigned long)d);
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD &&
            s->target_duty == (uint16_t)d) {
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            s->target_duty = (uint16_t)d;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = (uint16_t)d;
                MotorDriver_SetDuty((uint16_t)d);
            }
            UartProtocol_Printf("\r\n[OK] FWD duty=%lu", (unsigned long)d);
            return true;
        }
        MotionControl_RequestRun(DIR_FWD, (uint16_t)d);
        UartProtocol_Printf("\r\n[OK] FWD D=%lu", (unsigned long)d);
        return true;
    }
    if (cmd[0] == 'b' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        clear_fault_and_unlock();
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return true; }
        if (d < 0)   d = 0;
        if (d > PWM_MAX_DUTY) d = PWM_MAX_DUTY;
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            s->target_duty = (uint16_t)d;
            MotionControl_BeginNeutralSwitch(-1);
            UartProtocol_Printf("\r\n[OK] REV D=%lu (neutral switch)", (unsigned long)d);
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV &&
            s->target_duty == (uint16_t)d) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return true;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            s->target_duty = (uint16_t)d;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = (uint16_t)d;
                MotorDriver_SetDuty((uint16_t)d);
            }
            UartProtocol_Printf("\r\n[OK] REV duty=%lu", (unsigned long)d);
            return true;
        }
        MotionControl_RequestRun(DIR_REV, (uint16_t)d);
        UartProtocol_Printf("\r\n[OK] REV D=%lu", (unsigned long)d);
        return true;
    }

    /* --- stop / brake / estop / safe --- */
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
        MotionControl_StopImmediate();
        clear_fault_and_unlock();
        UartProtocol_Print("\r\n[OK] Stop");
        return true;
    }
    if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
        clear_fault_and_unlock();
        MotionControl_RequestBrake();
        if (MotorDriver_IsSafetyLocked()) {
            /* Safety lock still set means a fresh hard fault prevented
             * active brake; MotorDriver_ActiveBrake() degraded to coast. */
            UartProtocol_Print("\r\n[WARN] Brake (coast — safety locked)");
        } else {
            UartProtocol_Print("\r\n[OK] Active brake");
        }
        return true;
    }
    if (strcmp(cmd, "estop") == 0) {
        FaultManager_Raise(FAULT_ESTOP);
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[OK] EMERGENCY STOP (motor stopped, clears on next command)");
        return true;
    }
    if (strcmp(cmd, "safe") == 0 || strcmp(cmd, "alloff") == 0) {
        MotionControl_StopImmediate();
        clear_fault_and_unlock();
        UartProtocol_Print("\r\n[OK] Safe stop (all off)");
        return true;
    }

    /* --- pwm query/set --- */
    if (strcmp(cmd, "pwm") == 0) {
        UartProtocol_Printf("\r\n[INFO] TargetDuty=%u CurrentDuty=%u",
                            (unsigned)s->target_duty, (unsigned)s->current_duty);
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "pwm ", &ok);
    if (ok) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        clear_fault_and_unlock();
        if (v < 0)   v = 0;
        if (v > PWM_MAX_DUTY) v = PWM_MAX_DUTY;
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->target_duty = (uint16_t)v;
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            s->duty_update_request = true;
            s->last_motor_cmd_ms = HAL_GetTick();
        }
        UartProtocol_Printf("\r\n[OK] TargetDuty=%lu", (unsigned long)v);
        return true;
    }

    /* --- mode duty / mode speed (with legacy aliases) --- */
    if (strcmp(cmd, "mode") == 0) {
        UartProtocol_Printf("\r\n[INFO] Mode=%s",
                            s->mode == MODE_SPEED ? "SPEED" : "DUTY");
        return true;
    }
    if (strcmp(cmd, "mode duty") == 0 || strcmp(cmd, "pid off") == 0 ||
        strcmp(cmd, "mode normal") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        SpeedPI_Disable();
        s->mode = MODE_DUTY;
        UartProtocol_Print("\r\n[OK] Mode=DUTY");
        return true;
    }
    if (strcmp(cmd, "mode speed") == 0 || strcmp(cmd, "pid on") == 0 ||
        strcmp(cmd, "mode control") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        SpeedPI_Enable();
        s->mode = MODE_SPEED;
        UartProtocol_Print("\r\n[OK] Mode=SPEED");
        return true;
    }

    /* --- rpm --- */
    if (strcmp(cmd, "rpm") == 0) {
        UartProtocol_Printf("\r\n[INFO] TargetRPM_Cmd=%ld Ramped=%d Measured=%lu",
            (long)SpeedPI_GetRawTargetRpm(),
            (int)SpeedPI_GetRampedTargetRpm(),
            (unsigned long)HallSensor_CalculateRpm());
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "rpm ", &ok);
    if (ok) {
        if (v != 0 && MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        if (v == 0) {
            if (SpeedPI_IsEnabled()) SpeedPI_Disable();
            MotionControl_StopImmediate();
            clear_fault_and_unlock();
            UartProtocol_Print("\r\n[OK] RPM=0 stop");
            return true;
        }
        clear_fault_and_unlock();
        if (v > MAX_RPM_TARGET)  v = MAX_RPM_TARGET;
        if (v < -MAX_RPM_TARGET) v = -MAX_RPM_TARGET;

        int8_t new_dir = (v > 0) ? +1 : -1;
        int8_t old_dir = (int8_t)s->direction;

        if (!SpeedPI_IsEnabled()) {
            SpeedPI_Enable();
            s->mode = MODE_SPEED;
        }
        s->last_motor_cmd_ms = HAL_GetTick();

        if (s->phase == PHASE_RUNNING && old_dir != 0 && new_dir != old_dir) {
            SpeedPI_SetTargetRpm((int32_t)v);
            MotionControl_BeginNeutralSwitch(new_dir);
        } else if (s->phase == PHASE_RUNNING && new_dir == old_dir) {
            s->direction = (v > 0) ? DIR_FWD : DIR_REV;
            SpeedPI_SetTargetRpm((int32_t)v);
        } else {
            s->direction = (v > 0) ? DIR_FWD : DIR_REV;
            s->run_request = true;
            s->stop_request = false;
            SpeedPI_SetTargetRpm((int32_t)v);
        }
        UartProtocol_Printf("\r\n[OK] RPM=%ld", (long)v);
        return true;
    }

    return false;
}
