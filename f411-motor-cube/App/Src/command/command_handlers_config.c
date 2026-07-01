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
#include "config_snapshot.h"
#include "motion_control.h"
#include "telemetry.h"
#include "uart_protocol.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static bool parse_u16_values(const char *text, uint16_t *values,
                             uint8_t count, uint16_t maximum)
{
    const char *p = text;
    for (uint8_t i = 0U; i < count; i++) {
        while (*p == ' ') p++;
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end == p || value < 0L || value > (long)maximum) return false;
        values[i] = (uint16_t)value;
        p = end;
    }
    while (*p == ' ') p++;
    return *p == '\0';
}

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

    /* --- base <b1> ... <b8> --- */
    if (AppUtils_StartsWith(cmd, "base ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        uint16_t bands[SPEED_PI_BAND_COUNT];
        if (!parse_u16_values(cmd + 5, bands, SPEED_PI_BAND_COUNT, PWM_MAX_DUTY)) {
            UartProtocol_Print("\r\n[ERR] Usage: base <b1> <b2> <b3> <b4> <b5> <b6> <b7> <b8>");
            return true;
        }
        SpeedPI_SetBasePwm(bands);
        UartProtocol_Print("\r\n[OK] Base");
        for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
            UartProtocol_Printf(" %u", (unsigned)bands[i]);
        }
        return true;
    }

    /* --- boost <b1> ... <b8> <ms> --- */
    if (AppUtils_StartsWith(cmd, "boost ")) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        uint16_t values[SPEED_PI_BAND_COUNT + 1U];
        if (!parse_u16_values(cmd + 6, values, SPEED_PI_BAND_COUNT + 1U, PWM_MAX_DUTY) ||
            values[SPEED_PI_BAND_COUNT] > 1000U) {
            UartProtocol_Print("\r\n[ERR] Usage: boost <b1> <b2> <b3> <b4> <b5> <b6> <b7> <b8> <ms>");
            return true;
        }
        SpeedPI_SetBoostPwm(values, values[SPEED_PI_BAND_COUNT]);
        UartProtocol_Print("\r\n[OK] Boost");
        for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
            UartProtocol_Printf(" %u", (unsigned)values[i]);
        }
        UartProtocol_Printf(" ms=%u", (unsigned)values[SPEED_PI_BAND_COUNT]);
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
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return true;
            }
            uint8_t map[8];
            Commutation_GetMap(map);
            if (Storage_SaveHallMap(map)) {
                s->hall_map_dirty = false;
                s->hall_map_source = 3U;  /* FLASH */
                UartProtocol_Print("\r\n[OK] Hall map saved to flash");
            } else {
                UartProtocol_Print("\r\n[ERR] Hall map save failed");
            }
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

    /* Legacy compat: "save" / "savecfg" / "saveall" */
    if (strcmp(cmd, "save") == 0 || strcmp(cmd, "savecfg") == 0 ||
        strcmp(cmd, "saveall") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        PersistentConfig_t cfg;
        ConfigSnapshot_FromRuntime(&cfg);
        if (!ConfigSnapshot_Validate(&cfg)) {
            UartProtocol_Print("\r\n[ERR] Config validation failed");
            return true;
        }
        if (Storage_SaveConfig(&cfg)) {
            UartProtocol_Print("\r\n[OK] Config saved");
        } else {
            UartProtocol_Print("\r\n[ERR] Config save failed");
        }
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

    /* --- savecfg / loadcfg / erasecfg / cfg / defaults --- */
    if (strcmp(cmd, "loadcfg") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        PersistentConfig_t cfg;
        if (Storage_LoadConfig(&cfg)) {
            ConfigSnapshot_ApplyToRuntime(&cfg);
            UartProtocol_Print("\r\n[OK] Config loaded");
        } else {
            UartProtocol_Print("\r\n[INFO] No saved config");
        }
        return true;
    }
    if (strcmp(cmd, "erasecfg") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        if (Storage_EraseConfig()) {
            UartProtocol_Print("\r\n[OK] Config erased");
        } else {
            UartProtocol_Print("\r\n[ERR] Config erase failed");
        }
        return true;
    }
    if (strcmp(cmd, "cfg") == 0) {
        PersistentConfig_t cfg;
        ConfigSnapshot_FromRuntime(&cfg);

        UartProtocol_Printf("\r\nKp_m=%ld Ki_m=%ld",
            (long)(cfg.kp * 1000.0f), (long)(cfg.ki * 1000.0f));
        UartProtocol_Print("\r\nBase");
        for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++)
            UartProtocol_Printf(" %u", (unsigned)cfg.base_pwm[i]);
        UartProtocol_Print("\r\nBoost");
        for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++)
            UartProtocol_Printf(" %u", (unsigned)cfg.boost_pwm[i]);
        UartProtocol_Printf(" ms=%u", (unsigned)cfg.boost_ms);
        UartProtocol_Printf("\r\nRamp up=%ld down=%ld",
            (long)cfg.ramp_up, (long)cfg.ramp_down);
        UartProtocol_Printf("\r\nKick %s duty=%u ms=%u",
            cfg.kick_enabled ? "ON" : "OFF",
            (unsigned)cfg.kick_duty, (unsigned)cfg.kick_ms);
        UartProtocol_Printf("\r\nDutyRamp %s step=%u ms=%u",
            cfg.ramp_enabled ? "ON" : "OFF",
            (unsigned)cfg.ramp_step, (unsigned)cfg.ramp_interval_ms);
        UartProtocol_Printf("\r\nDefaultPWM=%u BrakeHoldMs=%u",
            (unsigned)cfg.default_pwm, (unsigned)cfg.brake_hold_ms);
        UartProtocol_Printf("\r\nTelPer=%lu",
            (unsigned long)cfg.telemetry_interval_ms);
        UartProtocol_Printf("\r\nFlash: %s",
            Storage_HasValidConfig() ? "VALID" : "EMPTY");
        UartProtocol_PrintNewline();
        return true;
    }
    if (strcmp(cmd, "defaults") == 0) {
        if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return true;
        }
        /* Reset AppState fields to defaults. */
        s->kick_enabled = false;
        s->ramp_enabled = true;
        s->kick_duty = 960;
        s->kick_ms = 50;
        s->ramp_step = 128;
        s->ramp_interval_ms = 5;
        s->default_pwm = 1600;
        s->brake_hold_ms = BRAKE_HOLD_MS;
        /* Reset SpeedPI to defaults. */
        SpeedPI_SetKp(DEFAULT_SPEED_KP);
        SpeedPI_SetKi(DEFAULT_SPEED_KI);
        {
            uint16_t bd[8] = {
                DEFAULT_BASE_PWM_1, DEFAULT_BASE_PWM_2, DEFAULT_BASE_PWM_3, DEFAULT_BASE_PWM_4,
                DEFAULT_BASE_PWM_5, DEFAULT_BASE_PWM_6, DEFAULT_BASE_PWM_7, DEFAULT_BASE_PWM_8
            };
            SpeedPI_SetBasePwm(bd);
        }
        {
            uint16_t bd[8] = {
                DEFAULT_BOOST_PWM_1, DEFAULT_BOOST_PWM_2, DEFAULT_BOOST_PWM_3, DEFAULT_BOOST_PWM_4,
                DEFAULT_BOOST_PWM_5, DEFAULT_BOOST_PWM_6, DEFAULT_BOOST_PWM_7, DEFAULT_BOOST_PWM_8
            };
            SpeedPI_SetBoostPwm(bd, DEFAULT_BOOST_TIME_MS);
        }
        SpeedPI_SetRamp(DEFAULT_RAMP_UP_RPM_SEC, DEFAULT_RAMP_DOWN_RPM_SEC);
        Telemetry_SetIntervalMs(TELEMETRY_INTERVAL_MS);
        SpeedPI_Reset();
        UartProtocol_Print("\r\n[OK] Defaults loaded into RAM (not saved to flash)");
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
