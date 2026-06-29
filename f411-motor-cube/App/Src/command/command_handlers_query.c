/* ============================================================
 * App/Src/command/command_handlers_query.c
 * Query/status command handlers: status, help, hall, spstat,
 * debug, dbg, telper.
 * ============================================================ */
#include "command_handlers_query.h"
#include "app_state.h"
#include "app_config.h"
#include "app_status.h"
#include "app_utils.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "uart_protocol.h"
#include "telemetry.h"

#include <string.h>

bool CommandHandlers_Query_Handle(char *cmd)
{
    AppState *s = AppState_Get();
    long v;
    bool ok;

    /* --- status --- */
    if (strcmp(cmd, "status") == 0) { AppStatus_PrintStatus(); return true; }

    /* --- help --- */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) { AppStatus_PrintHelp(); return true; }

    /* --- hall --- */
    if (strcmp(cmd, "hall") == 0 || strcmp(cmd, "h") == 0) {
        UartProtocol_Printf("\r\n[INFO] Hall=%u State=%u",
            (unsigned)HallSensor_GetStableRaw(),
            (unsigned)HallSensor_GetMappedState());
        return true;
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
        {
            float kp, ki, up, down;
            uint16_t base[SPEED_PI_BAND_COUNT];
            uint16_t boost[SPEED_PI_BAND_COUNT];
            uint16_t ms;
            SpeedPI_GetGains(&kp, &ki);
            SpeedPI_GetBasePwm(base);
            SpeedPI_GetBoostPwm(boost, &ms);
            SpeedPI_GetRampRates(&up, &down);
            /* NOTE: newlib-nano does not support %f.  Print gains as
             * milli-scaled integers to match the pi/kp/ki handlers. */
            UartProtocol_Printf("\r\nKp_m=%ld Ki_m=%ld",
                (long)(kp * 1000.0f), (long)(ki * 1000.0f));
            UartProtocol_Print("\r\nBase");
            for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
                UartProtocol_Printf(" %u", (unsigned)base[i]);
            }
            UartProtocol_Print("\r\nBoost");
            for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
                UartProtocol_Printf(" %u", (unsigned)boost[i]);
            }
            UartProtocol_Printf(" ms=%u", (unsigned)ms);
            UartProtocol_Printf("\r\nRamp up=%ld down=%ld",
                (long)up, (long)down);
        }
        UartProtocol_PrintNewline();
        return true;
    }

    /* --- debug --- */
    if (strcmp(cmd, "debug on") == 0)  { s->verboseDebug = true;  UartProtocol_Print("\r\n[OK] Debug ON");  return true; }
    if (strcmp(cmd, "debug off") == 0) { s->verboseDebug = false; UartProtocol_Print("\r\n[OK] Debug OFF"); return true; }

    /* --- telemetry --- */
    if (strcmp(cmd, "dbg on") == 0)  { Telemetry_SetMode(TELEMETRY_DEBUG);   UartProtocol_Print("\r\n[OK] Telemetry DBG ON");  return true; }
    if (strcmp(cmd, "dbg off") == 0) { Telemetry_SetMode(TELEMETRY_COMPACT); UartProtocol_Print("\r\n[OK] Telemetry DBG OFF"); return true; }
    v = AppUtils_ParseLongAfter(cmd, "telper ", &ok);
    if (ok) {
        if (v < 20) v = 20;    /* matches Telemetry_SetIntervalMs lower clamp */
        if (v > 5000) v = 5000;  /* matches Telemetry_SetIntervalMs upper clamp */
        Telemetry_SetIntervalMs((uint32_t)v);
        UartProtocol_Printf("\r\n[OK] Telemetry=%lu ms", (unsigned long)v);
        return true;
    }

    return false;
}
