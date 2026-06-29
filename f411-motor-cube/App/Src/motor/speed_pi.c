/* ============================================================
 * App/Src/speed_pi.c
 *
 * Non-blocking speed controller with the three states required by
 * the spec: IDLE / START_BOOST / SPEED_PI.
 *   duty = base_pwm(RPM) + Kp * error + Ki * integral
 *
 * Anti-windup: if the tentative output would saturate AND the error
 * would push further into saturation, the integrator is held.  The
 * integrator is bounded by +/- PID_INTEGRAL_LIMIT.
 *
 * The controller runs at 50 Hz (PID_INTERVAL_MS = 20 ms).  It is
 * driven from App_Loop, NOT from an ISR.
 * ============================================================ */

#include "speed_pi.h"
#include "hall_sensor.h"
#include "app_config.h"
#include "app_utils.h"       /* SAFE_ABS */

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool       enabled;
    SpeedPhase phase;
    SpeedFault fault;

    int32_t    target_rpm_cmd;
    float      target_rpm_ramped;

    float      kp;
    float      ki;
    float      integral;

    uint16_t   base_pwm[SPEED_PI_BAND_COUNT];
    uint16_t   boost_pwm[SPEED_PI_BAND_COUNT];
    uint16_t   boost_ms;
    uint8_t    boost_edge_thresh;

    float      ramp_up;
    float      ramp_down;

    uint32_t   last_tick_ms;
    uint32_t   last_hall_edge_ms;
    uint32_t   boost_start_ms;
    uint32_t   boost_start_edge;
    uint32_t   hall_edge_counter;

    uint16_t   computed_duty;
    int8_t     direction;     /* -1, 0, +1 */

    uint8_t    fault_retry;
} SpiRT;

static SpiRT s_spi;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint16_t clamp_u16(int v, int lo, int hi)
{
    if (v < lo) return (uint16_t)lo;
    if (v > hi) return (uint16_t)hi;
    return (uint16_t)v;
}

void SpeedPI_Init(void)
{
    memset(&s_spi, 0, sizeof(s_spi));
    s_spi.kp               = DEFAULT_SPEED_KP;
    s_spi.ki               = DEFAULT_SPEED_KI;
    const uint16_t base_defaults[SPEED_PI_BAND_COUNT] = {
        DEFAULT_BASE_PWM_1, DEFAULT_BASE_PWM_2, DEFAULT_BASE_PWM_3, DEFAULT_BASE_PWM_4,
        DEFAULT_BASE_PWM_5, DEFAULT_BASE_PWM_6, DEFAULT_BASE_PWM_7, DEFAULT_BASE_PWM_8
    };
    const uint16_t boost_defaults[SPEED_PI_BAND_COUNT] = {
        DEFAULT_BOOST_PWM_1, DEFAULT_BOOST_PWM_2, DEFAULT_BOOST_PWM_3, DEFAULT_BOOST_PWM_4,
        DEFAULT_BOOST_PWM_5, DEFAULT_BOOST_PWM_6, DEFAULT_BOOST_PWM_7, DEFAULT_BOOST_PWM_8
    };
    memcpy(s_spi.base_pwm, base_defaults, sizeof(s_spi.base_pwm));
    memcpy(s_spi.boost_pwm, boost_defaults, sizeof(s_spi.boost_pwm));
    s_spi.boost_ms         = DEFAULT_BOOST_TIME_MS;
    s_spi.boost_edge_thresh= DEFAULT_BOOST_EDGE_THRESH;
    s_spi.ramp_up          = DEFAULT_RAMP_UP_RPM_SEC;
    s_spi.ramp_down        = DEFAULT_RAMP_DOWN_RPM_SEC;
    s_spi.phase            = SPD_IDLE;
    s_spi.fault            = SPD_FAULT_NONE;
    s_spi.computed_duty    = 0U;
    s_spi.direction        = 0;
}

void SpeedPI_Reset(void)
{
    s_spi.integral         = 0.0f;
    s_spi.target_rpm_ramped= 0.0f;
    s_spi.phase            = SPD_IDLE;
    s_spi.fault            = SPD_FAULT_NONE;
    s_spi.computed_duty    = 0U;
    s_spi.boost_start_ms   = 0U;
    s_spi.boost_start_edge = s_spi.hall_edge_counter;
    s_spi.last_hall_edge_ms= 0U;
    s_spi.fault_retry      = 0U;
}

void SpeedPI_Enable(void)
{
    s_spi.enabled = true;
    SpeedPI_Reset();
}

void SpeedPI_Disable(void)
{
    s_spi.enabled = false;
    s_spi.target_rpm_cmd  = 0;
    s_spi.direction       = 0;
    SpeedPI_Reset();
}

bool SpeedPI_IsEnabled(void) { return s_spi.enabled; }

void SpeedPI_SetTargetRpm(int32_t rpm)
{
    s_spi.target_rpm_cmd = rpm;
    s_spi.direction = (rpm > 0) ? +1 : ((rpm < 0) ? -1 : 0);
}

static uint8_t band_for_rpm(float abs_rpm)
{
    if (abs_rpm <= 0.0f) return 0U;
    uint32_t scaled = (uint32_t)(abs_rpm * (float)SPEED_PI_BAND_COUNT);
    uint32_t index = scaled / (uint32_t)MAX_RPM_TARGET;
    if (index >= SPEED_PI_BAND_COUNT) index = SPEED_PI_BAND_COUNT - 1U;
    return (uint8_t)index;
}

static uint16_t base_pwm_for_rpm(float abs_rpm)
{
    return s_spi.base_pwm[band_for_rpm(abs_rpm)];
}

static uint16_t boost_pwm_for_rpm(float abs_rpm)
{
    return s_spi.boost_pwm[band_for_rpm(abs_rpm)];
}

void SpeedPI_Tick(uint32_t nowMs)
{
    if (!s_spi.enabled) {
        s_spi.computed_duty = 0U;
        return;
    }

    if ((nowMs - s_spi.last_tick_ms) < PID_INTERVAL_MS) {
        return;
    }
    float dt = (float)PID_INTERVAL_MS / 1000.0f;
    s_spi.last_tick_ms = nowMs;

    /* Track the most recent Hall edge via monotonic counter BEFORE
     * timeout check so a new edge arriving just before the timeout
     * evaluation is not missed (race fix). */
    {
        uint32_t edges = HallSensor_GetEdgeCounter();
        if (edges != s_spi.hall_edge_counter) {
            s_spi.last_hall_edge_ms = nowMs;
            s_spi.hall_edge_counter = edges;
        }
    }

    /* Hall feedback timeout.  If we ever got an edge but haven't seen
     * one for RPM_FEEDBACK_TIMEOUT_MS, raise a fault. */
    if (s_spi.last_hall_edge_ms != 0U &&
        (nowMs - s_spi.last_hall_edge_ms) > RPM_FEEDBACK_TIMEOUT_MS) {
        s_spi.fault = SPD_FAULT_NO_HALL;
        s_spi.computed_duty = 0U;
        s_spi.phase = SPD_IDLE;
        return;
    }

    /* Target = 0 -> coast. */
    if (s_spi.target_rpm_cmd == 0) {
        s_spi.target_rpm_ramped = 0.0f;
        s_spi.computed_duty     = 0U;
        s_spi.phase             = SPD_IDLE;
        s_spi.integral          = 0.0f;
        return;
    }

    /* Ramp the (absolute) target. */
    int32_t t = s_spi.target_rpm_cmd;
    float target_abs = (float)SAFE_ABS(t);
    if (s_spi.target_rpm_ramped < target_abs) {
        s_spi.target_rpm_ramped += s_spi.ramp_up * dt;
        if (s_spi.target_rpm_ramped > target_abs) s_spi.target_rpm_ramped = target_abs;
    } else if (s_spi.target_rpm_ramped > target_abs) {
        s_spi.target_rpm_ramped -= s_spi.ramp_down * dt;
        if (s_spi.target_rpm_ramped < target_abs) s_spi.target_rpm_ramped = target_abs;
    }

    /* ---- Start boost phase ---- */
    if (s_spi.phase == SPD_IDLE) {
        s_spi.boost_start_ms   = nowMs;
        s_spi.boost_start_edge = s_spi.hall_edge_counter;
        s_spi.integral         = 0.0f;
        s_spi.phase            = SPD_START_BOOST;
    }

    if (s_spi.phase == SPD_START_BOOST) {
        uint16_t boost = boost_pwm_for_rpm(s_spi.target_rpm_ramped);
        s_spi.computed_duty = boost;

        uint32_t edges = s_spi.hall_edge_counter - s_spi.boost_start_edge;
        float rpm = HallSensor_GetFilteredRpm();
        bool edges_ok = (edges >= s_spi.boost_edge_thresh);
        bool rpm_ok   = (rpm > BOOST_RPM_THRESHOLD);
        bool timeout  = ((nowMs - s_spi.boost_start_ms) >= s_spi.boost_ms);

        if (edges_ok || rpm_ok) {
            s_spi.phase    = SPD_SPEED_PI;
            s_spi.integral = 0.0f;
            s_spi.fault_retry = 0U;
        } else if (timeout) {
            if (edges == 0U) {
                s_spi.fault_retry++;
                if (s_spi.fault_retry >= 3U) {
                    s_spi.fault = SPD_FAULT_NO_HALL;
                    s_spi.computed_duty = 0U;
                    s_spi.phase = SPD_IDLE;
                    return;
                }
                /* Retry: go back to IDLE which re-enters START_BOOST
                 * on the next tick with a fresh timestamp and edge
                 * baseline.  Output is briefly zero (all-off). */
                s_spi.computed_duty = 0U;
                s_spi.phase = SPD_IDLE;
            } else {
                /* Some edges but below threshold — proceed to PI. */
                s_spi.phase    = SPD_SPEED_PI;
                s_spi.integral = 0.0f;
            }
        }
        return;
    }

    /* ---- Speed PI phase ---- */
    float rpm = HallSensor_GetFilteredRpm();
    float error = s_spi.target_rpm_ramped - rpm;

    float base = (float)base_pwm_for_rpm(s_spi.target_rpm_ramped);
    float p_term = s_spi.kp * error;

    /* Conditional anti-windup. */
    float tentative = base + p_term + s_spi.ki * (s_spi.integral + error * dt);
    bool sat_hi = (tentative >= (float)SPEED_PI_MAX_PWM);
    bool sat_lo = (tentative <= (float)SPEED_PI_MIN_PWM);
    bool reduces = (sat_hi && error < 0.0f) || (sat_lo && error > 0.0f);
    if ((!sat_hi && !sat_lo) || reduces) {
        s_spi.integral += error * dt;
    }
    s_spi.integral = clampf(s_spi.integral, -PID_INTEGRAL_LIMIT, PID_INTEGRAL_LIMIT);

    float i_term = s_spi.ki * s_spi.integral;
    float out    = base + p_term + i_term;

    s_spi.computed_duty = clamp_u16((int)roundf(out),
                                    (int)SPEED_PI_MIN_PWM,
                                    (int)SPEED_PI_MAX_PWM);
    if (s_spi.computed_duty == 0U && s_spi.target_rpm_cmd != 0) {
        s_spi.computed_duty = 1U;
    }

    /* P0-02b: Progressive stall duty reduction.  If no Hall edges for
     * longer than STALL_DUTY_REDUCE_START_MS but shorter than
     * RPM_FEEDBACK_TIMEOUT_MS, linearly reduce the maximum duty from
     * SPEED_PI_MAX_PWM down to 0.  This limits MOSFET and PSU stress
     * during a stall (motor locked, no current sensing) while still
     * allowing the full timeout period for recovery. */
    if (s_spi.last_hall_edge_ms != 0U) {
        uint32_t stall_ms = nowMs - s_spi.last_hall_edge_ms;
        if (stall_ms > STALL_DUTY_REDUCE_START_MS &&
            stall_ms < RPM_FEEDBACK_TIMEOUT_MS) {
            uint32_t reduce_window = RPM_FEEDBACK_TIMEOUT_MS
                                   - STALL_DUTY_REDUCE_START_MS;
            uint32_t into_stall = stall_ms - STALL_DUTY_REDUCE_START_MS;
            float factor = 1.0f - (float)into_stall / (float)reduce_window;
            uint16_t reduced_max = (uint16_t)((float)SPEED_PI_MAX_PWM * factor);
            if (s_spi.computed_duty > reduced_max) {
                s_spi.computed_duty = reduced_max;
            }
        }
    }
}

float   SpeedPI_GetRampedTargetRpm(void) { return s_spi.target_rpm_ramped; }
int32_t SpeedPI_GetRawTargetRpm(void)    { return s_spi.target_rpm_cmd; }
uint16_t SpeedPI_GetComputedDuty(void)   { return s_spi.computed_duty; }
SpeedPhase SpeedPI_GetPhase(void)        { return s_spi.phase; }
SpeedFault SpeedPI_GetFault(void)        { return s_spi.fault; }

float SpeedPI_GetK(void)                 { return s_spi.kp; }
void  SpeedPI_SetKp(float kp)            { s_spi.kp = clampf(kp, 0.0f, 10.0f); }
void  SpeedPI_SetKi(float ki)            { s_spi.ki = clampf(ki, 0.0f, 10.0f); }
void SpeedPI_SetBasePwm(const uint16_t bands[SPEED_PI_BAND_COUNT])
{
    /* ISSUE-040: clamp each band to 0..SPEED_PI_MAX_PWM so a stray
     * `base` command cannot push the feed-forward above the PI
     * saturation limit. */
    uint16_t mx = SPEED_PI_MAX_PWM;
    for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
        s_spi.base_pwm[i] = (bands[i] > mx) ? mx : bands[i];
    }
}
void SpeedPI_SetBoostPwm(const uint16_t bands[SPEED_PI_BAND_COUNT], uint16_t ms)
{
    uint16_t mx = SPEED_PI_MAX_PWM;
    for (uint8_t i = 0U; i < SPEED_PI_BAND_COUNT; i++) {
        s_spi.boost_pwm[i] = (bands[i] > mx) ? mx : bands[i];
    }
    if (ms > 1000U) ms = 1000U;
    s_spi.boost_ms   = ms;
}
void SpeedPI_SetRamp(float upPerSec, float downPerSec)
{
    s_spi.ramp_up   = clampf(upPerSec,   1.0f, 10000.0f);
    s_spi.ramp_down = clampf(downPerSec, 1.0f, 10000.0f);
}

void SpeedPI_GetGains(float *kp, float *ki)
{
    if (kp) *kp = s_spi.kp;
    if (ki) *ki = s_spi.ki;
}
void SpeedPI_GetBasePwm(uint16_t bands[SPEED_PI_BAND_COUNT])
{
    if (bands) memcpy(bands, s_spi.base_pwm, sizeof(s_spi.base_pwm));
}
void SpeedPI_GetBoostPwm(uint16_t bands[SPEED_PI_BAND_COUNT], uint16_t *ms)
{
    if (bands) memcpy(bands, s_spi.boost_pwm, sizeof(s_spi.boost_pwm));
    if (ms)   *ms   = s_spi.boost_ms;
}
void SpeedPI_GetRampRates(float *up, float *down)
{
    if (up)   *up   = s_spi.ramp_up;
    if (down) *down = s_spi.ramp_down;
}
