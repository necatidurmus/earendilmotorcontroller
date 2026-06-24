/* ============================================================
 * App/Src/app_main.c
 *
 * Top-level glue.  Called from Core/Src/main.c.
 *   App_Init()    - one-time setup
 *   App_Loop()    - non-blocking service loop
 *   App_Usart2RxIsr()    - called from USART2 ISR
 *   App_Tim6SchedulerTick() - called from TIM6 update ISR
 *   App_Tim1BrkIsr()      - called from TIM1 BRK ISR
 *   App_Tim4HallIsr()     - called from TIM4 ISR (Hall interface)
 *
 * Runtime model:
 *   - EXTI on PB6/PB7/PB8 fires on every Hall edge.  The ISR samples
 *     the Hall pins and TIM2 directly from registers and stores a raw
 *     snapshot + timestamp + sequence counter (no HAL overhead, no
 *     debounce in the ISR).
 *   - HallSensor_Update() in App_Loop runs the debounce state machine
 *     every iteration regardless of whether an EXTI fired — it reads
 *     the Hall pins directly, so a missed EXTI does not lose a stable
 *     transition (ISSUE-035).  Single-writer: only the main loop
 *     modifies the Hall runtime state.
 *   - Motor control is applied every loop iteration but uses a
 *     scheduler to throttle to ~1 kHz.
 *   - UART parser runs every loop iteration.
 *   - Telemetry runs every loop iteration but is rate-limited.
 * ============================================================ */

#include "app_main.h"
#include "app_config.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "speed_pi.h"
#include "uart_protocol.h"
#include "telemetry.h"
#include "fault_manager.h"
#include "service_task.h"
#include "storage.h"
#include "tim.h"
#include "usart.h"
#include "main.h"
#include "stm32f4xx_hal.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>     /* isfinite() for nan/inf rejection */

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

typedef enum {
    MODE_DUTY = 0,
    MODE_SPEED = 1
} AppMode;

typedef enum {
    PHASE_STOPPED  = 0,
    PHASE_RUNNING  = 1,
    PHASE_BRAKE    = 2,
    PHASE_NEUTRAL  = 3,
    PHASE_FAULT    = 4
} MotorPhase;

typedef enum {
    DIR_FWD = +1,
    DIR_REV = -1
} Direction;

static struct {
    AppMode     mode;
    MotorPhase  phase;
    Direction   direction;
    int8_t      pending_direction;   /* for neutral switch */

    uint8_t     target_duty;         /* manual mode */
    uint8_t     current_duty;        /* applied to motor driver */

    bool        run_request;
    bool        stop_request;
    bool        duty_update_request;

    uint32_t    last_motor_cmd_ms;
    uint32_t    last_loop_ms;
    uint32_t    phase_start_ms;
    uint32_t    neutral_release_ms;
    uint16_t    brake_hold_ms;

    bool        has_ever_run;
    uint32_t    last_edge_count;   /* Hall edge counter snapshot */
    uint32_t    last_edge_ms;      /* HAL_GetTick() when last edge seen */

    bool        verboseDebug;
    bool        queue_overflow;

    /* gatetest state */
    bool        gatetest_active;
    uint8_t     gatetest_sector;
    uint8_t     gatetest_duty;
    uint32_t    gatetest_start_ms;
    uint32_t    gatetest_timeout_ms;

    /* kick/ramp config (duty mode) */
    bool        kick_enabled;
    bool        ramp_enabled;
    uint8_t     kick_duty;
    uint16_t    kick_ms;
    uint8_t     ramp_step;
    uint16_t    ramp_interval_ms;
    uint8_t     default_pwm;
    uint32_t    last_ramp_update_ms;

    /* kick/ramp runtime state (duty mode) */
    bool        kick_active;        /* true during the kick pulse */
    uint32_t    kick_start_ms;      /* HAL_GetTick() at kick start */
    uint8_t     ramp_current_duty;  /* ramped duty being applied */
} s_app;

/* ----------------------------------------------------------------
 * ISR shims
 * ---------------------------------------------------------------- */

void App_Tim6SchedulerTick(void)
{
    /* No-op: scheduler is driven by SysTick / HAL_GetTick() in
     * App_Loop.  This stub is kept so the ISR prototype in
     * stm32f4xx_it.c remains link-clean even though TIM6 does not
     * exist on STM32F411. */
}

void App_Tim1BrkIsr(void)
{
    FaultManager_Raise(FAULT_HW_BREAK);
}

void App_Tim4HallIsr(void)
{
    /* No-op: Hall sensing uses EXTI on PB6/7/8 (ISSUE-007).
     * TIM4 Hall interface is not active.  Stub kept so the ISR
     * prototype in stm32f4xx_it.c remains link-clean. */
}

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static void stop_immediate(void)
{
    s_app.phase = PHASE_STOPPED;
    s_app.direction = (Direction)0;
    s_app.target_duty = 0U;
    s_app.current_duty = 0U;
    s_app.run_request = false;
    s_app.duty_update_request = false;
    /* ISSUE-025: clear the command watchdog timestamp so a stop
     * (rpm 0, startup fault, host-lost) does not trigger a delayed
     * spurious FAULT_WATCHDOG.  `stop` and `enter_brake()` already
     * do this — stop_immediate() must too. */
    s_app.last_motor_cmd_ms = 0U;
    /* ISSUE-026: reset has_ever_run so the next start gets the full
     * START_NO_HALL_TIMEOUT_MS startup window.  Without this the
     * flag stayed true from a previous run and the startup Hall
     * timeout was bypassed.  Snapshot the edge counter so the next
     * run detects its first edge relative to now. */
    s_app.has_ever_run = false;
    s_app.last_edge_count = HallSensor_GetEdgeCounter();
    s_app.last_edge_ms = 0U;
    /* Clear the kick/ramp state machine so the next start runs the
     * kick phase again. */
    s_app.kick_active = false;
    s_app.kick_start_ms = 0U;
    s_app.ramp_current_duty = 0U;
    s_app.last_ramp_update_ms = 0U;
    MotorDriver_AllOff();
    SpeedPI_Disable();   /* ISSUE-C: fully disable, not just reset */
}

/* ISSUE-C: Any serious motor fault requires clrerr before new motion.
 * This prevents a faulted motor from restarting on a stray command. */
static bool motion_allowed(void)
{
    return FaultManager_GetLast() == FAULT_NONE;
}

static bool service_busy(void)
{
    return s_app.gatetest_active || ServiceTask_IsActive();
}

static void begin_neutral_switch(int8_t new_direction)
{
    MotorDriver_AllOff();
    s_app.current_duty = 0U;
    s_app.phase = PHASE_NEUTRAL;
    s_app.pending_direction = new_direction;
    s_app.neutral_release_ms = HAL_GetTick() + DIRECTION_NEUTRAL_MS;
}

static void enter_brake(void)
{
    s_app.phase = PHASE_BRAKE;
    s_app.phase_start_ms = HAL_GetTick();
    s_app.direction = (Direction)0;
    s_app.run_request = false;
    SpeedPI_Reset();
    /* For initial bring-up, brake = coast too.  Active brake can be
     * added later — for now we do the safe thing. */
    MotorDriver_AllOff();
}

static void apply_duty_now(uint8_t duty)
{
    if (duty > 250U) duty = 250U;
    s_app.current_duty = duty;
    MotorDriver_SetDuty(duty);
}

static void clamp_loaded_config(void)
{
    if (s_app.kick_duty > KICK_DUTY_MAX) s_app.kick_duty = KICK_DUTY_MAX;
    if (s_app.kick_ms > KICK_MS_MAX) s_app.kick_ms = KICK_MS_MAX;
    if (s_app.ramp_step > RAMP_STEP_MAX) s_app.ramp_step = RAMP_STEP_MAX;
    if (s_app.ramp_interval_ms > RAMP_INTERVAL_MS_MAX) s_app.ramp_interval_ms = RAMP_INTERVAL_MS_MAX;
    if (s_app.default_pwm > DEFAULT_PWM_MAX) s_app.default_pwm = DEFAULT_PWM_MAX;
    if (s_app.brake_hold_ms < 100U) s_app.brake_hold_ms = 100U;
    if (s_app.brake_hold_ms > 10000U) s_app.brake_hold_ms = 10000U;
}

/* ----------------------------------------------------------------
 * Command parser
 * ---------------------------------------------------------------- */

static void trim_in_place(char *s)
{
    size_t len = strlen(s);
    while (len > 0U && (s[len-1] == '\r' || s[len-1] == '\n' ||
                        s[len-1] == ' '  || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
    size_t start = 0U;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0U) memmove(s, s + start, strlen(s + start) + 1U);
}

static void lower_in_place(char *s)
{
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static bool starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static long parse_long_after(const char *s, const char *prefix, bool *ok)
{
    if (!starts_with(s, prefix)) { *ok = false; return 0; }
    s += strlen(prefix);
    while (*s == ' ') s++;
    if (*s == '\0') { *ok = false; return 0; }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s)              { *ok = false; return 0; }
    /* Reject trailing garbage (e.g. "50abc"). */
    while (*end == ' ') end++;
    if (*end != '\0')          { *ok = false; return 0; }
    *ok = true;
    return v;
}

static float parse_float_after(const char *s, const char *prefix, bool *ok)
{
    if (!starts_with(s, prefix)) { *ok = false; return 0.0f; }
    s += strlen(prefix);
    while (*s == ' ') s++;
    if (*s == '\0') { *ok = false; return 0.0f; }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s)              { *ok = false; return 0.0f; }
    /* Reject trailing garbage (e.g. "1.5abc"). */
    while (*end == ' ') end++;
    if (*end != '\0')          { *ok = false; return 0.0f; }
    /* Reject nan / inf. */
    if (!isfinite(v))          { *ok = false; return 0.0f; }
    *ok = true;
    return v;
}

static void print_status(void)
{
    uint32_t rpm = HallSensor_CalculateRpm();
    int32_t  tcmd= SpeedPI_GetRawTargetRpm();
    float    trmp= SpeedPI_GetRampedTargetRpm();
    float    frpm= HallSensor_GetFilteredRpm();
    uint8_t  hall= HallSensor_GetStableRaw();
    uint8_t  cand= HallSensor_GetCandidateRaw();
    uint8_t  candc= HallSensor_GetCandidateCount();
    float    kp, ki;
    SpeedPI_GetGains(&kp, &ki);
    uint8_t blo, bmid, bhi;
    SpeedPI_GetBasePwm(&blo, &bmid, &bhi);
    uint8_t stlo, stmid, sthi;
    uint16_t stms;
    SpeedPI_GetBoostPwm(&stlo, &stmid, &sthi, &stms);
    float rup, rdown;
    SpeedPI_GetRampRates(&rup, &rdown);

    UartProtocol_Print("\r\n--- STATUS ---");
    UartProtocol_Printf("\r\nMode: %s", s_app.mode == MODE_SPEED ? "SPEED" : "DUTY");
    UartProtocol_Printf("\r\nPhase: %d", (int)s_app.phase);
    UartProtocol_Printf("\r\nDir: %s",
        s_app.direction > 0 ? "FWD" : (s_app.direction < 0 ? "REV" : "STOP"));
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
    UartProtocol_Printf("\r\nBase %u/%u/%u", (unsigned)blo, (unsigned)bmid, (unsigned)bhi);
    UartProtocol_Printf("\r\nBoost %u/%u/%u ms=%u", (unsigned)stlo, (unsigned)stmid, (unsigned)sthi, (unsigned)stms);
    UartProtocol_Printf("\r\nRamp up=%ld down=%ld", (long)rup, (long)rdown);
    UartProtocol_Printf("\r\nDuty: target=%u current=%u default=%u kick=%u/%u ramp_step=%u/%u",
        (unsigned)s_app.target_duty, (unsigned)s_app.current_duty,
        (unsigned)s_app.default_pwm,
        (unsigned)s_app.kick_duty, (unsigned)s_app.kick_ms,
        (unsigned)s_app.ramp_step, (unsigned)s_app.ramp_interval_ms);
    UartProtocol_Printf("\r\nFault: %s", FaultManager_GetName(FaultManager_GetLast()));
    UartProtocol_Printf("\r\nHallFault: %d  TXDrops: %lu",
        (int)HallSensor_GetFault(),
        (unsigned long)UartProtocol_GetTxDropCount());
    UartProtocol_PrintNewline();
}

static void print_help(void)
{
    UartProtocol_Print(
        "\r\n============================"
        "\r\n f/forward  |  f<0-250>"
        "\r\n b/backward |  b<0-250>"
        "\r\n s/stop     (coast)"
        "\r\n pwm <0-250>"
        "\r\n mode duty"
        "\r\n mode speed"
        "\r\n rpm <signed>"
        "\r\n rpm        (status)"
        "\r\n pi <kp> <ki>"
        "\r\n base <low> <mid> <high>"
        "\r\n boost <low> <mid> <high> <ms>"
        "\r\n ramp <up> <down>"
        "\r\n spstat"
        "\r\n hall"
        "\r\n status"
        "\r\n map / save / reload / mapreset"
        "\r\n identify / scan / test"
        "\r\n gatetest <0-5> <1-100>  (motor disconnected only)"
        "\r\n clrerr"
        "\r\n dbg on/off | telper <ms>"
        "\r\n help / ?"
        "\r\n============================");
    UartProtocol_PrintNewline();
}

static void print_hall_map(void)
{
    uint8_t map[8];
    Commutation_GetMap(map);
    UartProtocol_Print("\r\n[MAP] ");
    for (uint8_t i = 0; i < 8; i++) {
        UartProtocol_Printf("%u ", (unsigned)map[i]);
    }
    UartProtocol_PrintNewline();
}

static void handle_command(char *cmd, UartSource src)
{
    (void)src;
    trim_in_place(cmd);
    lower_in_place(cmd);
    if (cmd[0] == '\0') return;

    /* --- Motion commands (forward / backward / stop / pwm / f<n> / b<n>) ---
     * ISSUE-036: `f` / `b` without a value now use s_app.default_pwm
     * (set via `defpwm <n>`, default 100) instead of the hard-coded
     * 100.  This matches the legacy Arduino firmware where `controlPwmValue`
     * drove the bare `f`/`b` commands.  `f<n>`/`b<n>` still clamp to
     * 0..250 and reject negative values explicitly.
     *
     * ISSUE-045: heartbeat vs. new-motion distinction.  If the motor is
     * already PHASE_RUNNING in the same direction with the same target
     * duty, a repeat command is a heartbeat — just refresh
     * last_motor_cmd_ms.  Do NOT set run_request, duty_update_request,
     * or restart kick/ramp.  If the target duty changed, update
     * target_duty for the ramp to pick up on the next tick — but do
     * NOT set run_request (no phase_start_ms reset, no kick restart).
     * Only if the motor is stopped/braked/faulted does it set
     * run_request for the STOPPED→RUNNING transition. */
    if (strcmp(cmd, "f") == 0 || strcmp(cmd, "forward") == 0) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s_app.mode = MODE_DUTY;
        s_app.last_motor_cmd_ms = HAL_GetTick();
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD &&
            s_app.target_duty == s_app.default_pwm) {
            /* Heartbeat: already running forward at this duty. */
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Mid-run duty change: update target.  No kick restart,
             * no phase_start_ms reset.  If ramp is enabled, start
             * ramping from current_duty; otherwise apply immediately. */
            s_app.target_duty = s_app.default_pwm;
            if (s_app.ramp_enabled) {
                s_app.ramp_current_duty = s_app.current_duty;
                s_app.last_ramp_update_ms = HAL_GetTick();
            } else {
                apply_duty_now(s_app.default_pwm);
            }
            UartProtocol_Printf("\r\n[OK] FWD duty=%u", (unsigned)s_app.default_pwm);
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = s_app.default_pwm;
            begin_neutral_switch(+1);
            UartProtocol_Print("\r\n[OK] FWD (neutral switch)");
            return;
        }
        /* New motion (STOPPED→RUNNING). */
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.pending_direction = +1;
        s_app.direction = DIR_FWD;
        s_app.target_duty = s_app.default_pwm;
        UartProtocol_Printf("\r\n[OK] Run FWD D=%u", (unsigned)s_app.default_pwm);
        return;
    }
    if (strcmp(cmd, "b") == 0 || strcmp(cmd, "backward") == 0) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s_app.mode = MODE_DUTY;
        s_app.last_motor_cmd_ms = HAL_GetTick();
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV &&
            s_app.target_duty == s_app.default_pwm) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Mid-run duty change — same logic as FWD path above. */
            s_app.target_duty = s_app.default_pwm;
            if (s_app.ramp_enabled) {
                s_app.ramp_current_duty = s_app.current_duty;
                s_app.last_ramp_update_ms = HAL_GetTick();
            } else {
                apply_duty_now(s_app.default_pwm);
            }
            UartProtocol_Printf("\r\n[OK] REV duty=%u", (unsigned)s_app.default_pwm);
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = s_app.default_pwm;
            begin_neutral_switch(-1);
            UartProtocol_Print("\r\n[OK] REV (neutral switch)");
            return;
        }
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.pending_direction = -1;
        s_app.direction = DIR_REV;
        s_app.target_duty = s_app.default_pwm;
        UartProtocol_Printf("\r\n[OK] Run REV D=%u", (unsigned)s_app.default_pwm);
        return;
    }
    if (cmd[0] == 'f' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return; }
        if (d < 0)   d = 0;
        if (d > 250) d = 250;
        s_app.mode = MODE_DUTY;
        s_app.last_motor_cmd_ms = HAL_GetTick();
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD &&
            s_app.target_duty == (uint8_t)d) {
            UartProtocol_Print("\r\n[OK] FWD heartbeat");
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Mid-run duty change — same logic as bare-f path. */
            s_app.target_duty = (uint8_t)d;
            if (s_app.ramp_enabled) {
                s_app.ramp_current_duty = s_app.current_duty;
                s_app.last_ramp_update_ms = HAL_GetTick();
            } else {
                apply_duty_now((uint8_t)d);
            }
            UartProtocol_Printf("\r\n[OK] FWD duty=%lu", (unsigned long)d);
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = (uint8_t)d;
            begin_neutral_switch(+1);
            UartProtocol_Printf("\r\n[OK] FWD D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.pending_direction = +1;
        s_app.direction = DIR_FWD;
        s_app.target_duty = (uint8_t)d;
        UartProtocol_Printf("\r\n[OK] FWD D=%lu", (unsigned long)d);
        return;
    }
    if (cmd[0] == 'b' && cmd[1] >= '0' && cmd[1] <= '9') {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        char *end = NULL;
        long d = strtol(cmd + 1, &end, 10);
        if (*end != '\0') { UartProtocol_Print("\r\n[ERR] Bad duty"); return; }
        if (d < 0)   d = 0;
        if (d > 250) d = 250;
        s_app.mode = MODE_DUTY;
        s_app.last_motor_cmd_ms = HAL_GetTick();
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV &&
            s_app.target_duty == (uint8_t)d) {
            UartProtocol_Print("\r\n[OK] REV heartbeat");
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Mid-run duty change — same logic as bare-b path. */
            s_app.target_duty = (uint8_t)d;
            if (s_app.ramp_enabled) {
                s_app.ramp_current_duty = s_app.current_duty;
                s_app.last_ramp_update_ms = HAL_GetTick();
            } else {
                apply_duty_now((uint8_t)d);
            }
            UartProtocol_Printf("\r\n[OK] REV duty=%lu", (unsigned long)d);
            return;
        }
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = (uint8_t)d;
            begin_neutral_switch(-1);
            UartProtocol_Printf("\r\n[OK] REV D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.pending_direction = -1;
        s_app.direction = DIR_REV;
        s_app.target_duty = (uint8_t)d;
        UartProtocol_Printf("\r\n[OK] REV D=%lu", (unsigned long)d);
        return;
    }
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s_app.run_request = false;
        s_app.stop_request = true;
        s_app.duty_update_request = false;
        s_app.last_motor_cmd_ms = 0U;
        UartProtocol_Print("\r\n[OK] Stop");
        return;
    }
    if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        enter_brake();
        s_app.last_motor_cmd_ms = 0U;
        UartProtocol_Print("\r\n[OK] Brake (coast in first bring-up)");
        return;
    }

    /* --- pwm query/set ---
     * `pwm <n>` sets the target duty for mid-run duty updates.
     * It does NOT change the bare `f`/`b` default (that is `defpwm <n>`).
     * When stopped, `pwm <n>` just records the target duty for the
     * next run request — it must NOT arm the 800 ms watchdog.
     * When running, it triggers a mid-run duty change (ramp or immediate).
     *
     * Summary:
     *   `defpwm <n>`  — changes bare `f`/`b` default PWM
     *   `pwm <n>`     — sets target duty (mid-run update or pre-set)
     *   `f<n>`/`b<n>` — runs with specific duty n (0..250)
     */
    if (strcmp(cmd, "pwm") == 0) {
        UartProtocol_Printf("\r\n[INFO] TargetDuty=%u CurrentDuty=%u",
                            (unsigned)s_app.target_duty, (unsigned)s_app.current_duty);
        return;
    }
    bool ok = false;
    long v = parse_long_after(cmd, "pwm ", &ok);
    if (ok) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (v < 0)   v = 0;
        if (v > 250) v = 250;
        if (SpeedPI_IsEnabled()) SpeedPI_Disable();
        s_app.mode = MODE_DUTY;
        s_app.target_duty = (uint8_t)v;
        /* ISSUE-043/045: Only refresh the command watchdog and set
         * duty_update_request if the motor is already running.  When
         * stopped, `pwm <n>` just records the target duty for the
         * next `f`/`b`/`f<n>` — it must NOT arm the 800 ms watchdog
         * or leave a stale duty_update_request that would kick/ramp
         * after the next run request. */
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            s_app.duty_update_request = true;
            s_app.last_motor_cmd_ms = HAL_GetTick();
        }
        UartProtocol_Printf("\r\n[OK] TargetDuty=%lu", (unsigned long)v);
        return;
    }

    /* --- mode duty / mode speed (with legacy aliases) ---
     * ISSUE-037: accept `mode normal` (legacy Arduino for duty mode)
     * and `mode control` (legacy Arduino for speed mode) so old H7 /
     * tools scripts keep working.  They map to the cube modes:
     *   mode normal  -> MODE_DUTY  (manual PWM, no PI)
     *   mode control -> MODE_SPEED (RPM PI control)
     * `mode duty` / `mode speed` remain the canonical names. */
    if (strcmp(cmd, "mode") == 0) {
        UartProtocol_Printf("\r\n[INFO] Mode=%s",
                            s_app.mode == MODE_SPEED ? "SPEED" : "DUTY");
        return;
    }
    if (strcmp(cmd, "mode duty") == 0 || strcmp(cmd, "pid off") == 0 ||
        strcmp(cmd, "mode normal") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        SpeedPI_Disable();
        s_app.mode = MODE_DUTY;
        UartProtocol_Print("\r\n[OK] Mode=DUTY");
        return;
    }
    if (strcmp(cmd, "mode speed") == 0 || strcmp(cmd, "pid on") == 0 ||
        strcmp(cmd, "mode control") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        SpeedPI_Enable();
        s_app.mode = MODE_SPEED;
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
    v = parse_long_after(cmd, "rpm ", &ok);
    if (ok) {
        if (v != 0 && service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (v == 0) {
            if (SpeedPI_IsEnabled()) SpeedPI_Disable();
            stop_immediate();
            UartProtocol_Print("\r\n[OK] RPM=0 stop");
            return;
        }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (v > MAX_RPM_TARGET)  v = MAX_RPM_TARGET;
        if (v < -MAX_RPM_TARGET) v = -MAX_RPM_TARGET;

        int8_t new_dir = (v > 0) ? +1 : -1;
        /* Capture the real running direction BEFORE overwriting
         * s_app.direction.  In duty mode SpeedPI_GetRawTargetRpm()
         * may be 0, so use s_app.direction as the authoritative
         * source of the current physical direction. */
        int8_t old_dir = (int8_t)s_app.direction;

        if (!SpeedPI_IsEnabled()) {
            SpeedPI_Enable();
            s_app.mode = MODE_SPEED;
        }
        SpeedPI_SetTargetRpm((int32_t)v);
        s_app.last_motor_cmd_ms = HAL_GetTick();

        if (s_app.phase == PHASE_RUNNING && old_dir != 0 && new_dir != old_dir) {
            /* Direction reversal while running — neutral switch.
             * s_app.direction is set by begin_neutral_switch via
             * pending_direction after the neutral period. */
            begin_neutral_switch(new_dir);
        } else if (s_app.phase == PHASE_RUNNING && new_dir == old_dir) {
            /* Already running same direction — heartbeat. */
            s_app.direction = (v > 0) ? DIR_FWD : DIR_REV;
        } else {
            /* Stopped / not yet running — request start. */
            s_app.direction = (v > 0) ? DIR_FWD : DIR_REV;
            s_app.run_request = true;
            s_app.stop_request = false;
        }
        UartProtocol_Printf("\r\n[OK] RPM=%ld", (long)v);
        return;
    }

    /* --- pi <kp> <ki> --- */
    if (starts_with(cmd, "pi ")) {
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
        /* Reject trailing garbage after second value. */
        while (*end2 == ' ') end2++;
        if (*end2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        SpeedPI_SetKp(kp);
        SpeedPI_SetKi(ki);
        UartProtocol_Printf("\r\n[OK] Kp_m=%ld Ki_m=%ld",
            (long)(kp * 1000.0f), (long)(ki * 1000.0f));
        return;
    }

    /* --- kp / ki standalone (compat) --- */
    float fv = parse_float_after(cmd, "kp ", &ok);
    if (ok) { SpeedPI_SetKp(fv); UartProtocol_Printf("\r\n[OK] Kp_m=%ld", (long)(fv * 1000.0f)); return; }
    fv = parse_float_after(cmd, "ki ", &ok);
    if (ok) { SpeedPI_SetKi(fv); UartProtocol_Printf("\r\n[OK] Ki_m=%ld", (long)(fv * 1000.0f)); return; }

    /* --- base <lo> <mid> <hi> ---
     * Each value is clamped to 0..250 (PWM duty range).  Negative
     * or non-numeric tokens are rejected with [ERR].
     * SpeedPI_SetBasePwm() further clamps to SPEED_PI_MAX_PWM;
     * the reply prints the actual stored values. */
    if (starts_with(cmd, "base ")) {
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
        /* Reject trailing garbage after last argument. */
        while (*e3 == ' ') e3++;
        if (*e3 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        if (lo < 0) lo = 0;
        if (lo > 250) lo = 250;
        if (mid < 0) mid = 0;
        if (mid > 250) mid = 250;
        if (hi < 0) hi = 0;
        if (hi > 250) hi = 250;
        SpeedPI_SetBasePwm((uint8_t)lo, (uint8_t)mid, (uint8_t)hi);
        /* Print actual stored values (may be further clamped by
         * SPEED_PI_MAX_PWM inside SpeedPI_SetBasePwm). */
        uint8_t alo, amid, ahi;
        SpeedPI_GetBasePwm(&alo, &amid, &ahi);
        UartProtocol_Printf("\r\n[OK] Base L=%u M=%u H=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi);
        return;
    }

    /* --- boost <lo> <mid> <hi> <ms> ---
     * PWM values clamped to 0..250; ms clamped to 0..1000.
     * SpeedPI_SetBoostPwm() further clamps to SPEED_PI_MAX_PWM;
     * the reply prints the actual stored values. */
    if (starts_with(cmd, "boost ")) {
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
        /* Reject trailing garbage after last argument. */
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
        /* Print actual stored values (may be further clamped by
         * SPEED_PI_MAX_PWM inside SpeedPI_SetBoostPwm). */
        uint8_t alo, amid, ahi;
        uint16_t ams;
        SpeedPI_GetBoostPwm(&alo, &amid, &ahi, &ams);
        UartProtocol_Printf("\r\n[OK] Boost L=%u M=%u H=%u ms=%u",
                            (unsigned)alo, (unsigned)amid, (unsigned)ahi, (unsigned)ams);
        return;
    }

    /* --- ramp <up> <down> --- */
    if (starts_with(cmd, "ramp ")) {
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
        /* Reject trailing garbage after second value. */
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
            s_app.mode == MODE_SPEED ? "SPEED" : "DUTY",
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

    /* --- map / save / reload / mapreset --- */
    if (strcmp(cmd, "map") == 0)         { print_hall_map(); return; }
    if (strcmp(cmd, "mapreset") == 0)    {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        Commutation_LoadDefaultMap();
        UartProtocol_Print("\r\n[OK] Default map loaded");
        print_hall_map();
        return;
    }
    if (strcmp(cmd, "save") == 0) {
        UartProtocol_Print("\r\n[ERR] Flash storage disabled until safe implementation");
        return;
    }
    if (strcmp(cmd, "reload") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        uint8_t map[8];
        if (Storage_LoadHallMap(map)) {
            bool all_ok = true;
            for (uint8_t i = 0; i < 8; i++) {
                if (!Commutation_SetMapEntry(i, map[i])) { all_ok = false; break; }
            }
            if (all_ok) {
                UartProtocol_Print("\r\n[OK] Hall map loaded from flash");
            } else {
                Commutation_LoadDefaultMap();
                UartProtocol_Print("\r\n[ERR] Flash map entry rejected, defaults restored");
            }
        } else {
            Commutation_LoadDefaultMap();
            UartProtocol_Print("\r\n[INFO] No saved map, defaults loaded");
        }
        print_hall_map();
        return;
    }

    /* --- identify / scan / test --- */
    if (strcmp(cmd, "identify") == 0) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        ServiceTask_Request(SVC_IDENTIFY);
        return;
    }
    if (strcmp(cmd, "scan") == 0) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        ServiceTask_Request(SVC_SCAN);
        return;
    }
    if (strcmp(cmd, "test") == 0) {
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        ServiceTask_Request(SVC_TEST);
        return;
    }

    /* --- gatetest <sector> <duty> ---
     * Motor-disconnected-only scope verification.
     * Applies one sector at low duty for a short timeout. */
    if (starts_with(cmd, "gatetest ")) {
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return;
        }
        if (ServiceTask_IsActive()) {
            UartProtocol_Print("\r\n[ERR] Service task active");
            return;
        }
        const char *p = cmd + 9;
        while (*p == ' ') p++;
        char *ge1 = NULL;
        long sector = strtol(p, &ge1, 10);
        if (ge1 == p) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-100>"); return; }
        while (*ge1 == ' ') ge1++;
        char *ge2 = NULL;
        long duty = strtol(ge1, &ge2, 10);
        if (ge2 == ge1) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-100>"); return; }
        /* Reject trailing garbage. */
        while (*ge2 == ' ') ge2++;
        if (*ge2 != '\0') { UartProtocol_Print("\r\n[ERR] Trailing garbage"); return; }
        if (sector < 0 || sector > 5 || duty < 1 || duty > 100) {
            UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-100>");
            return;
        }
        s_app.gatetest_active   = true;
        s_app.gatetest_sector   = (uint8_t)sector;
        s_app.gatetest_duty     = (uint8_t)duty;
        s_app.gatetest_start_ms = HAL_GetTick();
        s_app.gatetest_timeout_ms = 500U;
        MotorDriver_ApplyStep((uint8_t)sector, +1, (uint8_t)duty);
        UartProtocol_Printf("\r\n[OK] Gate test sector=%lu duty=%lu timeout=500ms",
                            (unsigned long)sector, (unsigned long)duty);
        UartProtocol_Print("\r\n[WARN] Motor must be disconnected!");
        return;
    }
    /* --- savecfg / loadcfg / defaults / saveall --- */
    if (strcmp(cmd, "savecfg") == 0) {
        UartProtocol_Print("\r\n[ERR] Flash storage disabled until safe implementation");
        return;
    }
    if (strcmp(cmd, "loadcfg") == 0) {
        uint8_t kd, rs, dp;
        uint16_t km, ri, bh;
        if (Storage_LoadConfig(&kd, &km, &rs, &ri, &dp, &bh)) {
            s_app.kick_duty = kd; s_app.kick_ms = km;
            s_app.ramp_step = rs; s_app.ramp_interval_ms = ri;
            s_app.default_pwm = dp; s_app.brake_hold_ms = bh;
            clamp_loaded_config();
            UartProtocol_Print("\r\n[OK] Config loaded");
        } else {
            UartProtocol_Print("\r\n[INFO] No saved config, defaults kept");
        }
        return;
    }
    if (strcmp(cmd, "defaults") == 0) {
        /* ISSUE-044: keep the safe bring-up defaults (kick off). */
        s_app.kick_enabled = false;
        s_app.ramp_enabled = true;
        s_app.kick_duty = 60;
        s_app.kick_ms = 50;
        s_app.ramp_step = 8;
        s_app.ramp_interval_ms = 5;
        s_app.default_pwm = 100;
        s_app.brake_hold_ms = BRAKE_HOLD_MS;
        UartProtocol_Print("\r\n[OK] Defaults loaded into RAM (kick OFF, ramp ON)");
        return;
    }
    if (strcmp(cmd, "saveall") == 0) {
        UartProtocol_Print("\r\n[ERR] Flash storage disabled until safe implementation");
        return;
    }

    /* --- kick / ramp config commands --- */
    if (strcmp(cmd, "kick on") == 0)  { s_app.kick_enabled = true;  UartProtocol_Print("\r\n[OK] Kick ON");  return; }
    if (strcmp(cmd, "kick off") == 0) { s_app.kick_enabled = false; UartProtocol_Print("\r\n[OK] Kick OFF"); return; }
    if (strcmp(cmd, "ramp on") == 0)  { s_app.ramp_enabled = true;  UartProtocol_Print("\r\n[OK] Ramp ON");  return; }
    if (strcmp(cmd, "ramp off") == 0) { s_app.ramp_enabled = false; UartProtocol_Print("\r\n[OK] Ramp OFF"); return; }

    /* --- kick / ramp config commands ---
     * ISSUE-038: range-check all numeric arguments so negative or
     * huge values do not wrap into uint8_t / uint16_t silently. */
    v = parse_long_after(cmd, "kickduty ", &ok);
    if (ok) {
        if (v < 0) v = 0;
        if (v > KICK_DUTY_MAX) v = KICK_DUTY_MAX;
        s_app.kick_duty = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] KickDuty=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "kickms ", &ok);
    if (ok) {
        if (v < 0) v = 0;
        if (v > KICK_MS_MAX) v = KICK_MS_MAX;
        s_app.kick_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] KickMs=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "ramprate ", &ok);
    if (ok) {
        if (v < 0) v = 0;
        if (v > RAMP_STEP_MAX) v = RAMP_STEP_MAX;
        s_app.ramp_step = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] RampStep=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "rampms ", &ok);
    if (ok) {
        if (v < 0) v = 0;
        if (v > RAMP_INTERVAL_MS_MAX) v = RAMP_INTERVAL_MS_MAX;
        s_app.ramp_interval_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] RampMs=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "defpwm ", &ok);
    if (ok) {
        if (v < 0) v = 0;
        if (v > DEFAULT_PWM_MAX) v = DEFAULT_PWM_MAX;
        s_app.default_pwm = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] DefaultPWM=%lu", (unsigned long)v);
        return;
    }

    /* --- status --- */
    if (strcmp(cmd, "status") == 0) { print_status(); return; }

    /* --- clrerr ---
     * Clear faults AND force app state to safe STOPPED so the motor
     * cannot auto-restart on a stale run_request or phase. */
    if (strcmp(cmd, "clrerr") == 0) {
        FaultManager_Clear();
        HallSensor_ClearFault();
        s_app.queue_overflow = false;
        UartProtocol_ResetTxDropCount();
        UartProtocol_ResetCmdDropCount();
        s_app.phase = PHASE_STOPPED;
        s_app.direction = (Direction)0;
        s_app.target_duty = 0U;
        s_app.current_duty = 0U;
        s_app.run_request = false;
        s_app.stop_request = false;
        s_app.duty_update_request = false;
        s_app.last_motor_cmd_ms = 0U;
        s_app.has_ever_run = false;
        s_app.last_edge_count = HallSensor_GetEdgeCounter();
        s_app.last_edge_ms = 0U;
        s_app.kick_active = false;
        s_app.kick_start_ms = 0U;
        s_app.ramp_current_duty = 0U;
        s_app.last_ramp_update_ms = 0U;
        MotorDriver_AllOff();
        SpeedPI_Disable();
        UartProtocol_Print("\r\n[OK] Errors cleared, motor stopped");
        return;
    }

    /* --- debug --- */
    if (strcmp(cmd, "debug on") == 0)  { s_app.verboseDebug = true;  UartProtocol_Print("\r\n[OK] Debug ON");  return; }
    if (strcmp(cmd, "debug off") == 0) { s_app.verboseDebug = false; UartProtocol_Print("\r\n[OK] Debug OFF"); return; }

    /* --- telemetry --- */
    if (strcmp(cmd, "dbg on") == 0)  { Telemetry_SetMode(TELEMETRY_DEBUG);   UartProtocol_Print("\r\n[OK] Telemetry DBG ON");  return; }
    if (strcmp(cmd, "dbg off") == 0) { Telemetry_SetMode(TELEMETRY_COMPACT); UartProtocol_Print("\r\n[OK] Telemetry DBG OFF"); return; }
    v = parse_long_after(cmd, "telper ", &ok);
    if (ok) {
        if (v < 10) v = 10;
        if (v > 60000) v = 60000;
        Telemetry_SetIntervalMs((uint32_t)v);
        UartProtocol_Printf("\r\n[OK] Telemetry=%lu ms", (unsigned long)v);
        return;
    }

    /* --- help --- */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) { print_help(); return; }

    UartProtocol_Print("\r\n[ERR] Unknown command");
}

/* ----------------------------------------------------------------
 * Service loop helpers
 * ---------------------------------------------------------------- */

static void service_motor(void)
{
    /* ISSUE-024: Do not let service_motor() clobber the gate
     * outputs that ServiceTask_Update() (test/identify) or the
     * gatetest handler just applied.  Both run earlier in App_Loop
     * and drive the gates directly while the motor is stopped; the
     * `else { MotorDriver_AllOff(); }` branch below would
     * immediately extinguish them.  Early-return and leave the
     * service/gate-test outputs untouched. */
    if (s_app.gatetest_active) return;
    if (ServiceTask_IsActive()) return;

    /* Central fault guard: if any fault is latched, force the motor
     * to a safe state immediately.  This catches faults raised from
     * ISR context (e.g. App_Tim1BrkIsr) or from any other path where
     * the app state was not explicitly brought to STOPPED/FAULT. */
    if (FaultManager_GetLast() != FAULT_NONE) {
        MotorDriver_AllOff();
        SpeedPI_Disable();
        s_app.phase = PHASE_FAULT;
        s_app.direction = (Direction)0;
        s_app.target_duty = 0U;
        s_app.current_duty = 0U;
        s_app.run_request = false;
        s_app.stop_request = false;
        s_app.duty_update_request = false;
        s_app.last_motor_cmd_ms = 0U;
        s_app.kick_active = false;
        s_app.kick_start_ms = 0U;
        s_app.ramp_current_duty = 0U;
        s_app.last_ramp_update_ms = 0U;
        return;
    }

    /* ISSUE-010: Surface SpeedPI internal faults through the central
     * FaultManager so telemetry FC and the safety path react. */
    if (SpeedPI_IsEnabled() && SpeedPI_GetFault() == SPD_FAULT_NO_HALL) {
        FaultManager_Raise(FAULT_NO_HALL);
        stop_immediate();
        UartProtocol_Print("\r\n[ERR] SpeedPI: no Hall feedback");
        return;
    }

    /* ISSUE-039: Surface Hall-side faults (invalid 0b000/0b111
     * persisted > INVALID_HALL_STOP_US, or illegal-transition count
     * exceeded) through the central FaultManager.  Only raise while
     * the motor is actually running — when stopped, invalid Hall is
     * expected (motor not turning).  No current sense is involved;
     * this is purely Hall-pattern based. */
    if (s_app.phase == PHASE_RUNNING) {
        HallFault hf = HallSensor_GetFault();
        if (hf == HALL_FAULT_INVALID_PERSIST) {
            FaultManager_Raise(FAULT_INVALID_HALL);
            stop_immediate();
            UartProtocol_Print("\r\n[ERR] Invalid Hall persisted");
            return;
        }
        if (hf == HALL_FAULT_ILLEGAL_TRANSITION) {
            FaultManager_Raise(FAULT_ILLEGAL_TRANSITION);
            stop_immediate();
            UartProtocol_Print("\r\n[ERR] Illegal Hall transition spam");
            return;
        }
    }

    /* Apply deferred requests. */
    if (s_app.stop_request) {
        stop_immediate();
        s_app.stop_request = false;
    }
    if (s_app.duty_update_request) {
        /* ISSUE-045: duty_update_request is only set by `pwm <n>`
         * (not by f/b/f<n>/b<n> — those handle duty changes inline
         * in the command handler).  When the motor is already running,
         * this is a mid-run duty change: update target_duty and start
         * a ramp from current_duty toward the new target.  No kick
         * restart.  When stopped, just store target_duty for the next
         * run request — do not apply duty or start kick/ramp. */
        if (!SpeedPI_IsEnabled() &&
            (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL)) {
            /* Mid-run duty change: ramp from current duty to new
             * target.  If ramp is disabled, apply immediately.
             * Kick is NOT restarted. */
            if (s_app.ramp_enabled) {
                s_app.ramp_current_duty = s_app.current_duty;
                s_app.last_ramp_update_ms = HAL_GetTick();
            } else {
                s_app.ramp_current_duty = s_app.target_duty;
                s_app.current_duty = s_app.target_duty;
                apply_duty_now(s_app.target_duty);
            }
        }
        s_app.duty_update_request = false;
    }
    if (s_app.run_request) {
        /* Resolve the requested direction for both duty and speed modes
         * so a non-zero `rpm` command actually enters PHASE_RUNNING
         * (ISSUE-008). */
        int8_t req_dir;
        if (s_app.mode == MODE_SPEED) {
            int32_t t = SpeedPI_GetRawTargetRpm();
            req_dir = (t > 0) ? +1 : (t < 0 ? -1 : 0);
        } else {
            req_dir = (s_app.direction > 0) ? +1 : ((s_app.direction < 0) ? -1 : 0);
        }

        /* ISSUE-045: distinguish three cases:
         *   1. RUNNING + same direction → heartbeat only, no state change.
         *      Do NOT update phase_start_ms (the Hall startup timeout
         *      must not be refreshed by heartbeat commands).
         *      Do NOT restart kick/ramp.
         *   2. RUNNING + opposite direction → begin_neutral_switch.
         *   3. STOPPED/BRAKE/FAULT → RUNNING: real transition,
         *      phase_start_ms stamped, kick/ramp armed (just_started). */
        if (req_dir != 0 &&
            s_app.phase == PHASE_RUNNING && s_app.direction != 0 &&
            s_app.direction != (Direction)req_dir) {
            /* Case 2: direction reversal while running. */
            begin_neutral_switch(req_dir);
        } else if (req_dir != 0 &&
                   s_app.phase == PHASE_RUNNING &&
                   s_app.direction == (Direction)req_dir) {
            /* Case 1: already running in the same direction — heartbeat
             * only.  The command handler already refreshed
             * last_motor_cmd_ms.  Do nothing here; the motor continues
             * with its existing kick/ramp/PI state.  phase_start_ms is
             * NOT reset so the Hall startup timeout keeps counting from
             * the real start of motion. */
        } else if (req_dir != 0 && s_app.phase != PHASE_NEUTRAL) {
            /* Case 3: STOPPED/BRAKE/FAULT → RUNNING.  This is a real
             * transition.  Stamp phase_start_ms and arm kick/ramp. */
            s_app.phase = PHASE_RUNNING;
            s_app.direction = (Direction)req_dir;
            s_app.phase_start_ms = HAL_GetTick();
            /* Reset Hall edge baseline so each start gets its own
             * startup timeout window.  Without this, stale edges from
             * a previous run or manual wheel rotation could bypass
             * the START_NO_HALL_TIMEOUT_MS check. */
            s_app.has_ever_run = false;
            s_app.last_edge_count = HallSensor_GetEdgeCounter();
            s_app.last_edge_ms = HAL_GetTick();
            /* Arm kick/ramp only on the real STOPPED→RUNNING transition.
             * This is the sole place kick is started; heartbeat commands
             * and mid-run duty changes do not reach here. */
            if (!SpeedPI_IsEnabled()) {
                if (s_app.kick_enabled && s_app.target_duty > 0U) {
                    s_app.kick_active = true;
                    s_app.kick_start_ms = HAL_GetTick();
                    s_app.ramp_current_duty = s_app.kick_duty;
                } else {
                    s_app.kick_active = false;
                    if (s_app.ramp_enabled) {
                        s_app.ramp_current_duty = 0U;
                    } else {
                        s_app.ramp_current_duty = s_app.target_duty;
                    }
                }
                s_app.current_duty = s_app.ramp_current_duty;
                apply_duty_now(s_app.ramp_current_duty);
                s_app.last_ramp_update_ms = HAL_GetTick();
            }
        }
        s_app.run_request = false;
    }

    /* Neutral-wait phase */
    if (s_app.phase == PHASE_NEUTRAL) {
        MotorDriver_AllOff();
        if ((HAL_GetTick() - (s_app.neutral_release_ms - DIRECTION_NEUTRAL_MS)) >= DIRECTION_NEUTRAL_MS) {
            s_app.direction = (Direction)s_app.pending_direction;
            s_app.phase = PHASE_RUNNING;
            s_app.phase_start_ms = HAL_GetTick();
            /* Reset Hall edge baseline after direction reversal.
             * Without this, stale edges from the previous direction
             * could bypass the startup timeout or duty Hall loss check. */
            s_app.has_ever_run = false;
            s_app.last_edge_count = HallSensor_GetEdgeCounter();
            s_app.last_edge_ms = HAL_GetTick();
        }
        return;
    }

    /* Brake timeout. */
    if (s_app.phase == PHASE_BRAKE &&
        (HAL_GetTick() - s_app.phase_start_ms) >= s_app.brake_hold_ms) {
        stop_immediate();
        UartProtocol_Print("\r\n[WARN] Brake timeout");
    }

    /* SpeedPI computed duty only used in speed mode. */
    uint8_t apply_duty = s_app.current_duty;
    if (SpeedPI_IsEnabled()) {
        apply_duty = SpeedPI_GetComputedDuty();
        s_app.current_duty = apply_duty;
        /* ISSUE-B: Do NOT refresh last_motor_cmd_ms here.  Speed mode
         * now requires real command heartbeat (rpm <signed>) just like
         * duty mode.  A single rpm 23 must not keep the motor running
         * indefinitely — the H7/terminal must keep sending commands. */
    } else if (s_app.phase == PHASE_RUNNING) {
        /* ISSUE-038: Duty-mode kick/ramp progression.  The kick pulse
         * is time-bounded; after it expires, ramp toward target_duty
         * in ramp_step increments every ramp_interval_ms.  When
         * ramp_enabled is false the duty was already set directly in
         * the duty_update_request handler above. */
        uint32_t now = HAL_GetTick();
        if (s_app.kick_active) {
            if ((now - s_app.kick_start_ms) >= s_app.kick_ms) {
                s_app.kick_active = false;
                /* Kick expired — start ramping from the kick duty
                 * down/up toward target_duty.  If ramp is disabled,
                 * jump straight to target. */
                if (s_app.ramp_enabled) {
                    s_app.ramp_current_duty = s_app.kick_duty;
                    s_app.last_ramp_update_ms = now;
                } else {
                    s_app.ramp_current_duty = s_app.target_duty;
                }
                s_app.current_duty = s_app.ramp_current_duty;
                apply_duty_now(s_app.ramp_current_duty);
            }
            /* While kick is active, apply_duty stays at kick_duty. */
            apply_duty = s_app.current_duty;
        } else if (s_app.ramp_enabled &&
                   s_app.ramp_current_duty != s_app.target_duty &&
                   s_app.ramp_interval_ms > 0U &&
                   (now - s_app.last_ramp_update_ms) >= s_app.ramp_interval_ms) {
            s_app.last_ramp_update_ms = now;
            if (s_app.ramp_current_duty < s_app.target_duty) {
                uint16_t delta = (uint16_t)s_app.ramp_step;
                uint16_t gap = (uint16_t)(s_app.target_duty - s_app.ramp_current_duty);
                s_app.ramp_current_duty = (delta >= gap)
                    ? s_app.target_duty
                    : (uint8_t)(s_app.ramp_current_duty + delta);
            } else {
                uint16_t delta = (uint16_t)s_app.ramp_step;
                uint16_t gap = (uint16_t)(s_app.ramp_current_duty - s_app.target_duty);
                s_app.ramp_current_duty = (delta >= gap)
                    ? s_app.target_duty
                    : (uint8_t)(s_app.ramp_current_duty - delta);
            }
            s_app.current_duty = s_app.ramp_current_duty;
            apply_duty = s_app.current_duty;
            apply_duty_now(s_app.ramp_current_duty);
        } else {
            apply_duty = s_app.current_duty;
        }
    }

    /* Decide what to actually drive. */
    if (s_app.phase == PHASE_RUNNING) {
        /* If we have a valid Hall state, drive the matching sector.
         * If not, hold the last driven state briefly so that
         * transient 0b000/0b111 glitches do not stall the motor. */
        uint8_t state = HallSensor_GetMappedState();
        if (state == 0xFFU) {
            state = HallSensor_GetLastDrivenState();
        }
        if (state <= 5U) {
            MotorDriver_ApplyStep(state, (int8_t)s_app.direction, apply_duty);
        } else {
            MotorDriver_AllOff();
        }
    } else if (s_app.phase == PHASE_BRAKE) {
        MotorDriver_AllOff();   /* coast during brake for first bring-up */
    } else {
        MotorDriver_AllOff();
    }

    /* Hall freshness / startup timeout (ISSUE-009).
     *
     * has_ever_run is set true the first time a Hall edge is observed
     * while RUNNING.  Before that, the motor is allowed the full
     * START_NO_HALL_TIMEOUT_MS (700 ms) to produce its first edge —
     * previously the freshness check faulted immediately because
     * has_ever_run was never set. */
    if (s_app.phase == PHASE_RUNNING) {
        uint32_t edges = HallSensor_GetEdgeCounter();
        if (edges != s_app.last_edge_count) {
            s_app.last_edge_count = edges;
            s_app.has_ever_run = true;
            s_app.last_edge_ms = HAL_GetTick();
        }

        if (HallSensor_GetFreshness() == HALL_STALE) {
            if (!s_app.has_ever_run) {
                if ((HAL_GetTick() - s_app.phase_start_ms) > START_NO_HALL_TIMEOUT_MS) {
                    FaultManager_Raise(FAULT_NO_HALL);
                    stop_immediate();
                    UartProtocol_Print("\r\n[ERR] No Hall edge in startup window");
                }
            } else if (SpeedPI_IsEnabled() &&
                       SpeedPI_GetRawTargetRpm() != 0 &&
                       (HAL_GetTick() - s_app.last_motor_cmd_ms) > RPM_FEEDBACK_TIMEOUT_MS) {
                FaultManager_Raise(FAULT_NO_HALL);
                stop_immediate();
                UartProtocol_Print("\r\n[ERR] Hall lost in speed mode");
            } else if (!SpeedPI_IsEnabled() &&
                       s_app.target_duty > 0U &&
                       s_app.has_ever_run &&
                       (HAL_GetTick() - s_app.last_edge_ms) > DUTY_HALL_LOSS_TIMEOUT_MS) {
                FaultManager_Raise(FAULT_NO_HALL);
                stop_immediate();
                UartProtocol_Print("\r\n[ERR] Hall lost in duty mode");
            }
        }
    }
}

static void service_watchdogs(void)
{
    /* ISSUE-043: Only run the command watchdog while the motor is
     * actually active (RUNNING or NEUTRAL direction-switch).  When
     * stopped, `pwm <n>` legitimately just records a target duty
     * without starting motion — it must NOT arm the 800 ms watchdog
     * and produce a spurious FAULT_WATCHDOG.  The same applies to
     * `defpwm <n>` and other config commands.  Watchdog arming is
     * tied to motor motion, not to command presence.
     *
     * Host-disconnect is similarly only checked while the motor is
     * running; a stopped motor cannot "lose" a host. */
    if (s_app.phase != PHASE_RUNNING && s_app.phase != PHASE_NEUTRAL) {
        return;
    }

    uint32_t now = HAL_GetTick();
    if (s_app.last_motor_cmd_ms == 0U) return;
    if ((now - s_app.last_motor_cmd_ms) > CMD_WATCHDOG_MS) {
        stop_immediate();
        FaultManager_Raise(FAULT_WATCHDOG);
        UartProtocol_Print("\r\n[WARN] WD stop");
        s_app.last_motor_cmd_ms = 0U;
        return;
    }
    /* ISSUE-B: Host disconnect now applies to both duty and speed
     * modes.  Previously speed mode was exempt, meaning a single
     * rpm command could keep the motor running after H7/terminal
     * disconnects.  Now both modes require real command heartbeat. */
    if (s_app.phase == PHASE_RUNNING &&
        !UartProtocol_HasRecentActivity(now, HOST_DISCONNECT_TIMEOUT_MS)) {
        stop_immediate();
        FaultManager_Raise(FAULT_HOST_LOST);
        UartProtocol_Print("\r\n[WARN] Host disconnect stop");
        s_app.last_motor_cmd_ms = 0U;
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void App_Init(void)
{
    memset(&s_app, 0, sizeof(s_app));
    s_app.brake_hold_ms = BRAKE_HOLD_MS;
    s_app.target_duty = 0U;
    s_app.current_duty = 0U;
    s_app.mode = MODE_DUTY;
    /* ISSUE-044: kick is DISABLED by default for first bring-up.
     * The legacy Arduino firmware enabled a 225-duty kick pulse, but
     * that is too aggressive for a system with no current sense and
     * an unverified power stage — `f10` would apply duty 225 for
     * 50 ms before ramping down, potentially exceeding the PSU
     * current limit.  Enable kick explicitly with `kick on` (and a
     * conservative `kickduty`) only after low-duty tests pass.
     * See docs/BRINGUP.md Stage 4. */
    s_app.kick_enabled = false;
    s_app.ramp_enabled = true;
    s_app.kick_duty = 60;       /* conservative, only used if kick on */
    s_app.kick_ms = 50;
    s_app.ramp_step = 8;        /* small steps for gentle ramp */
    s_app.ramp_interval_ms = 5;
    s_app.default_pwm = 100;    /* safe default for bare f/b */

    /* Bring up the peripherals. */
    MotorDriver_Init();
    Commutation_LoadDefaultMap();
    HallSensor_Init();
    SpeedPI_Init();
    UartProtocol_Init();
    Telemetry_Init();
    FaultManager_Init();
    ServiceTask_Init();

    /* Load saved hall map from flash (if any) */
    {
        uint8_t map[8];
        if (Storage_LoadHallMap(map)) {
            for (uint8_t i = 0; i < 8; i++)
                Commutation_SetMapEntry(i, map[i]);
        }
    }

    /* Load saved config from flash (if any) */
    {
        uint8_t kd, rs, dp;
        uint16_t km, ri, bh;
        if (Storage_LoadConfig(&kd, &km, &rs, &ri, &dp, &bh)) {
            s_app.kick_duty = kd;
            s_app.kick_ms = km;
            s_app.ramp_step = rs;
            s_app.ramp_interval_ms = ri;
            s_app.default_pwm = dp;
            s_app.brake_hold_ms = bh;
            clamp_loaded_config();
        }
    }

    print_help();
    UartProtocol_Print("\r\n[CUBE] f411-motor-cube firmware ready");
    UartProtocol_PrintNewline();
}

void App_Loop(void)
{
    uint32_t now = HAL_GetTick();

    /* 1. Service Hall sensor. */
    HallSensor_Update();

    /* 1b. Gate test timeout (motor-disconnected scope test). */
    if (s_app.gatetest_active) {
        if ((HAL_GetTick() - s_app.gatetest_start_ms) >= s_app.gatetest_timeout_ms) {
            MotorDriver_AllOff();
            s_app.gatetest_active = false;
            UartProtocol_Print("\r\n[OK] Gate test done (outputs off)");
        }
    }

    /* 2. Drain UART ring buffer and parse any complete lines. */
    UartProtocol_Pump();
    char line[UART_LINE_MAX];
    UartSource src;
    while (UartProtocol_PopLine(line, sizeof(line), &src)) {
        handle_command(line, src);
    }

    /* 3. Service tasks (identify/scan/test) — non-blocking. */
    ServiceTask_Update();

    /* 4. Speed PI tick (50 Hz internally). */
    SpeedPI_Tick(now);

    /* 5. Service motor outputs. */
    service_motor();

    /* 6. Watchdogs (command timeout, host disconnect). */
    service_watchdogs();

    /* 7. Telemetry. */
    Telemetry_Tick(now);

    s_app.last_loop_ms = now;
}

/* ----------------------------------------------------------------
 * Read-only accessors for the telemetry layer.
 * These expose the app state machine values so telemetry.c can report
 * the same fields the legacy Arduino firmware reported (motor phase,
 * target/current duty, direction, speed-mode flag, brake flag) without
 * reaching into app_main private state.
 * ---------------------------------------------------------------- */

uint8_t App_GetTargetDuty(void)   { return s_app.target_duty; }
uint8_t App_GetCurrentDuty(void)  { return s_app.current_duty; }
int8_t  App_GetDirection(void)    { return (int8_t)s_app.direction; }
uint8_t App_GetMotorPhase(void)   { return (uint8_t)s_app.phase; }
bool    App_IsSpeedMode(void)     { return s_app.mode == MODE_SPEED; }
bool    App_IsBrakeActive(void)   { return s_app.phase == PHASE_BRAKE; }
