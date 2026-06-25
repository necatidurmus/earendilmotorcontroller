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

    int32_t     pending_rpm_target;

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

    /* arming state (safety) */
    bool        gate_test_armed;
    bool        service_armed;
    uint32_t    gate_arm_start_ms;
    uint32_t    service_arm_start_ms;

    /* direction reversal RPM wait */
    int8_t      reversal_pending_dir;
    uint32_t    reversal_start_ms;
    bool        reversal_waiting;

    /* RX overflow tracking */
    uint32_t    last_rx_drop_count;

    /* Hall map source tracking */
    uint8_t     hall_map_source;   /* 0=default, 1=identify, 2=manual, 3=flash */
    bool        hall_map_dirty;    /* true if RAM differs from flash/default */

    /* Candidate map for safe-apply workflow */
    uint8_t     candidate_map[8];
    bool        candidate_active;  /* true if user is editing a candidate */

    /* Identify result tracking */
    uint8_t     identify_last_result; /* 0=none, 1=ok, 2=rejected_dup, 3=rejected_missing,
                                         4=rejected_invalid_raw, 5=aborted */
    bool        identify_was_run;
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
    /* Safety-critical: cancel service tasks and gate test FIRST
     * so they cannot re-energize outputs after AllOff below. */
    if (ServiceTask_IsActive()) ServiceTask_Cancel();
    s_app.gatetest_active = false;

    SpeedPI_Disable();
    MotorDriver_AllOff();

    s_app.phase = PHASE_STOPPED;
    s_app.direction = (Direction)0;
    s_app.pending_direction = 0;
    s_app.target_duty = 0U;
    s_app.current_duty = 0U;
    s_app.ramp_current_duty = 0U;
    s_app.kick_active = false;
    s_app.kick_start_ms = 0U;
    s_app.last_ramp_update_ms = 0U;
    s_app.run_request = false;
    s_app.stop_request = false;
    s_app.duty_update_request = false;
    s_app.last_motor_cmd_ms = 0U;
    s_app.has_ever_run = false;
    s_app.last_edge_count = HallSensor_GetEdgeCounter();
    s_app.last_edge_ms = 0U;
    /* Reset arming on stop */
    s_app.gate_test_armed = false;
    s_app.service_armed = false;
    /* Reset direction reversal state */
    s_app.reversal_waiting = false;
    s_app.reversal_pending_dir = 0;
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
    /* Preserve the pending RPM target BEFORE disabling SpeedPI.
     * SpeedPI_Disable() zeros target_rpm_cmd, so save it first. */
    s_app.pending_rpm_target = SpeedPI_GetRawTargetRpm();
    SpeedPI_Disable();
    s_app.current_duty = 0U;
    /* Reset ramp/kick runtime state so the reverse direction starts
     * from zero duty with a fresh ramp/kick sequence.  Without this,
     * ramp_current_duty could carry the old forward duty into the
     * reverse direction (torque jump). */
    s_app.ramp_current_duty = 0U;
    s_app.kick_active = false;
    s_app.kick_start_ms = 0U;
    s_app.last_ramp_update_ms = HAL_GetTick();
    s_app.has_ever_run = false;
    s_app.last_edge_count = HallSensor_GetEdgeCounter();
    s_app.last_edge_ms = HAL_GetTick();
    s_app.phase = PHASE_NEUTRAL;
    s_app.pending_direction = new_direction;
    /* D2 fix: store start time (not release time) so the NEUTRAL
     * handler can use subtraction-based comparison that is safe
     * across the ~49.7 day HAL_GetTick() wrap. */
    s_app.neutral_release_ms = HAL_GetTick();
    /* Direction reversal RPM wait: do not start new direction until
     * motor RPM drops below threshold or timeout expires. */
    s_app.reversal_waiting = true;
    s_app.reversal_pending_dir = new_direction;
    s_app.reversal_start_ms = HAL_GetTick();
}

static void apply_duty_now(uint8_t duty)
{
    if (duty > 250U) duty = 250U;
    s_app.current_duty = duty;
    MotorDriver_SetDuty(duty);
}

/* Initialize duty ramp/kick runtime state when entering RUNNING from
 * STOPPED or from NEUTRAL (direction reversal).  This ensures a clean
 * start-from-zero ramp in both cases.  Must be called AFTER
 * s_app.target_duty is set. */
static void init_duty_start_runtime(void)
{
    if (SpeedPI_IsEnabled()) return;  /* SpeedPI manages its own duty */
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
    UartProtocol_Printf("\r\nSafety: current_sense=NOT_PRESENT current_limit=DISABLED bkin=DISABLED");
    UartProtocol_Printf("\r\nSafety: safety_level=BENCH_ONLY_BATTERY_UNSAFE");
    UartProtocol_Printf("\r\nSafety: gate_arm=%u service_arm=%u safety_lock=%u",
        (unsigned)s_app.gate_test_armed,
        (unsigned)s_app.service_armed,
        (unsigned)MotorDriver_IsSafetyLocked());
    UartProtocol_Printf("\r\nDrops: rx=%lu tx=%lu cmd=%lu epreempt=%lu",
        (unsigned long)UartProtocol_GetRxDropCount(),
        (unsigned long)UartProtocol_GetTxDropCount(),
        (unsigned long)UartProtocol_GetCmdDropCount(),
        (unsigned long)UartProtocol_GetEmergencyPreemptCount());
    UartProtocol_Printf("\r\nAge: cmd=%lu hall=%lu",
        (unsigned long)(s_app.last_motor_cmd_ms == 0U ? 0UL : HAL_GetTick() - s_app.last_motor_cmd_ms),
        (unsigned long)(s_app.last_edge_ms == 0U ? 0UL : HAL_GetTick() - s_app.last_edge_ms));
    /* Hall map status */
    {
        const char *src_name = "DEFAULT";
        switch (s_app.hall_map_source) {
        case 1: src_name = "RAM_IDENTIFY"; break;
        case 2: src_name = "RAM_MANUAL"; break;
        case 3: src_name = "FLASH"; break;
        case 4: src_name = "INVALID_FALLBACK"; break;
        default: src_name = "DEFAULT"; break;
        }
        uint8_t tmpmap[8];
        Commutation_GetMap(tmpmap);
        bool map_valid = Commutation_ValidateHallMap(tmpmap);
        UartProtocol_Printf("\r\nHallMap: source=%s valid=%u dirty=%u storage=DISABLED",
            src_name, (unsigned)map_valid, (unsigned)s_app.hall_map_dirty);
        if (s_app.identify_was_run) {
            const char *ires = "NONE";
            switch (s_app.identify_last_result) {
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
        "\r\n identify / scan / test"
        "\r\n gatetest <0-5> <1-10>  (motor disconnected only, arming req)"
        "\r\n clrerr"
        "\r\n estop / safe / alloff"
        "\r\n arm gatetest <token>"
        "\r\n arm service <token>"
        "\r\n disarm gatetest"
        "\r\n disarm service"
        "\r\n dbg on/off | telper <ms>"
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
        "\r\n  map load             load from flash"
        "\r\n  map save             save to flash (disabled)"
        "\r\n"
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
        /* K3 fix: direction reversal check BEFORE heartbeat so that a
         * direction change is never silently swallowed by the
         * same-direction+same-duty early return. */
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Direction reversal while running — neutral switch.
             * Preserve current target_duty (user may have set via pwm <n>). */
            begin_neutral_switch(+1);
            UartProtocol_Print("\r\n[OK] FWD (neutral switch)");
            return;
        }
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
        /* New motion (STOPPED→RUNNING).  Y1 fix: also set
         * pending_direction so begin_neutral_switch path and the
         * run_request path are consistent. */
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.direction = DIR_FWD;
        s_app.pending_direction = +1;
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
        /* K3 fix: direction reversal check BEFORE heartbeat. */
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Direction reversal while running — neutral switch.
             * Preserve current target_duty (user may have set via pwm <n>). */
            begin_neutral_switch(-1);
            UartProtocol_Print("\r\n[OK] REV (neutral switch)");
            return;
        }
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
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.direction = DIR_REV;
        s_app.pending_direction = -1;
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
        /* K3 fix: direction reversal check BEFORE heartbeat. */
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_REV) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = (uint8_t)d;
            begin_neutral_switch(+1);
            UartProtocol_Printf("\r\n[OK] FWD D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
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
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.direction = DIR_FWD;
        s_app.pending_direction = +1;
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
        /* K3 fix: direction reversal check BEFORE heartbeat. */
        if (s_app.phase == PHASE_RUNNING && s_app.direction == DIR_FWD) {
            /* Direction reversal while running — neutral switch. */
            s_app.target_duty = (uint8_t)d;
            begin_neutral_switch(-1);
            UartProtocol_Printf("\r\n[OK] REV D=%lu (neutral switch)", (unsigned long)d);
            return;
        }
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
        s_app.run_request = true;
        s_app.stop_request = false;
        s_app.direction = DIR_REV;
        s_app.pending_direction = -1;
        s_app.target_duty = (uint8_t)d;
        UartProtocol_Printf("\r\n[OK] REV D=%lu", (unsigned long)d);
        return;
    }
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "stop") == 0) {
        stop_immediate();
        UartProtocol_Print("\r\n[OK] Stop");
        return;
    }
    if (strcmp(cmd, "x") == 0 || strcmp(cmd, "brake") == 0) {
        /* ISSUE-046: brake is coast for first bring-up (no current sense).
         * Cancel service tasks and gate test immediately so they cannot
         * re-energize outputs.  Use stop_immediate() for full cleanup. */
        stop_immediate();
        UartProtocol_Print("\r\n[OK] Brake (coast in first bring-up)");
        return;
    }
    if (strcmp(cmd, "estop") == 0) {
        FaultManager_Raise(FAULT_ESTOP);
        stop_immediate();
        UartProtocol_Print("\r\n[OK] EMERGENCY STOP (fault latched, clrerr required)");
        return;
    }
    if (strcmp(cmd, "safe") == 0 || strcmp(cmd, "alloff") == 0) {
        stop_immediate();
        UartProtocol_Print("\r\n[OK] Safe stop (all off)");
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
        s_app.last_motor_cmd_ms = HAL_GetTick();

        if (s_app.phase == PHASE_RUNNING && old_dir != 0 && new_dir != old_dir) {
            /* Direction reversal while running — neutral switch.
             * s_app.direction is set by begin_neutral_switch via
             * pending_direction after the neutral period.
             * Set RPM target AFTER begin_neutral_switch because it
             * calls SpeedPI_Disable() which clears target_rpm_cmd. */
            begin_neutral_switch(new_dir);
            SpeedPI_SetTargetRpm((int32_t)v);
        } else if (s_app.phase == PHASE_RUNNING && new_dir == old_dir) {
            /* Already running same direction — heartbeat. */
            s_app.direction = (v > 0) ? DIR_FWD : DIR_REV;
            SpeedPI_SetTargetRpm((int32_t)v);
        } else {
            /* Stopped / not yet running — request start. */
            s_app.direction = (v > 0) ? DIR_FWD : DIR_REV;
            s_app.run_request = true;
            s_app.stop_request = false;
            SpeedPI_SetTargetRpm((int32_t)v);
        }
        UartProtocol_Printf("\r\n[OK] RPM=%ld", (long)v);
        return;
    }

    /* --- pi <kp> <ki> --- */
    if (starts_with(cmd, "pi ")) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        SpeedPI_SetKp(fv); UartProtocol_Printf("\r\n[OK] Kp_m=%ld", (long)(fv * 1000.0f)); return;
    }
    fv = parse_float_after(cmd, "ki ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        SpeedPI_SetKi(fv); UartProtocol_Printf("\r\n[OK] Ki_m=%ld", (long)(fv * 1000.0f)); return;
    }

    /* --- base <lo> <mid> <hi> ---
     * Each value is clamped to 0..250 (PWM duty range).  Negative
     * or non-numeric tokens are rejected with [ERR].
     * SpeedPI_SetBasePwm() further clamps to SPEED_PI_MAX_PWM;
     * the reply prints the actual stored values. */
    if (starts_with(cmd, "base ")) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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

    /* --- map subcommands ---
     * Safe-apply workflow: active map is never modified directly.
     * User edits a candidate map, validates it, then applies. */
    if (starts_with(cmd, "map")) {
        const char *sub = cmd + 3;

        /* "map" alone — show active map */
        if (sub[0] == '\0') {
            uint8_t map[8];
            Commutation_GetMap(map);
            const char *src_name = "DEFAULT";
            switch (s_app.hall_map_source) {
            case 1: src_name = "RAM_IDENTIFY"; break;
            case 2: src_name = "RAM_MANUAL"; break;
            case 3: src_name = "FLASH"; break;
            case 4: src_name = "INVALID_FALLBACK"; break;
            default: src_name = "DEFAULT"; break;
            }
            bool valid = Commutation_ValidateHallMap(map);
            UartProtocol_Printf("\r\nHALL_MAP active valid=%u source=%s dirty=%u",
                (unsigned)valid, src_name, (unsigned)s_app.hall_map_dirty);
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

        /* "map validate" */
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

        /* "map default" or "map reset" */
        if (strcmp(sub, " default") == 0 || strcmp(sub, " reset") == 0) {
            if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
            s_app.hall_map_source = 0U;  /* DEFAULT */
            s_app.hall_map_dirty = false;
            s_app.candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded into RAM");
            print_hall_map();
            return;
        }

        /* "map edit" — copy active to candidate */
        if (strcmp(sub, " edit") == 0) {
            Commutation_GetMap(s_app.candidate_map);
            s_app.candidate_active = true;
            UartProtocol_Print("\r\n[OK] Active map copied to candidate");
            return;
        }

        /* "map set <raw> <sector|invalid>" — edit candidate entry.
         * Custom two-argument parser: parse_long_after() rejects trailing
         * chars so "map set 3 2" would fail with it. */
        if (starts_with(sub, " set ")) {
            const char *p = sub + 5;
            while (*p == ' ') p++;
            /* Parse first arg: raw hall code 0..7 */
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
            /* Expect whitespace between args */
            const char *q = end1;
            while (*q == ' ') q++;
            if (*q == '\0') {
                UartProtocol_Print("\r\n[ERR] Usage: map set <0-7> <0-5|invalid>");
                return;
            }
            /* Parse second arg: sector 0..5, 255, or "invalid" */
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
                /* Reject trailing garbage after second number */
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
            /* Enforce raw 0/7 can only be invalid (255) */
            if ((raw == 0 || raw == 7) && sector != 255) {
                UartProtocol_Print("\r\n[ERR] raw 0 and raw 7 must be 'invalid' (255)");
                return;
            }
            if (!s_app.candidate_active) {
                /* Auto-start candidate from active map */
                Commutation_GetMap(s_app.candidate_map);
                s_app.candidate_active = true;
            }
            s_app.candidate_map[(uint8_t)raw] = (uint8_t)sector;
            char reason[32];
            bool valid = Commutation_ValidateHallMapVerbose(
                s_app.candidate_map, reason, sizeof(reason));
            UartProtocol_Printf("\r\n[OK] candidate[%lu] = %lu  valid=%u",
                (unsigned long)raw, (unsigned long)sector, (unsigned)valid);
            if (!valid) {
                UartProtocol_Printf(" (%s)", reason);
            }
            return;
        }

        /* "map candidate" — show candidate map */
        if (strcmp(sub, " candidate") == 0) {
            if (!s_app.candidate_active) {
                UartProtocol_Print("\r\n[INFO] No candidate map. Use 'map edit' first.");
                return;
            }
            char reason[32];
            bool valid = Commutation_ValidateHallMapVerbose(
                s_app.candidate_map, reason, sizeof(reason));
            UartProtocol_Printf("\r\nHALL_MAP candidate valid=%u", (unsigned)valid);
            if (!valid) UartProtocol_Printf(" reason=%s", reason);
            UartProtocol_Print("\r\nraw:    0   1   2   3   4   5   6   7");
            UartProtocol_Print("\r\nstate:");
            for (uint8_t i = 0; i < 8; i++)
                UartProtocol_Printf(" %3u", (unsigned)s_app.candidate_map[i]);
            UartProtocol_PrintNewline();
            return;
        }

        /* "map apply" — validate candidate and apply to active */
        if (strcmp(sub, " apply") == 0) {
            if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            if (FaultManager_GetLast() != FAULT_NONE) {
                UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr");
                return;
            }
            if (!s_app.candidate_active) {
                UartProtocol_Print("\r\n[ERR] No candidate map. Use 'map edit' first.");
                return;
            }
            char reason[32];
            if (!Commutation_ValidateHallMapVerbose(s_app.candidate_map, reason, sizeof(reason))) {
                UartProtocol_Printf("\r\n[ERR] Candidate map rejected: %s", reason);
                UartProtocol_Print("\r\n[SAFE] Active map unchanged");
                return;
            }
            /* Apply atomically */
            Commutation_ApplyMap(s_app.candidate_map);
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s_app.hall_map_source = 2U;  /* MANUAL */
            s_app.hall_map_dirty = true;
            s_app.candidate_active = false;
            UartProtocol_Print("\r\n[OK] Candidate map applied to active RAM map");
            UartProtocol_Print("\r\n[WARN] Map is RAM-only. Use 'map save' after verification if storage is enabled.");
            print_hall_map();
            return;
        }

        /* "map discard" — discard candidate */
        if (strcmp(sub, " discard") == 0) {
            s_app.candidate_active = false;
            memset(s_app.candidate_map, 255, sizeof(s_app.candidate_map));
            UartProtocol_Print("\r\n[OK] Candidate map discarded");
            return;
        }

        /* "map load" — load from flash */
        if (strcmp(sub, " load") == 0 || strcmp(sub, " reload") == 0) {
            if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
                    s_app.hall_map_source = 0U;
                    s_app.hall_map_dirty = false;
                } else {
                    Commutation_ApplyMap(map);
                    HallSensor_OnMapChanged();
                    SpeedPI_Reset();
                    s_app.hall_map_source = 3U;  /* FLASH */
                    s_app.hall_map_dirty = false;
                    UartProtocol_Print("\r\n[OK] Hall map loaded from flash");
                }
            } else {
                Commutation_LoadDefaultMap();
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s_app.hall_map_source = 0U;
                s_app.hall_map_dirty = false;
                UartProtocol_Print("\r\n[INFO] No saved map in flash, defaults loaded");
            }
            print_hall_map();
            return;
        }

        /* "map save" — disabled */
        if (strcmp(sub, " save") == 0) {
            UartProtocol_Print("\r\n[ERR] Persistent storage disabled in this build");
            UartProtocol_Print("\r\n[INFO] Map lives in RAM only until reset");
            return;
        }

        /* Legacy compat: "mapreset" */
        if (strcmp(cmd, "mapreset") == 0) {
            if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
                UartProtocol_Print("\r\n[ERR] Stop motor first");
                return;
            }
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s_app.hall_map_source = 0U;
            s_app.hall_map_dirty = false;
            s_app.candidate_active = false;
            UartProtocol_Print("\r\n[OK] Default map loaded");
            print_hall_map();
            return;
        }

        /* Unknown map subcommand */
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
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
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
                s_app.hall_map_source = 0U;
                s_app.hall_map_dirty = false;
            } else {
                Commutation_ApplyMap(map);
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s_app.hall_map_source = 3U;
                s_app.hall_map_dirty = false;
                UartProtocol_Print("\r\n[OK] Hall map loaded from flash");
            }
        } else {
            Commutation_LoadDefaultMap();
            HallSensor_OnMapChanged();
            SpeedPI_Reset();
            s_app.hall_map_source = 0U;
            s_app.hall_map_dirty = false;
            UartProtocol_Print("\r\n[INFO] No saved map, defaults loaded");
        }
        print_hall_map();
        return;
    }

    /* --- identify / scan / test --- */
    if (strcmp(cmd, "identify") == 0) {
        if (!s_app.service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
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
        s_app.identify_was_run = true;
        s_app.identify_last_result = 0U;
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
        if (!s_app.service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        if (service_busy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return; }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
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

    /* --- gatetest <sector> <duty> ---
     * Motor-disconnected-only scope verification.
     * Requires arming: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND */
    if (starts_with(cmd, "gatetest ")) {
        if (!s_app.gate_test_armed) {
            UartProtocol_Print("\r\n[ERR] Gate test not armed. Use: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
            return;
        }
        if (!motion_allowed()) { UartProtocol_Print("\r\n[ERR] Fault latched; use clrerr"); return; }
        if (s_app.phase != PHASE_STOPPED && s_app.phase != PHASE_FAULT) {
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
        s_app.gatetest_active   = true;
        s_app.gatetest_sector   = (uint8_t)sector;
        s_app.gatetest_duty     = (uint8_t)duty;
        s_app.gatetest_start_ms = HAL_GetTick();
        s_app.gatetest_timeout_ms = 100U;
        MotorDriver_ApplyStep((uint8_t)sector, +1, (uint8_t)duty);
        UartProtocol_Printf("\r\n[OK] Gate test sector=%lu duty=%lu timeout=100ms",
                            (unsigned long)sector, (unsigned long)duty);
        return;
    }
    /* --- savecfg / loadcfg / defaults / saveall --- */
    if (strcmp(cmd, "loadcfg") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
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
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
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

    /* --- kick / ramp config commands --- */
    if (strcmp(cmd, "kick on") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s_app.kick_enabled = true;  UartProtocol_Print("\r\n[OK] Kick ON");  return;
    }
    if (strcmp(cmd, "kick off") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s_app.kick_enabled = false; UartProtocol_Print("\r\n[OK] Kick OFF"); return;
    }
    if (strcmp(cmd, "ramp on") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s_app.ramp_enabled = true;  UartProtocol_Print("\r\n[OK] Ramp ON");  return;
    }
    if (strcmp(cmd, "ramp off") == 0) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        s_app.ramp_enabled = false; UartProtocol_Print("\r\n[OK] Ramp OFF"); return;
    }

    /* --- kick / ramp config commands ---
     * ISSUE-038: range-check all numeric arguments so negative or
     * huge values do not wrap into uint8_t / uint16_t silently. */
    v = parse_long_after(cmd, "kickduty ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > KICK_DUTY_MAX) v = KICK_DUTY_MAX;
        s_app.kick_duty = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] KickDuty=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "kickms ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > KICK_MS_MAX) v = KICK_MS_MAX;
        s_app.kick_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] KickMs=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "ramprate ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > RAMP_STEP_MAX) v = RAMP_STEP_MAX;
        s_app.ramp_step = (uint8_t)v;
        UartProtocol_Printf("\r\n[OK] RampStep=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "rampms ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
        if (v < 0) v = 0;
        if (v > RAMP_INTERVAL_MS_MAX) v = RAMP_INTERVAL_MS_MAX;
        s_app.ramp_interval_ms = (uint16_t)v;
        UartProtocol_Printf("\r\n[OK] RampMs=%lu", (unsigned long)v);
        return;
    }
    v = parse_long_after(cmd, "defpwm ", &ok);
    if (ok) {
        if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
            UartProtocol_Print("\r\n[ERR] Stop motor first"); return;
        }
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
        /* P0-05: Warn the operator that clrerr clears ALL faults
         * including safety-critical ones.  Without current sensing,
         * running on battery is dangerous — the operator must
         * acknowledge this risk before resuming. */
        FaultCode prev_fault = FaultManager_GetLast();
        UartProtocol_Print("\r\n[WARN] clrerr clears ALL faults including safety-critical");
        UartProtocol_Print("\r\n[WARN] No current sense — battery operation is UNSAFE");
        if (prev_fault != FAULT_NONE) {
            UartProtocol_Printf("\r\n[WARN] Clearing fault: %s",
                FaultManager_GetName(prev_fault));
        }
        FaultManager_Clear();
        HallSensor_ClearFault();
        s_app.queue_overflow = false;
        UartProtocol_ResetTxDropCount();
        UartProtocol_ResetCmdDropCount();
        UartProtocol_ResetRxDropCount();
        UartProtocol_ResetEmergencyPreemptCount();
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
        s_app.gatetest_active = false;
        s_app.reversal_waiting = false;
        s_app.reversal_pending_dir = 0;
        if (ServiceTask_IsActive()) ServiceTask_Cancel();
        MotorDriver_AllOff();
        MotorDriver_SetSafetyLock(false);  /* unlock after clrerr */
        SpeedPI_Disable();
        UartProtocol_Print("\r\n[OK] Errors cleared, motor stopped, safety unlocked");
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

    /* --- arm / disarm (safety) --- */
    if (starts_with(cmd, "arm gatetest ")) {
        const char *token = cmd + 13;
        while (*token == ' ') token++;
        if (strcmp(token, "motor_disconnected_i_understand") == 0) {
            s_app.gate_test_armed = true;
            s_app.gate_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Gate test armed for 30s. Motor must be disconnected!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
        }
        return;
    }
    if (starts_with(cmd, "arm service ")) {
        const char *token = cmd + 12;
        while (*token == ' ') token++;
        if (strcmp(token, "current_limited_bench_supply") == 0) {
            s_app.service_armed = true;
            s_app.service_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Service armed for 30s. Use current-limited PSU!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm service CURRENT_LIMITED_BENCH_SUPPLY");
        }
        return;
    }
    if (strcmp(cmd, "disarm gatetest") == 0) {
        s_app.gate_test_armed = false;
        UartProtocol_Print("\r\n[OK] Gate test disarmed");
        return;
    }
    if (strcmp(cmd, "disarm service") == 0) {
        s_app.service_armed = false;
        UartProtocol_Print("\r\n[OK] Service disarmed");
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
    /* Central fault guard: if any fault is latched, force the motor
     * to a safe state immediately.  This MUST run before the
     * gatetest/service early-returns so a fault during a service
     * task is not bypassed. */
    if (FaultManager_GetLast() != FAULT_NONE) {
        s_app.gatetest_active = false;
        if (ServiceTask_IsActive()) ServiceTask_Cancel();
        MotorDriver_AllOff();
        MotorDriver_SetSafetyLock(true);
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
        s_app.gate_test_armed = false;
        s_app.service_armed = false;
        s_app.reversal_waiting = false;
        return;
    }

    /* RX overflow safety: if RX ring overflowed while motor is active,
     * stop the motor and raise a fault. */
    {
        uint32_t rx_drop = UartProtocol_GetRxDropCount();
        if (rx_drop != s_app.last_rx_drop_count) {
            s_app.last_rx_drop_count = rx_drop;
            if (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL) {
                FaultManager_Raise(FAULT_UART_RX_OVERFLOW);
                stop_immediate();
                UartProtocol_Print("\r\n[ERR] RX overflow during motor active");
                return;
            }
        }
    }

    /* Do not let service_motor() clobber the gate outputs that
     * ServiceTask_Update() or gatetest handler just applied. */
    if (s_app.gatetest_active) return;
    if (ServiceTask_IsActive()) return;

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
     * the motor is actually running. */
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
        if (!SpeedPI_IsEnabled() &&
            (s_app.phase == PHASE_RUNNING || s_app.phase == PHASE_NEUTRAL)) {
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
        int8_t req_dir;
        if (s_app.mode == MODE_SPEED) {
            int32_t t = SpeedPI_GetRawTargetRpm();
            req_dir = (t > 0) ? +1 : (t < 0 ? -1 : 0);
        } else {
            req_dir = (s_app.direction > 0) ? +1 : ((s_app.direction < 0) ? -1 : 0);
        }

        if (req_dir != 0 &&
            s_app.phase == PHASE_RUNNING && s_app.direction != 0 &&
            s_app.direction != (Direction)req_dir) {
            begin_neutral_switch(req_dir);
        } else if (req_dir != 0 &&
                   s_app.phase == PHASE_RUNNING &&
                   s_app.direction == (Direction)req_dir) {
            /* heartbeat — do nothing */
        } else if (req_dir != 0 && s_app.phase != PHASE_NEUTRAL) {
            s_app.phase = PHASE_RUNNING;
            s_app.direction = (Direction)req_dir;
            s_app.phase_start_ms = HAL_GetTick();
            s_app.has_ever_run = false;
            s_app.last_edge_count = HallSensor_GetEdgeCounter();
            s_app.last_edge_ms = HAL_GetTick();
            init_duty_start_runtime();
        }
        s_app.run_request = false;
    }

    /* Neutral-wait phase with RPM-based direction reversal safety */
    if (s_app.phase == PHASE_NEUTRAL) {
        MotorDriver_AllOff();
        uint32_t now = HAL_GetTick();

        /* D2 fix: use subtraction-based elapsed time so the check is
         * safe across the ~49.7 day HAL_GetTick() 32-bit wrap. */

        /* Direction reversal RPM wait: check if motor has slowed down */
        if (s_app.reversal_waiting) {
            uint32_t rpm = HallSensor_CalculateRpm();
            bool rpm_low = (rpm < 5U);
            bool timeout = ((now - s_app.reversal_start_ms) > 3000U);
            bool neutral_time = ((now - s_app.neutral_release_ms) >= DIRECTION_NEUTRAL_MS);

            if (rpm_low || timeout) {
                /* RPM low enough or timeout — proceed with reversal */
                s_app.reversal_waiting = false;
                if (neutral_time) {
                    s_app.direction = (Direction)s_app.pending_direction;
                    s_app.phase = PHASE_RUNNING;
                    s_app.phase_start_ms = now;
                    s_app.has_ever_run = false;
                    s_app.last_edge_count = HallSensor_GetEdgeCounter();
                    s_app.last_edge_ms = now;
                    init_duty_start_runtime();

                    /* Re-enable SpeedPI with the preserved target if we were in speed mode */
                    if (s_app.mode == MODE_SPEED) {
                        SpeedPI_Enable();
                        SpeedPI_SetTargetRpm(s_app.pending_rpm_target);
                    }
                }
            } else if (neutral_time && !rpm_low) {
                /* Neutral time expired but RPM still high — reset
                 * neutral start so the next iteration waits another
                 * DIRECTION_NEUTRAL_MS window. */
                s_app.neutral_release_ms = now;
            }
            return;
        }

        /* Fallback: simple neutral timeout (no reversal wait) */
        if ((now - s_app.neutral_release_ms) >= DIRECTION_NEUTRAL_MS) {
            s_app.direction = (Direction)s_app.pending_direction;
            s_app.phase = PHASE_RUNNING;
            s_app.phase_start_ms = now;
            s_app.has_ever_run = false;
            s_app.last_edge_count = HallSensor_GetEdgeCounter();
            s_app.last_edge_ms = now;
            init_duty_start_runtime();

            /* Re-enable SpeedPI with the preserved target if we were in speed mode */
            if (s_app.mode == MODE_SPEED) {
                SpeedPI_Enable();
                SpeedPI_SetTargetRpm(s_app.pending_rpm_target);
            }
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
    } else if (s_app.phase == PHASE_RUNNING) {
        uint32_t now = HAL_GetTick();
        if (s_app.kick_active) {
            if ((now - s_app.kick_start_ms) >= s_app.kick_ms) {
                s_app.kick_active = false;
                if (s_app.ramp_enabled) {
                    s_app.ramp_current_duty = s_app.kick_duty;
                    s_app.last_ramp_update_ms = now;
                } else {
                    s_app.ramp_current_duty = s_app.target_duty;
                }
                s_app.current_duty = s_app.ramp_current_duty;
                apply_duty_now(s_app.ramp_current_duty);
            }
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
        /* P1-001 fix (corrected): check the CURRENT raw Hall state for
         * validity, not the cached lastValidState.  HallSensor_GetMappedState()
         * returns lastValidState which stays at the previous good sector
         * even when Hall outputs 0b000/0b111.  This would cause a 100ms
         * blind drive on the stale sector.  Instead, check raw validity
         * first and AllOff immediately on invalid. */
        if (!HallSensor_IsCurrentRawValid()) {
            MotorDriver_AllOff();
        } else {
            uint8_t state = HallSensor_GetMappedState();
            if (state <= 5U) {
                MotorDriver_ApplyStep(state, (int8_t)s_app.direction, apply_duty);
            } else {
                MotorDriver_AllOff();
            }
        }
    } else if (s_app.phase == PHASE_BRAKE) {
        /* ISSUE-046: coast in first bring-up (no active brake) */
        MotorDriver_AllOff();
    } else {
        MotorDriver_AllOff();
    }

    /* Hall freshness / startup timeout (ISSUE-009). */
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
                       /* P0-008 fix: use last_edge_ms instead of last_motor_cmd_ms
                        * for Hall feedback timeout.  last_motor_cmd_ms is for
                        * command watchdog / host disconnect only. */
                       (HAL_GetTick() - s_app.last_edge_ms) > RPM_FEEDBACK_TIMEOUT_MS) {
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

    /* Arming auto-disarm */
    if (s_app.gate_test_armed && (HAL_GetTick() - s_app.gate_arm_start_ms) > ARM_TIMEOUT_MS) {
        s_app.gate_test_armed = false;
        UartProtocol_Print("\r\n[INFO] Gate test arming expired");
    }
    if (s_app.service_armed && (HAL_GetTick() - s_app.service_arm_start_ms) > ARM_TIMEOUT_MS) {
        s_app.service_armed = false;
        UartProtocol_Print("\r\n[INFO] Service arming expired");
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

    /* Arming defaults */
    s_app.gate_test_armed = false;
    s_app.service_armed = false;
    s_app.gate_arm_start_ms = 0U;
    s_app.service_arm_start_ms = 0U;

    /* Direction reversal defaults */
    s_app.reversal_waiting = false;
    s_app.reversal_pending_dir = 0;
    s_app.reversal_start_ms = 0U;

    /* RX overflow tracking */
    s_app.last_rx_drop_count = 0U;

    /* Hall map defaults */
    s_app.hall_map_source = 0U;  /* DEFAULT */
    s_app.hall_map_dirty = false;
    s_app.candidate_active = false;
    memset(s_app.candidate_map, 255, sizeof(s_app.candidate_map));
    s_app.identify_last_result = 0U;
    s_app.identify_was_run = false;

    /* Bring up the peripherals. */
    MotorDriver_Init();
    Commutation_LoadDefaultMap();
    HallSensor_Init();
    SpeedPI_Init();
    UartProtocol_Init();
    Telemetry_Init();
    FaultManager_Init();
    ServiceTask_Init();

    /* Load saved hall map from flash (if any).
     * Full validation ensures raw 0/7 are 255, all 6 sectors present,
     * no duplicates.  If invalid, fall back to defaults. */
    {
        uint8_t map[8];
        if (Storage_LoadHallMap(map)) {
            if (Commutation_ValidateHallMap(map)) {
                Commutation_ApplyMap(map);
                s_app.hall_map_source = 3U;  /* FLASH */
                s_app.hall_map_dirty = false;
            } else {
                /* Flash map invalid — keep defaults, warn via status */
                s_app.hall_map_source = 4U;  /* INVALID_FALLBACK */
                s_app.hall_map_dirty = false;
            }
        }
    }

    /* Load saved config from flash (if any).
     * D1 fix: always call clamp_loaded_config() to validate
     * brake_hold_ms and other fields, even if flash data looks
     * reasonable — a zero brake_hold_ms would fire the brake
     * timeout immediately. */
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
        }
        clamp_loaded_config();
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

void App_SetIdentifyResult(uint8_t result)
{
    s_app.identify_last_result = result;
    if (result == 1U) {
        /* Identify produced a valid map — apply it to the active map.
         * The candidate has already been validated by service_task. */
        s_app.hall_map_source = 1U;  /* RAM_IDENTIFY */
        s_app.hall_map_dirty = true;
    }
}
