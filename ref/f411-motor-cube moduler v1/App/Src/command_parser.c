/* ============================================================
 * App/Src/command_parser.c
 * UART command parser and dispatch.
 * Extracted from app_main.c — all response strings and
 * behaviour must be identical.
 * ============================================================ */
#include "command_parser.h"
#include "app_state.h"
#include "app_types.h"
#include "app_config.h"
#include "app_utils.h"
#include "app_status.h"
#include "motion_control.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "uart_protocol.h"
#include "telemetry.h"
#include "fault_manager.h"
#include "service_task.h"
#include "storage.h"
#include "bldc_commutation.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

void CommandParser_Handle(char *cmd, UartSource src)
{
    AppState *s = AppState_Get();
    (void)src;
    AppUtils_TrimInPlace(cmd);
    AppUtils_LowerInPlace(cmd);
    if (cmd[0] == '\0') return;

    /* --- Motion commands (forward / backward / stop / pwm / f<n> / b<n>) --- */
    if (strcmp(cmd, "f") == 0 || strcmp(cmd, "forward") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            MotionControl_BeginNeutralSwitch(+1);
            UartProtocol_Print("\r\n[OK] FWD (neutral switch)");
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD &&
            s->target_duty == s->default_pwm) {
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return;
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
            return;
        }
        MotionControl_RequestRun(DIR_FWD, s->default_pwm);
        UartProtocol_Printf("\r\n[OK] Run FWD D=%u", (unsigned)s->default_pwm);
        return;
    }
    if (strcmp(cmd, "b") == 0 || strcmp(cmd, "backward") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            MotionControl_BeginNeutralSwitch(-1);
            UartProtocol_Print("\r\n[OK] REV (neutral switch)");
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV &&
            s->target_duty == s->default_pwm) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return;
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
            return;
        }
        MotionControl_RequestRun(DIR_REV, s->default_pwm);
        UartProtocol_Printf("\r\n[OK] Run REV D=%u", (unsigned)s->default_pwm);
        return;
    }
    if (cmd[0] == 'f' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return; }
        if (d < 0)   d = 0;
        if (d > 250) d = 250;
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            s->target_duty = (uint8_t)d;
            MotionControl_BeginNeutralSwitch(+1);
            UartProtocol_Printf("\r\n[OK] FWD D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD &&
            s->target_duty == (uint8_t)d) {
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            s->target_duty = (uint8_t)d;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = (uint8_t)d;
                MotorDriver_SetDuty((uint8_t)d);
            }
            UartProtocol_Printf("\r\n[OK] FWD duty=%lu", (unsigned long)d);
            return;
        }
        MotionControl_RequestRun(DIR_FWD, (uint8_t)d);
        UartProtocol_Printf("\r\n[OK] FWD D=%lu", (unsigned long)d);
        return;
    }
    if (cmd[0] == 'b' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return; }
        if (d < 0)   d = 0;
        if (d > 250) d = 250;
        s->mode = MODE_DUTY;
        s->last_motor_cmd_ms = HAL_GetTick();
        if (s->phase == PHASE_RUNNING && s->direction == DIR_FWD) {
            s->target_duty = (uint8_t)d;
            MotionControl_BeginNeutralSwitch(-1);
            UartProtocol_Printf("\r\n[OK] REV D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV &&
            s->target_duty == (uint8_t)d) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return;
        }
        if (s->phase == PHASE_RUNNING && s->direction == DIR_REV) {
            s->target_duty = (uint8_t)d;
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->current_duty = (uint8_t)d;
                MotorDriver_SetDuty((uint8_t)d);
            }
            UartProtocol_Printf("\r\n[OK] REV duty=%lu", (unsigned long)d);
            return;
        }
        MotionControl_RequestRun(DIR_REV, (uint8_t)d);
        UartProtocol_Printf("\r\n[OK] REV D=%lu", (unsigned long)d);
        return;
    }
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[OK] Stop");
        return;
    }
    if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[OK] Brake (coast in first bring-up)");
        return;
    }
    if (strcmp(cmd, "estop") == 0) {
        FaultManager_Raise(FAULT_ESTOP);
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[OK] EMERGENCY STOP (fault latched, clrerr required)");
        return;
    }
    if (strcmp(cmd, "safe") == 0 || strcmp(cmd, "alloff") == 0) {
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[OK] Safe stop (all off)");
        return;
    }

    /* --- pwm query/set --- */
    if (strcmp(cmd, "pwm") == 0) {
        UartProtocol_Printf("\r\n[INFO] TargetDuty=%u CurrentDuty=%u",
                            (unsigned)s->target_duty, (unsigned)s->current_duty);
        return;
    }
    bool ok = false;
    long v = AppUtils_ParseLongAfter(cmd, "pwm ", &ok);
    if (ok) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (v < 0)   v = 0;
        if (v > 250) v = 250;
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s->mode = MODE_DUTY;
        s->target_duty = (uint8_t)v;
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            s->duty_update_request = true;
            s->last_motor_cmd_ms = HAL_GetTick();
        }
        UartProtocol_Printf("\r\n[OK] TargetDuty=%lu", (unsigned long)v);
        return;
    }

    /* --- mode duty / mode speed (with legacy aliases) --- */
    if (strcmp(cmd, "mode") == 0) {
        UartProtocol_Printf("\r\n[INFO] Mode=%s",
                            s->mode == MODE_SPEED ? "SPEED" : "DUTY");
        return;
    }
    if (strcmp(cmd, "mode duty") == 0 || strcmp(cmd, "pid off") == 0 ||
        strcmp(cmd, "mode normal") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        SpeedPI_Disable();
        s->mode = MODE_DUTY;
        UartProtocol_Print("\r\n[OK] Mode=DUTY");
        return;
    }
    if (strcmp(cmd, "mode speed") == 0 || strcmp(cmd, "pid on") == 0 ||
        strcmp(cmd, "mode control") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        SpeedPI_Enable();
        s->mode = MODE_SPEED;
        UartProtocol_Print("\r\n[OK] Mode=SPEED");
        return;
    }

    /* --- rpm --- */
    if (strcmp(cmd, "rpm") == 0) {
        UartProtocol_Printf("\r\n[INFO] TargetRPM_Cmd=%ld Ramped=%d Measured=%lu",
            (long)SpeedPI_GetRawTargetRpm(),
            (int)SpeedPI_GetRampedTargetRpm(),
            (unsigned long)HallSensor_CalculateRpm());
        return;
    }
    v = AppUtils_ParseLongAfter(cmd, "rpm ", &ok);
    if (ok) {
        if (v != 0 && MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (v == 0) {
            if (SpeedPI_IsEnabled()) SpeedPI_Disable();
            MotionControl_StopImmediate();
            UartProtocol_Print("\r\n[OK] RPM=0 stop");
            return;
        }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
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
            MotionControl_BeginNeutralSwitch(new_dir);
            SpeedPI_SetTargetRpm((int32_t)v);
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
        return;
    }

    /* --- pi <kp> <ki> --- */
    if (AppUtils_StartsWith(cmd, "pi ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        const char *p = cmd + 3;
        while (*p == ' ') p++;
        char *end1 = NULL;
        float kp = strtof(p, &end1);
        if (end1 == p) { UartProtocol_Print("\r\n[ERR] Usage: pi <kp> <ki>"); return; }
        if (!isfinite(kp)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return; }
        while (*end1 == ' ') end1++;
        char *end2 = NULL;
        float ki = strtof(end1, &end2);
        if (end2 == end1) { UartProtocol_Print("\r\n[ERR] Usage: pi <kp> <ki>"); return; }
        if (!isfinite(ki)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return; }
        while (*end2 == ' ') end2++;
        if (*end2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        SpeedPI_SetKp(kp);
        SpeedPI_SetKi(ki);
        UartProtocol_Printf("\r\n[OK] Kp_m=%ld Ki_m=%ld",
            (long)(kp * 1000.0f), (long)(ki * 1000.0f));
        return;
    }

    /* --- kp / ki standalone (compat) --- */
    float fv = AppUtils_ParseFloatAfter(cmd, "kp ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        SpeedPI_SetKp(fv); UartProtocol_Printf("\r\n[OK] Kp_m=%ld", (long)(fv * 1000.0f)); return;
    }
    fv = AppUtils_ParseFloatAfter(cmd, "ki ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        SpeedPI_SetKi(fv); UartProtocol_Printf("\r\n[OK] Ki_m=%ld", (long)(fv * 1000.0f)); return;
    }

    /* --- base <lo> <mid> <hi> --- */
    if (AppUtils_StartsWith(cmd, "base ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        const char *p = cmd + 5;
        while (*p == ' ') p++;
        char *e1 = NULL, *e2 = NULL, *e3 = NULL;
        long lo  = strtol(p, &e1, 10);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return; }
        while (*e1 == ' ') e1++;
        long mid = strtol(e1, &e2, 10);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return; }
        while (*e2 == ' ') e2++;
        long hi  = strtol(e2, &e3, 10);
        if (e3 == e2) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return; }
        while (*e3 == ' ') e3++;
        if (*e3 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        if (lo < 0) lo = 0;
        if (lo > 250) lo = 250;
        if (mid < 0) mid = 0;
        if (mid > 250) mid = 250;
        if (hi < 0) hi = 0;
        if (hi > 250) hi = 250;
        SpeedPI_SetBasePwm((uint8_t)lo, (uint8_t)mid, (uint8_t)hi);
        uint8_t alo, amid, ahi;
        SpeedPI_GetBasePwm(&alo, &amid, &ahi);
        UartProtocol_Printf("\r\n[OK] Base L=%u M=%u H=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi);
        return;
    }

    /* --- boost <lo> <mid> <hi> <ms> --- */
    if (AppUtils_StartsWith(cmd, "boost ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        const char *p = cmd + 6;
        while (*p == ' ') p++;
        char *e1 = NULL, *e2 = NULL, *e3 = NULL, *e4 = NULL;
        long lo  = strtol(p, &e1, 10);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return; }
        while (*e1 == ' ') e1++;
        long mid = strtol(e1, &e2, 10);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return; }
        while (*e2 == ' ') e2++;
        long hi  = strtol(e2, &e3, 10);
        if (e3 == e2) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return; }
        while (*e3 == ' ') e3++;
        long ms  = strtol(e3, &e4, 10);
        if (e4 == e3) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return; }
        while (*e4 == ' ') e4++;
        if (*e4 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        if (lo < 0) lo = 0;
        if (lo > 250) lo = 250;
        if (mid < 0) mid = 0;
        if (mid > 250) mid = 250;
        if (hi < 0) hi = 0;
        if (hi > 250) hi = 250;
        if (ms < 0) ms = 0;
        if (ms > 1000) ms = 1000;
        SpeedPI_SetBoostPwm((uint8_t)lo, (uint8_t)mid, (uint8_t)hi, (uint16_t)ms);
        uint8_t alo, amid, ahi;
        uint16_t ams;
        SpeedPI_GetBoostPwm(&alo, &amid, &ahi, &ams);
        UartProtocol_Printf("\r\n[OK] Boost L=%u M=%u H=%u ms=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi, (unsigned)ams);
        return;
    }

    /* --- ramp <up> <down> --- */
    if (AppUtils_StartsWith(cmd, "ramp ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        const char *p = cmd + 5;
        while (*p == ' ') p++;
        char *e1 = NULL;
        float up = strtof(p, &e1);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: ramp <up_rpm_s> <down_rpm_s>"); return; }
        if (!isfinite(up)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return; }
        while (*e1 == ' ') e1++;
        char *e2 = NULL;
        float down = strtof(e1, &e2);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: ramp <up_rpm_s> <down_rpm_s>"); return; }
        if (!isfinite(down)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return; }
        while (*e2 == ' ') e2++;
        if (*e2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        SpeedPI_SetRamp(up, down);
        UartProtocol_Printf("\r\n[OK] Ramp up=%ld down=%ld", (long)up, (long)down);
        return;
    }

    /* --- spstat --- */
    if (strcmp(cmd, "spstat") == 0) {
        UartProtocol_Print("\r\n--- SPEED PI STATUS ---");
        UartProtocol_Printf("\r\nMode=%s Phase=%d Tcmd=%ld Trmp=%d F=%d",
            s->mode == MODE_SPEED ? "SPEED" : "DUTY",
            (int)SpeedPI_GetPhase(),
            (long)SpeedPI_GetRawTargetRpm(),
            (int)SpeedPI_GetRampedTargetRpm(),
            (int)HallSensor_GetFilteredRpm());
        UartProtocol_Printf("\r\nComputedDuty=%u Hall=%u",
            (unsigned)SpeedPI_GetComputedDuty(),
            (unsigned)HallSensor_GetStableRaw());
        UartProtocol_PrintNewline();
        return;
    }

    /* --- hall --- */
    if (strcmp(cmd, "hall") == 0 || strcmp(cmd, "h") == 0) {
        UartProtocol_Printf("\r\n[INFO] Hall=%u State=%u",
            (unsigned)HallSensor_GetStableRaw(),
            (unsigned)HallSensor_GetMappedState());
        return;
    }

    /* --- map subcommands --- */
    if (AppUtils_StartsWith(cmd, "map")) {
        const char *sub = cmd + 3;

        if (sub[0] == '\0') {
            uint8_t map[8];
            Commutation_GetMap(map);
            const char *src_name = "DEFAULT";
            switch (s->hall_map_source) {
            case 1: src_name = "RAM_IDENTIFY"; break;
            case 2: src_name = "RAM_MANUAL"; break;
            case 3: src_name = "FLASH"; break;
            case 4: src_name = "INVALID_FALLBACK"; break;
            default: src_name = "DEFAULT"; break;
            }
            bool valid = Commutation_ValidateHallMap(map);
            UartProtocol_Printf("\r\nHALL_MAP active valid=%u source=%s dirty=%u",
                (unsigned)valid, src_name, (unsigned)s->hall_map_dirty);
            UartProtocol_Print("\r\nraw:    0   1   2   3   4   5   6   7");
            UartProtocol_Print("\r\nstate:");
            for (uint8_t i = 0; i < 8; i++)
                UartProtocol_Printf(" %3u", (unsigned)map[i]);
            UartProtocol_PrintNewline();
            if (!valid) {
                char reason[32];
                Commutation_ValidateHallMapVerbose(map, reason, sizeof(reason));
                UartProtocol_Printf("\r\n[WARN] Active map invalid: %s", reason);
            }
            return;
        }

        if (strcmp(sub, " validate") == 0) {
            uint8_t map[8];
            Commutation_GetMap(map);
            char reason[32];
            if (Commutation_ValidateHallMapVerbose(map, reason, sizeof(reason))) {
                UartProtocol_Print("\r\n[OK] Active map valid");
            } else {
                UartProtocol_Printf("\r\n[ERR] Active map invalid: %s", reason);
            }
            return;
        }

        if (strcmp(sub, " default") == 0 || strcmp(sub, " reset") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return;
            }
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 0U;
            s->hall_map_dirty = false;
            s->candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded into RAM");
            AppStatus_PrintHallMap();
            return;
        }

        if (strcmp(sub, " edit") == 0) {
            Commutation_GetMap(s->candidate_map);
            s->candidate_active = true;
            UartProtocol_Print("\r\n[OK] Active map copied to candidate");
            return;
        }

        if (AppUtils_StartsWith(sub, " set ")) {
            const char *p = sub + 5;
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return;
            }
            char *end1 = NULL;
            long raw = strtol(p, &end1, 10);
            if (end1 == p || raw < 0 || raw > 7) {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return;
            }
            const char *q = end1;
            while (*q == ' ') q++;
            if (*q == '\0') {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return;
            }
            long sector;
            if (strcmp(q, "invalid") == 0 || strcmp(q, "255") == 0) {
                sector = 255;
            } else {
                char *end2 = NULL;
                sector = strtol(q, &end2, 10);
                if (end2 == q) {
                    UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                    return;
                }
                while (*end2 == ' ') end2++;
                if (*end2 != '\0') {
                    UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                    return;
                }
            }
            if (sector != 255 && (sector < 0 || sector > 5)) {
                UartProtocol_Print("\r\n[ERR] Sector must be 0..5 or 'invalid'");
                return;
            }
            if ((raw == 0 || raw == 7) && sector != 255) {
                UartProtocol_Print("\r\n[ERR] raw 0 and raw 7 must be 'invalid' (255)");
                return;
            }
            if (!s->candidate_active) {
                Commutation_GetMap(s->candidate_map);
                s->candidate_active = true;
            }
            s->candidate_map[(uint8_t)raw] = (uint8_t)sector;
            char reason[32];
            bool valid = Commutation_ValidateHallMapVerbose(
                s->candidate_map, reason, sizeof(reason));
            UartProtocol_Printf("\r\n[OK] candidate[%lu] = %lu  valid=%u",
                (unsigned long)raw, (unsigned long)sector, (unsigned)valid);
            if (!valid) {
                UartProtocol_Printf(" (%s)", reason);
            }
            return;
        }

        if (strcmp(sub, " candidate") == 0) {
            if (!s->candidate_active) {
                UartProtocol_Print("\r\n[INFO] No candidate map. Use 'map edit' first.");
                return;
            }
            char reason[32];
            bool valid = Commutation_ValidateHallMapVerbose(
                s->candidate_map, reason, sizeof(reason));
            UartProtocol_Printf("\r\nHALL_MAP candidate valid=%u", (unsigned)valid);
            if (!valid) UartProtocol_Printf(" reason=%s", reason);
            UartProtocol_Print("\r\nraw:    0   1   2   3   4   5   6   7");
            UartProtocol_Print("\r\nstate:");
            for (uint8_t i = 0; i < 8; i++)
                UartProtocol_Printf(" %3u", (unsigned)s->candidate_map[i]);
            UartProtocol_PrintNewline();
            return;
        }

        if (strcmp(sub, " apply") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return;
            }
            if (!s->candidate_active) {
                UartProtocol_Print("\r\n[ERR] No candidate map. Use 'map edit' first.");
                return;
            }
            char reason[32];
            if (!Commutation_ValidateHallMapVerbose(s->candidate_map, reason, sizeof(reason))) {
                UartProtocol_Printf("\r\n[ERR] Candidate map rejected: %s", reason);
                UartProtocol_Print("\r\n[SAFE] Active map unchanged");
                return;
            }
            Commutation_ApplyMap(s->candidate_map);
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 2U;
            s->hall_map_dirty = true;
            s->candidate_active = false;
            UartProtocol_Print("\r\n[OK] Candidate map applied to active RAM map");
            UartProtocol_Print("\r\n[WARN] Map is RAM-only. Use 'map save' after verification if storage is enabled.");
            AppStatus_PrintHallMap();
            return;
        }

        if (strcmp(sub, " discard") == 0) {
            s->candidate_active = false;
            memset(s->candidate_map, 255, sizeof(s->candidate_map));
            UartProtocol_Print("\r\n[OK] Candidate map discarded");
            return;
        }

        if (strcmp(sub, " load") == 0 || strcmp(sub, " reload") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return;
            }
            uint8_t map[8];
            if (Storage_LoadHallMap(map)) {
                char reason[32];
                if (!Commutation_ValidateHallMapVerbose(map, reason, sizeof(reason))) {
                    UartProtocol_Printf("\r\n[ERR] Flash map invalid: %s", reason);
                    UartProtocol_Print("\r\n[SAFE] Using default map");
                    Commutation_LoadDefaultMap();
                    HallSensor_OnMapChanged();
                    SpeedPI_Reset();
                    s->hall_map_source = 0U;
                    s->hall_map_dirty = false;
                } else {
                    Commutation_ApplyMap(map);
                    HallSensor_OnMapChanged();
                    SpeedPI_Reset();
                    s->hall_map_source = 3U;
                    s->hall_map_dirty = false;
                    UartProtocol_Print("\r\n[OK] Hall map loaded from flash");
                }
            } else {
                Commutation_LoadDefaultMap();
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s->hall_map_source = 0U;
                s->hall_map_dirty = false;
                UartProtocol_Print("\r\n[INFO] No saved map in flash, defaults loaded");
            }
            AppStatus_PrintHallMap();
            return;
        }

        if (strcmp(sub, " save") == 0) {
            UartProtocol_Print("\r\n[ERR] Persistent storage disabled in this build");
            UartProtocol_Print("\r\n[INFO] Map lives in RAM only until reset");
            return;
        }

        if (strcmp(cmd, "mapreset") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 0U;
            s->hall_map_dirty = false;
            s->candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded");
            AppStatus_PrintHallMap();
            return;
        }

        UartProtocol_Print("\r\n[ERR] Unknown map subcommand. Type 'help' for list.");
        return;
    }

    /* Legacy compat: "save" */
    if (strcmp(cmd, "save") == 0 || strcmp(cmd, "savecfg") == 0 ||
        strcmp(cmd, "saveall") == 0) {
        UartProtocol_Print("\r\n[ERR] Persistent storage disabled in this build");
        return;
    }

    /* Legacy compat: "reload" */
    if (strcmp(cmd, "reload") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        if (FaultManager_GetLast() != FAULT_NONE) {
            UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
            return;
        }
        uint8_t map[8];
        if (Storage_LoadHallMap(map)) {
            char reason[32];
            if (!Commutation_ValidateHallMapVerbose(map, reason, sizeof(reason))) {
                UartProtocol_Printf("\r\n[ERR] Flash map invalid: %s", reason);
                Commutation_LoadDefaultMap();
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s->hall_map_source = 0U;
                s->hall_map_dirty = false;
            } else {
                Commutation_ApplyMap(map);
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s->hall_map_source = 3U;
                s->hall_map_dirty = false;
                UartProtocol_Print("\r\n[OK] Hall map loaded from flash");
            }
        } else {
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 0U;
            s->hall_map_dirty = false;
            UartProtocol_Print("\r\n[INFO] No saved map, defaults loaded");
        }
        AppStatus_PrintHallMap();
        return;
    }

    /* --- identify / scan / test --- */
    if (strcmp(cmd, "identify") == 0) {
        if (!s->service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        if (MotorDriver_IsSafetyLocked()) {
            UartProtocol_Print("\r\n[ERR] Safety locked; use clrerr");
            return;
        }
        UartProtocol_Print("\r\n[WARN] identify will energize motor phases.");
        UartProtocol_Print("\r\n[WARN] Use only with current-limited bench supply.");
        UartProtocol_Print("\r\n[WARN] Wheels unloaded, low voltage, emergency stop ready.");
        s->identify_was_run = true;
        s->identify_last_result = 0U;
        ServiceTask_Request(SVC_IDENTIFY);
        return;
    }
    if (strcmp(cmd, "scan") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        ServiceTask_Request(SVC_SCAN);
        return;
    }
    if (strcmp(cmd, "test") == 0) {
        if (!s->service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        if (MotorDriver_IsSafetyLocked()) {
            UartProtocol_Print("\r\n[ERR] Safety locked; use clrerr");
            return;
        }
        ServiceTask_Request(SVC_TEST);
        return;
    }

    /* --- gatetest <sector> <duty> --- */
    if (AppUtils_StartsWith(cmd, "gatetest ")) {
        if (!s->gate_test_armed) {
            UartProtocol_Print("\r\n[ERR] Gate test not armed. Use: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
            return;
        }
        if (!MotionControl_Allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        if (ServiceTask_IsActive()) {
            UartProtocol_Print("\r\n[ERR] Service task active");
            return;
        }
        if (MotorDriver_IsSafetyLocked()) {
            UartProtocol_Print("\r\n[ERR] Safety locked; use clrerr");
            return;
        }
        const char *p = cmd + 9;
        while (*p == ' ') p++;
        char *ge1 = NULL;
        long sector = strtol(p, &ge1, 10);
        if (ge1 == p) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-10>"); return; }
        while (*ge1 == ' ') ge1++;
        char *ge2 = NULL;
        long duty = strtol(ge1, &ge2, 10);
        if (ge2 == ge1) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-10>"); return; }
        while (*ge2 == ' ') ge2++;
        if (*ge2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        if (sector < 0 || sector > 5 || duty < 1 || duty > 10) {
            UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-10> (duty limited to 10)");
            return;
        }
        UartProtocol_Print("\r\n[WARN] Motor must be disconnected!");
        s->gatetest_active   = true;
        s->gatetest_sector   = (uint8_t)sector;
        s->gatetest_duty     = (uint8_t)duty;
        s->gatetest_start_ms = HAL_GetTick();
        s->gatetest_timeout_ms = 100U;
        MotorDriver_ApplyStep((uint8_t)sector, +1, (uint8_t)duty);
        UartProtocol_Printf("\r\n[OK] Gate test sector=%lu duty=%lu timeout=100ms",
                            (unsigned long)sector, (unsigned long)duty);
        return;
    }

    /* --- savecfg / loadcfg / defaults --- */
    if (strcmp(cmd, "loadcfg") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        uint8_t kd, rs, dp;
        uint16_t km, ri, bh;
        if (Storage_LoadConfig(&kd, &km, &rs, &ri, &dp, &bh)) {
            s->kick_duty = kd; s->kick_ms = km;
            s->ramp_step = rs; s->ramp_interval_ms = ri;
            s->default_pwm = dp; s->brake_hold_ms = bh;
            MotionControl_ClampLoadedConfig();
            UartProtocol_Print("\r\n[OK] Config loaded");
        } else {
            UartProtocol_Print("\r\n[INFO] No saved config, defaults kept");
        }
        return;
    }
    if (strcmp(cmd, "defaults") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s->kick_enabled = false;
        s->ramp_enabled = true;
        s->kick_duty = 60;
        s->kick_ms = 50;
        s->ramp_step = 8;
        s->ramp_interval_ms = 5;
        s->default_pwm = 100;
        s->brake_hold_ms = BRAKE_HOLD_MS;
        UartProtocol_Print("\r\n[OK] Defaults loaded into RAM (kick OFF, ramp ON)");
        return;
    }

    /* --- kick / ramp config commands --- */
    if (strcmp(cmd, "kick on") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s->kick_enabled = true;  UartProtocol_Print("\r\n[OK] Kick ON");  return;
    }
    if (strcmp(cmd, "kick off") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s->kick_enabled = false; UartProtocol_Print("\r\n[OK] Kick OFF"); return;
    }
    if (strcmp(cmd, "ramp on") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s->ramp_enabled = true;  UartProtocol_Print("\r\n[OK] Ramp ON");  return;
    }
    if (strcmp(cmd, "ramp off") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s->ramp_enabled = false; UartProtocol_Print("\r\n[OK] Ramp OFF"); return;
    }

    v = AppUtils_ParseLongAfter(cmd, "kickduty ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > KICK_DUTY_MAX) v = KICK_DUTY_MAX;
        s->kick_duty = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] KickDuty=%lu", (unsigned long)v);
        return;
    }
    v = AppUtils_ParseLongAfter(cmd, "kickms ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > KICK_MS_MAX) v = KICK_MS_MAX;
        s->kick_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] KickMs=%lu", (unsigned long)v);
        return;
    }
    v = AppUtils_ParseLongAfter(cmd, "ramprate ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > RAMP_STEP_MAX) v = RAMP_STEP_MAX;
        s->ramp_step = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] RampStep=%lu", (unsigned long)v);
        return;
    }
    v = AppUtils_ParseLongAfter(cmd, "rampms ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > RAMP_INTERVAL_MS_MAX) v = RAMP_INTERVAL_MS_MAX;
        s->ramp_interval_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] RampMs=%lu", (unsigned long)v);
        return;
    }
    v = AppUtils_ParseLongAfter(cmd, "defpwm ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > DEFAULT_PWM_MAX) v = DEFAULT_PWM_MAX;
        s->default_pwm = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] DefaultPWM=%lu", (unsigned long)v);
        return;
    }

    /* --- status --- */
    if (strcmp(cmd, "status") == 0) { AppStatus_PrintStatus(); return; }

    /* --- clrerr --- */
    if (strcmp(cmd, "clrerr") == 0) {
        FaultCode prev_fault = FaultManager_GetLast();
        UartProtocol_Print("\r\n[WARN] clrerr clears ALL faults including safety-critical");
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
        UartProtocol_ResetEmergencyPreemptCount();
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
        return;
    }

    /* --- debug --- */
    if (strcmp(cmd, "debug on") == 0)  { s->verboseDebug = true;  UartProtocol_Print("\r\n[OK] Debug ON");  return; }
    if (strcmp(cmd, "debug off") == 0) { s->verboseDebug = false; UartProtocol_Print("\r\n[OK] Debug OFF"); return; }

    /* --- telemetry --- */
    if (strcmp(cmd, "dbg on") == 0)  { Telemetry_SetMode(TELEMETRY_DEBUG);   UartProtocol_Print("\r\n[OK] Telemetry DBG ON");  return; }
    if (strcmp(cmd, "dbg off") == 0) { Telemetry_SetMode(TELEMETRY_COMPACT); UartProtocol_Print("\r\n[OK] Telemetry DBG OFF"); return; }
    v = AppUtils_ParseLongAfter(cmd, "telper ", &ok);
    if (ok) {
        if (v < 10) v = 10;
        if (v > 60000) v = 60000;
        Telemetry_SetIntervalMs((uint32_t)v);
        UartProtocol_Printf("\r\n[OK] Telemetry=%lu ms", (unsigned long)v);
        return;
    }

    /* --- arm / disarm (safety) --- */
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
        return;
    }
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
        return;
    }
    if (strcmp(cmd, "disarm gatetest") == 0) {
        s->gate_test_armed = false;
        UartProtocol_Print("\r\n[OK] Gate test disarmed");
        return;
    }
    if (strcmp(cmd, "disarm service") == 0) {
        s->service_armed = false;
        UartProtocol_Print("\r\n[OK] Service disarmed");
        return;
    }

    /* --- help --- */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) { AppStatus_PrintHelp(); return; }

    UartProtocol_Print("\r\n[ERR] Unknown command");
}
