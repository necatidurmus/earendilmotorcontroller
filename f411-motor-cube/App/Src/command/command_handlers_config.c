/* ============================================================
 * App/Src/command/command_handlers_config.c
 * Config command handlers: pi, kp, ki, base, boost, ramp,
 * map, kick*, ramp*, defpwm, defaults, loadcfg, save*.
 * ============================================================ */
#include "command_handlers_config.h"
#include "app_state.h"
#include "app_types.h"
#include "app_config.h"
#include "app_status.h"
#include "app_utils.h"
#include "speed_pi.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "fault_manager.h"
#include "storage.h"
#include "motion_control.h"
#include "uart_protocol.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

bool CommandHandlers_Config_Handle(char *cmd)
{
    AppState *s = AppState_Get();
    long v;
    bool ok;
    float fv;

    /* --- pi <kp> <ki> --- */
    if (AppUtils_StartsWith(cmd, "pi ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        const char *p = cmd + 3;
        while (*p == ' ') p++;
        char *end1 = NULL;
        float kp = strtof(p, &end1);
        if (end1 == p) { UartProtocol_Print("\r\n[ERR] Usage: pi <kp> <ki>"); return true; }
        if (!isfinite(kp)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return true; }
        while (*end1 == ' ') end1++;
        char *end2 = NULL;
        float ki = strtof(end1, &end2);
        if (end2 == end1) { UartProtocol_Print("\r\n[ERR] Usage: pi <kp> <ki>"); return true; }
        if (!isfinite(ki)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return true; }
        while (*end2 == ' ') end2++;
        if (*end2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return true; }
        SpeedPI_SetKp(kp);
        SpeedPI_SetKi(ki);
        UartProtocol_Printf("\r\n[OK] Kp_m=%ld Ki_m=%ld",
            (long)(kp * 1000.0f), (long)(ki * 1000.0f));
        return true;
    }

    /* --- kp / ki standalone (compat) --- */
    fv = AppUtils_ParseFloatAfter(cmd, "kp ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        SpeedPI_SetKp(fv); UartProtocol_Printf("\r\n[OK] Kp_m=%ld", (long)(fv * 1000.0f)); return true;
    }
    fv = AppUtils_ParseFloatAfter(cmd, "ki ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        SpeedPI_SetKi(fv); UartProtocol_Printf("\r\n[OK] Ki_m=%ld", (long)(fv * 1000.0f)); return true;
    }

    /* --- base <lo> <mid> <hi> --- */
    if (AppUtils_StartsWith(cmd, "base ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        const char *p = cmd + 5;
        while (*p == ' ') p++;
        char *e1 = NULL, *e2 = NULL, *e3 = NULL;
        long lo  = strtol(p, &e1, 10);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return true; }
        while (*e1 == ' ') e1++;
        long mid = strtol(e1, &e2, 10);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return true; }
        while (*e2 == ' ') e2++;
        long hi  = strtol(e2, &e3, 10);
        if (e3 == e2) { UartProtocol_Print("\r\n[ERR] Usage: base <lo> <mid> <hi>"); return true; }
        while (*e3 == ' ') e3++;
        if (*e3 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return true; }
        if (lo < 0) lo = 0;
        if (lo > PWM_MAX_DUTY) lo = PWM_MAX_DUTY;
        if (mid < 0) mid = 0;
        if (mid > PWM_MAX_DUTY) mid = PWM_MAX_DUTY;
        if (hi < 0) hi = 0;
        if (hi > PWM_MAX_DUTY) hi = PWM_MAX_DUTY;
        SpeedPI_SetBasePwm((uint16_t)lo, (uint16_t)mid, (uint16_t)hi);
        uint16_t alo, amid, ahi;
        SpeedPI_GetBasePwm(&alo, &amid, &ahi);
        UartProtocol_Printf("\r\n[OK] Base L=%u M=%u H=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi);
        return true;
    }

    /* --- boost <lo> <mid> <hi> <ms> --- */
    if (AppUtils_StartsWith(cmd, "boost ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        const char *p = cmd + 6;
        while (*p == ' ') p++;
        char *e1 = NULL, *e2 = NULL, *e3 = NULL, *e4 = NULL;
        long lo  = strtol(p, &e1, 10);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return true; }
        while (*e1 == ' ') e1++;
        long mid = strtol(e1, &e2, 10);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return true; }
        while (*e2 == ' ') e2++;
        long hi  = strtol(e2, &e3, 10);
        if (e3 == e2) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return true; }
        while (*e3 == ' ') e3++;
        long ms  = strtol(e3, &e4, 10);
        if (e4 == e3) { UartProtocol_Print("\r\n[ERR] Usage: boost <lo> <mid> <hi> <ms>"); return true; }
        while (*e4 == ' ') e4++;
        if (*e4 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return true; }
        if (lo < 0) lo = 0;
        if (lo > PWM_MAX_DUTY) lo = PWM_MAX_DUTY;
        if (mid < 0) mid = 0;
        if (mid > PWM_MAX_DUTY) mid = PWM_MAX_DUTY;
        if (hi < 0) hi = 0;
        if (hi > PWM_MAX_DUTY) hi = PWM_MAX_DUTY;
        if (ms < 0) ms = 0;
        if (ms > 1000) ms = 1000;
        SpeedPI_SetBoostPwm((uint16_t)lo, (uint16_t)mid, (uint16_t)hi, (uint16_t)ms);
        uint16_t alo, amid, ahi;
        uint16_t ams;
        SpeedPI_GetBoostPwm(&alo, &amid, &ahi, &ams);
        UartProtocol_Printf("\r\n[OK] Boost L=%u M=%u H=%u ms=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi, (unsigned)ams);
        return true;
    }

    /* --- ramp <up> <down> --- */
    if (AppUtils_StartsWith(cmd, "ramp ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        const char *p = cmd + 5;
        while (*p == ' ') p++;
        char *e1 = NULL;
        float up = strtof(p, &e1);
        if (e1 == p) { UartProtocol_Print("\r\n[ERR] Usage: ramp <up_rpm_s> <down_rpm_s>"); return true; }
        if (!isfinite(up)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return true; }
        while (*e1 == ' ') e1++;
        char *e2 = NULL;
        float down = strtof(e1, &e2);
        if (e2 == e1) { UartProtocol_Print("\r\n[ERR] Usage: ramp <up_rpm_s> <down_rpm_s>"); return true; }
        if (!isfinite(down)) { UartProtocol_Print("\r\n[ERR] nan/inf rejected"); return true; }
        while (*e2 == ' ') e2++;
        if (*e2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return true; }
        SpeedPI_SetRamp(up, down);
        UartProtocol_Printf("\r\n[OK] Ramp up=%ld down=%ld", (long)up, (long)down);
        return true;
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
            return true;
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
            return true;
        }

        if (strcmp(sub, " default") == 0 || strcmp(sub, " reset") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return true;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return true;
            }
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 0U;
            s->hall_map_dirty = false;
            s->candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded into RAM");
            AppStatus_PrintHallMap();
            return true;
        }

        if (strcmp(sub, " edit") == 0) {
            Commutation_GetMap(s->candidate_map);
            s->candidate_active = true;
            UartProtocol_Print("\r\n[OK] Active map copied to candidate");
            return true;
        }

        if (AppUtils_StartsWith(sub, " set ")) {
            const char *p = sub + 5;
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return true;
            }
            char *end1 = NULL;
            long raw = strtol(p, &end1, 10);
            if (end1 == p || raw < 0 || raw > 7) {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return true;
            }
            const char *q = end1;
            while (*q == ' ') q++;
            if (*q == '\0') {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return true;
            }
            long sector;
            if (strcmp(q, "invalid") == 0 || strcmp(q, "255") == 0) {
                sector = 255;
            } else {
                char *end2 = NULL;
                sector = strtol(q, &end2, 10);
                if (end2 == q) {
                    UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                    return true;
                }
                while (*end2 == ' ') end2++;
                if (*end2 != '\0') {
                    UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                    return true;
                }
            }
            if (sector != 255 && (sector < 0 || sector > 5)) {
                UartProtocol_Print("\r\n[ERR] Sector must be 0..5 or 'invalid'");
                return true;
            }
            if ((raw == 0 || raw == 7) && sector != 255) {
                UartProtocol_Print("\r\n[ERR] raw 0 and raw 7 must be 'invalid' (255)");
                return true;
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
            return true;
        }

        if (strcmp(sub, " candidate") == 0) {
            if (!s->candidate_active) {
                UartProtocol_Print("\r\n[INFO] No candidate map. Use 'map edit' first.");
                return true;
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
            return true;
        }

        if (strcmp(sub, " apply") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return true;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return true;
            }
            if (!s->candidate_active) {
                UartProtocol_Print("\r\n[ERR] No candidate map. Use 'map edit' first.");
                return true;
            }
            char reason[32];
            if (!Commutation_ValidateHallMapVerbose(s->candidate_map, reason, sizeof(reason))) {
                UartProtocol_Printf("\r\n[ERR] Candidate map rejected: %s", reason);
                UartProtocol_Print("\r\n[SAFE] Active map unchanged");
                return true;
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
            return true;
        }

        if (strcmp(sub, " discard") == 0) {
            s->candidate_active = false;
            memset(s->candidate_map, 255, sizeof(s->candidate_map));
            UartProtocol_Print("\r\n[OK] Candidate map discarded");
            return true;
        }

        if (strcmp(sub, " load") == 0 || strcmp(sub, " reload") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return true;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return true;
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
            return true;
        }

        if (strcmp(sub, " save") == 0) {
            UartProtocol_Print("\r\n[ERR] Persistent storage disabled in this build");
            UartProtocol_Print("\r\n[INFO] Map lives in RAM only until reset");
            return true;
        }

        if (strcmp(cmd, "mapreset") == 0) {
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return true;
            }
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s->hall_map_source = 0U;
            s->hall_map_dirty = false;
            s->candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded");
            AppStatus_PrintHallMap();
            return true;
        }

        UartProtocol_Print("\r\n[ERR] Unknown map subcommand. Type 'help' for list.");
        return true;
    }

    /* Legacy compat: "save" */
    if (strcmp(cmd, "save") == 0 || strcmp(cmd, "savecfg") == 0 ||
        strcmp(cmd, "saveall") == 0) {
        UartProtocol_Print("\r\n[ERR] Persistent storage disabled in this build");
        return true;
    }

    /* Legacy compat: "reload" */
    if (strcmp(cmd, "reload") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        if (FaultManager_GetLast() != FAULT_NONE) {
            UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
            return true;
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
        return true;
    }

    /* --- savecfg / loadcfg / defaults --- */
    if (strcmp(cmd, "loadcfg") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        uint16_t kd, km, rs, ri, dp, bh;
        if (Storage_LoadConfig(&kd, &km, &rs, &ri, &dp, &bh)) {
            s->kick_duty = kd; s->kick_ms = km;
            s->ramp_step = rs; s->ramp_interval_ms = ri;
            s->default_pwm = dp; s->brake_hold_ms = bh;
            MotionControl_ClampLoadedConfig();
            UartProtocol_Print("\r\n[OK] Config loaded");
        } else {
            UartProtocol_Print("\r\n[INFO] No saved config, defaults kept");
        }
        return true;
    }
    if (strcmp(cmd, "defaults") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        s->kick_enabled = false;
        s->ramp_enabled = true;
        s->kick_duty = 960;
        s->kick_ms = 50;
        s->ramp_step = 128;
        s->ramp_interval_ms = 5;
        s->default_pwm = 1600;
        s->brake_hold_ms = BRAKE_HOLD_MS;
        UartProtocol_Print("\r\n[OK] Defaults loaded into RAM (kick OFF, ramp ON)");
        return true;
    }

    /* --- kick / ramp config commands --- */
    if (strcmp(cmd, "kick on") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        s->kick_enabled = true;  UartProtocol_Print("\r\n[OK] Kick ON");  return true;
    }
    if (strcmp(cmd, "kick off") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        s->kick_enabled = false; UartProtocol_Print("\r\n[OK] Kick OFF"); return true;
    }
    if (strcmp(cmd, "ramp on") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        s->ramp_enabled = true;  UartProtocol_Print("\r\n[OK] Ramp ON");  return true;
    }
    if (strcmp(cmd, "ramp off") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        s->ramp_enabled = false; UartProtocol_Print("\r\n[OK] Ramp OFF"); return true;
    }

    v = AppUtils_ParseLongAfter(cmd, "kickduty ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (v < 0) v = 0;
        if (v > KICK_DUTY_MAX) v = KICK_DUTY_MAX;
        s->kick_duty = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] KickDuty=%lu", (unsigned long)v);
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "kickms ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (v < 0) v = 0;
        if (v > KICK_MS_MAX) v = KICK_MS_MAX;
        s->kick_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] KickMs=%lu", (unsigned long)v);
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "ramprate ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (v < 0) v = 0;
        if (v > RAMP_STEP_MAX) v = RAMP_STEP_MAX;
        s->ramp_step = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] RampStep=%lu", (unsigned long)v);
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "rampms ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (v < 0) v = 0;
        if (v > RAMP_INTERVAL_MS_MAX) v = RAMP_INTERVAL_MS_MAX;
        s->ramp_interval_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] RampMs=%lu", (unsigned long)v);
        return true;
    }
    v = AppUtils_ParseLongAfter(cmd, "defpwm ", &ok);
    if (ok) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (v < 0) v = 0;
        if (v > DEFAULT_PWM_MAX) v = DEFAULT_PWM_MAX;
        s->default_pwm = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] DefaultPWM=%lu", (unsigned long)v);
        return true;
    }

    return false;
}
