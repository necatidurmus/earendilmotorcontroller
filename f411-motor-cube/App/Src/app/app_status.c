/* ============================================================
 * App/Src/app_status.c
 * Status, help, and Hall map text output.
 * Extracted from app_main.c — output format must be identical.
 * ============================================================ */
#include "app_status.h"
#include "app_state.h"
#include "app_config.h"
#include "uart_protocol.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "fault_manager.h"
#include "motor_driver.h"
#include "bldc_commutation.h"
#include "storage.h"
#include "stm32f4xx_hal.h"

void AppStatus_PrintStatus(void)
{
    AppState *s = AppState_Get();
    uint32_t rpm = HallSensor_CalculateRpm();
    int32_t  tcmd= SpeedPI_GetRawTargetRpm();
    float    trmp= SpeedPI_GetRampedTargetRpm();
    float    frpm= HallSensor_GetFilteredRpm();
    uint8_t  hall= HallSensor_GetStableRaw();
    uint8_t  cand= HallSensor_GetCandidateRaw();
    uint8_t  candc= HallSensor_GetCandidateCount();
    float    kp, ki;
    SpeedPI_GetGains(&kp, &ki);
    uint16_t base[SPEED_PI_BAND_COUNT];
    SpeedPI_GetBasePwm(base);
    uint16_t boost[SPEED_PI_BAND_COUNT];
    uint16_t stms;
    SpeedPI_GetBoostPwm(boost, &stms);
    float rup, rdown;
    SpeedPI_GetRampRates(&rup, &rdown);

    UartProtocol_Print("\r\n--- STATUS ---");
    UartProtocol_Printf("\r\nMode: %s", s->mode == MODE_SPEED ? "SPEED" : "DUTY");
    UartProtocol_Printf("\r\nPhase: %d", (int)s->phase);
    UartProtocol_Printf("\r\nDir: %s",
        s->direction > 0 ? "FWD" : (s->direction < 0 ? "REV" : "STOP"));
    UartProtocol_Printf("\r\nRPM(meas): %lu", (unsigned long)rpm);
    UartProtocol_Printf("\r\nHall: stable=%u cand=%u/%u mapped=%u driven=%u",
        (unsigned)hall, (unsigned)cand, (unsigned)candc,
        (unsigned)HallSensor_GetMappedState(),
        (unsigned)HallSensor_GetLastDrivenState());
    UartProtocol_Printf("\r\nHall edges: valid=%lu invalid_raw=%lu invalid_trans=%lu irq=%lu",
        (unsigned long)HallSensor_GetValidTransitionCount(),
        (unsigned long)HallSensor_GetInvalidRawCount(),
        (unsigned long)HallSensor_GetInvalidTransitionCount(),
        (unsigned long)HallSensor_GetIrqCount());
    UartProtocol_Printf("\r\nPI Tcmd=%ld Trmp=%d F=%d", (long)tcmd, (int)trmp, (int)frpm);
    UartProtocol_Printf("\r\nPI Kp_m=%ld Ki_m=%ld",
        (long)(kp * 1000.0f), (long)(ki * 1000.0f));
    UartProtocol_Print("\r\nBase");
    for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
        UartProtocol_Printf(" %u", (unsigned)base[i]);
    }
    UartProtocol_Print("\r\nBoost");
    for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
        UartProtocol_Printf(" %u", (unsigned)boost[i]);
    }
    UartProtocol_Printf(" ms=%u", (unsigned)stms);
    UartProtocol_Printf("\r\nRamp up=%ld down=%ld", (long)rup, (long)rdown);
    UartProtocol_Printf("\r\nDuty: target=%u current=%u default=%u kick=%u/%u ramp_step=%u/%u",
        (unsigned)s->target_duty, (unsigned)s->current_duty,
        (unsigned)s->default_pwm,
        (unsigned)s->kick_duty, (unsigned)s->kick_ms,
        (unsigned)s->ramp_step, (unsigned)s->ramp_interval_ms);
    UartProtocol_Printf("\r\nFault: %s", FaultManager_GetName(FaultManager_GetLast()));
    UartProtocol_Printf("\r\nHallFault: %d  TXDrops: %lu  UARTErr: %lu  RxBytes: %lu",
        (int)HallSensor_GetFault(),
        (unsigned long)UartProtocol_GetTxDropCount(),
        (unsigned long)UartProtocol_GetUartErrorCount(),
        (unsigned long)UartProtocol_GetRxByteCount());
    UartProtocol_Printf("\r\nSafety: current_sense=NOT_PRESENT current_limit=DISABLED bkin=DISABLED");
    UartProtocol_Printf("\r\nSafety: safety_level=BENCH_ONLY_BATTERY_UNSAFE");
    UartProtocol_Printf("\r\nSafety: gate_arm=%u service_arm=%u safety_lock=%u",
        (unsigned)s->gate_test_armed,
        (unsigned)s->service_armed,
        (unsigned)MotorDriver_IsSafetyLocked());
    UartProtocol_Printf("\r\nDrops: rx=%lu tx=%lu cmd=%lu epreempt=%lu",
        (unsigned long)UartProtocol_GetRxDropCount(),
        (unsigned long)UartProtocol_GetTxDropCount(),
        (unsigned long)UartProtocol_GetCmdDropCount(),
        (unsigned long)UartProtocol_GetEmergencyPreemptCount());
    UartProtocol_Printf("\r\nAge: cmd=%lu hall=%lu",
        (unsigned long)(s->last_motor_cmd_ms == 0U ? 0UL : HAL_GetTick() - s->last_motor_cmd_ms),
        (unsigned long)(s->last_edge_ms == 0U ? 0UL : HAL_GetTick() - s->last_edge_ms));
    {
        const char *src_name = "DEFAULT";
        switch (s->hall_map_source) {
        case 1: src_name = "RAM_IDENTIFY"; break;
        case 2: src_name = "RAM_MANUAL"; break;
        case 3: src_name = "FLASH"; break;
        case 4: src_name = "INVALID_FALLBACK"; break;
        default: src_name = "DEFAULT"; break;
        }
        uint8_t tmpmap[8];
        Commutation_GetMap(tmpmap);
        bool map_valid = Commutation_ValidateHallMap(tmpmap);
        UartProtocol_Printf("\r\nHallMap: source=%s valid=%u dirty=%u cfg=%s",
            src_name, (unsigned)map_valid, (unsigned)s->hall_map_dirty,
            Storage_HasValidConfig() ? "SAVED" : "DEFAULT");
        if (s->identify_was_run) {
            const char *ires = "NONE";
            switch (s->identify_last_result) {
            case 1: ires = "OK"; break;
            case 2: ires = "REJECTED_DUPLICATE"; break;
            case 3: ires = "REJECTED_MISSING"; break;
            case 4: ires = "REJECTED_INVALID_RAW"; break;
            case 5: ires = "ABORTED"; break;
            default: ires = "NONE"; break;
            }
            UartProtocol_Printf("\r\nIdentify: last_result=%s", ires);
        }
    }
    UartProtocol_PrintNewline();
}

void AppStatus_PrintHelp(void)
{
    UartProtocol_Print(
        "\r\n============================"
        "\r\n f/forward  |  f<0-4000>"
        "\r\n b/backward |  b<0-4000>"
        "\r\n s/stop     (coast)"
        "\r\n pwm <0-4000>"
        "\r\n mode duty"
        "\r\n mode speed"
        "\r\n rpm <signed>"
        "\r\n rpm        (status)"
        "\r\n pi <kp> <ki>"
        "\r\n base <b1> ... <b8>"
        "\r\n boost <b1> ... <b8> <ms>"
        "\r\n ramp <up> <down>"
        "\r\n ramp on/off    (duty ramp enable)"
        "\r\n kick on/off    (startup kick enable)"
        "\r\n kickduty/kickms/ramprate/rampms/defpwm"
        "\r\n spstat"
        "\r\n hall"
        "\r\n status"
        "\r\n identify / scan / test"
        "\r\n gatetest <0-5> <1-4000>  (motor disconnected only, arming req)"
        "\r\n clrerr"
        "\r\n estop / safe / alloff"
        "\r\n arm gatetest <token>"
        "\r\n arm service <token>"
        "\r\n disarm gatetest"
        "\r\n disarm service"
        "\r\n dbg on/off | telper <ms>"
        "\r\n"
        "\r\n Config persistence (Flash):"
        "\r\n  savecfg / save / saveall  save runtime config to Flash"
        "\r\n                          (saveall=alias; Hall map is separate)"
        "\r\n  loadcfg                 load config from Flash into runtime"
        "\r\n  erasecfg                erase saved config from Flash"
        "\r\n  cfg                     show runtime config + Flash status"
        "\r\n  defaults                apply factory defaults to RAM"
        "\r\n                          (does NOT auto-save; use savecfg to persist)"
        "\r\n"
        "\r\n Hall map commands:"
        "\r\n  map                  show active map"
        "\r\n  map validate         validate active map"
        "\r\n  map edit             copy active to candidate"
        "\r\n  map set <raw> <sec>  edit candidate entry"
        "\r\n  map candidate        show candidate map"
        "\r\n  map apply            apply candidate -> active"
        "\r\n  map discard          discard candidate"
        "\r\n  map default          load default map"
        "\r\n  map load             load Hall map from flash"
        "\r\n  map save             save Hall map to flash"
        "\r\n  reload               alias for map load (Hall map only, NOT config)"
        "\r\n  mapreset             alias for map default"
        "\r\n"
        "\r\n help / ?"
        "\r\n============================");
    UartProtocol_PrintNewline();
}

void AppStatus_PrintHallMap(void)
{
    uint8_t map[8];
    Commutation_GetMap(map);
    UartProtocol_Print("\r\n[MAP] ");
    for (uint8_t i = 0; i < 8; i++) {
        UartProtocol_Printf("%u ", (unsigned)map[i]);
    }
    UartProtocol_PrintNewline();
}
