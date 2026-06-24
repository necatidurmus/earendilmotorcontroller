/* ============================================================
 * App/Src/telemetry.c
 * Rate-limited, non-blocking telemetry output.
 *
 * Compact format (default):
 *   RPM:<m>,T:<t>,D:<d>,DIR:<F|R|N>,APP_PH:<p>,SP:<0|1>,BRAKE:<0|1>,
 *   FC:<c>,H:<h>,PWM_SET:<s>,PWM_ACT:<a>\n
 *
 *   RPM     measured mechanical RPM
 *   T       |target rpm| when speed mode, else 0
 *   D       current applied duty (0..255)
 *   DIR     F / R / N
 *   APP_PH  app motor phase (0=STOP,1=RUN,2=BRAKE,3=NEUTRAL,4=FAULT)
 *   SP      1 if speed (PI) mode, else 0
 *   BRAKE   1 if brake phase, else 0
 *   FC      fault code (FaultManager)
 *   H       raw Hall code (0..7)
 *   PWM_SET target duty (0..255)
 *   PWM_ACT actual applied duty (0..255)  -- never CCR ticks
 *
 * Debug format (dbg on):
 *   RPM:<m>,RF:<f>,Tcmd:<c>,Trmp:<r>,ERR:<e>,D:<d>,SPD_PH:<p>,FC:<c>,
 *   PWM_SET:<s>,PWM_ACT:<a>\n
 * ============================================================ */

#include "telemetry.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "fault_manager.h"
#include "app_main.h"
#include "uart_protocol.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <stdlib.h>

static TelemetryMode s_mode = TELEMETRY_COMPACT;
static uint32_t      s_interval_ms = TELEMETRY_INTERVAL_MS;
static uint32_t      s_last_ms     = 0U;

void Telemetry_Init(void)
{
    s_mode = TELEMETRY_COMPACT;
    s_interval_ms = TELEMETRY_INTERVAL_MS;
    s_last_ms = HAL_GetTick();
}

void Telemetry_Tick(uint32_t nowMs)
{
    if ((nowMs - s_last_ms) < s_interval_ms) return;
    s_last_ms = nowMs;

    if (s_mode == TELEMETRY_DEBUG) {
        float frpm    = HallSensor_GetFilteredRpm();
        int32_t tcmd  = SpeedPI_GetRawTargetRpm();
        float trmp    = SpeedPI_GetRampedTargetRpm();
        float err     = trmp - frpm;
        uint8_t setd  = App_GetTargetDuty();
        uint8_t actd  = MotorDriver_GetDuty();
        SpeedPhase sp = SpeedPI_GetPhase();
        FaultCode fc  = FaultManager_GetLast();

        UartProtocol_Printf("RPM:%u,RF:%d,Tcmd:%ld,Trmp:%d,ERR:%d,D:%u,SPD_PH:%u,FC:%u,PWM_SET:%u,PWM_ACT:%u",
                            (unsigned)HallSensor_CalculateRpm(),
                            (int)frpm,
                            (long)tcmd,
                            (int)trmp,
                            (int)err,
                            (unsigned)App_GetCurrentDuty(),
                            (unsigned)sp,
                            (unsigned)fc,
                            (unsigned)setd,
                            (unsigned)actd);
        UartProtocol_PrintNewline();
    } else {
        uint32_t rpm    = HallSensor_CalculateRpm();
        int32_t raw_target = SpeedPI_GetRawTargetRpm();
        uint32_t target = App_IsSpeedMode()
                          ? (uint32_t)(raw_target >= 0 ? raw_target : -raw_target) : 0U;
        uint8_t  setd   = App_GetTargetDuty();
        uint8_t  actd   = MotorDriver_GetDuty();
        FaultCode fc    = FaultManager_GetLast();
        uint8_t  hall   = HallSensor_GetStableRaw();
        int8_t   dir    = App_GetDirection();
        const char dirc = (dir > 0) ? 'F' : (dir < 0 ? 'R' : 'N');

        UartProtocol_Printf("RPM:%lu,T:%lu,D:%u,DIR:%c,APP_PH:%u,SP:%u,BRAKE:%u,FC:%u,H:%u,PWM_SET:%u,PWM_ACT:%u,QDROP:%lu",
                            (unsigned long)rpm,
                            (unsigned long)target,
                            (unsigned)App_GetCurrentDuty(),
                            (char)dirc,
                            (unsigned)App_GetMotorPhase(),
                            App_IsSpeedMode() ? 1U : 0U,
                            App_IsBrakeActive() ? 1U : 0U,
                            (unsigned)fc,
                            (unsigned)hall,
                            (unsigned)setd,
                            (unsigned)actd,
                            (unsigned long)UartProtocol_GetCmdDropCount());
        UartProtocol_PrintNewline();
    }
}

void Telemetry_SetMode(TelemetryMode mode)        { s_mode = mode; }
TelemetryMode Telemetry_GetMode(void)             { return s_mode; }
void Telemetry_SetIntervalMs(uint32_t ms)
{
    if (ms < 20U)  ms = 20U;
    if (ms > 5000U) ms = 5000U;
    s_interval_ms = ms;
}
uint32_t Telemetry_GetIntervalMs(void)            { return s_interval_ms; }
