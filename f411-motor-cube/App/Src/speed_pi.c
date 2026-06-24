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

    uint8_t    base_low;
    uint8_t    base_mid;
    uint8_t    base_high;

    uint8_t    boost_low;
    uint8_t    boost_mid;
    uint8_t    boost_high;
    uint16_t   boost_ms;
    uint8_t    boost_edge_thresh;

    float      ramp_up;
    float      ramp_down;

    uint32_t   last_tick_ms;
    uint32_t   last_hall_edge_ms;
    uint32_t   boost_start_ms;
    uint32_t   boost_start_edge;
    uint32_t   hall_edge_counter;

    uint8_t    computed_duty;
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

static uint8_t clamp_u8(int v, int lo, int hi)
{
    if (v < lo) return (uint8_t)lo;
    if (v > hi) return (uint8_t)hi;
    return (uint8_t)v;
}

void SpeedPI_Init(void)
{
    memset(&s_spi, 0, sizeof(s_spi));
    s_spi.kp               = DEFAULT_SPEED_KP;
    s_spi.ki               = DEFAULT_SPEED_KI;
    s_spi.base_low         = DEFAULT_BASE_PWM_LOW;
    s_spi.base_mid         = DEFAULT_BASE_PWM_MID;
    s_spi.base_high        = DEFAULT_BASE_PWM_HIGH;
    s_spi.boost_low        = DEFAULT_BOOST_LOW_PWM;
    s_spi.boost_mid        = DEFAULT_BOOST_MID_PWM;
    s_spi.boost_high       = DEFAULT_BOOST_HIGH_PWM;
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

static uint8_t base_pwm_for_rpm(float abs_rpm)
{
    if (abs_rpm <= 30.0f)        return s_spi.base_low;
    else if (abs_rpm <= 150.0f)  return s_spi.base_mid;
    else                          return s_spi.base_high;
}

static uint8_t boost_pwm_for_rpm(float abs_rpm)
{
    if (abs_rpm <= 30.0f)        return s_spi.boost_low;
    else if (abs_rpm <= 150.0f)  return s_spi.boost_mid;
    else                          return s_spi.boost_high;
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
    float target_abs = (float)(t >= 0 ? t : -t);
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
        uint8_t boost = boost_pwm_for_rpm(s_spi.target_rpm_ramped);
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

    s_spi.computed_duty = clamp_u8((int)roundf(out),
                                   (int)SPEED_PI_MIN_PWM,
                                   (int)SPEED_PI_MAX_PWM);
    if (s_spi.computed_duty == 0U && s_spi.target_rpm_cmd != 0) {
        s_spi.computed_duty = 1U;
    }
}

float   SpeedPI_GetRampedTargetRpm(void) { return s_spi.target_rpm_ramped; }
int32_t SpeedPI_GetRawTargetRpm(void)    { return s_spi.target_rpm_cmd; }
uint8_t SpeedPI_GetComputedDuty(void)    { return s_spi.computed_duty; }
SpeedPhase SpeedPI_GetPhase(void)        { return s_spi.phase; }
SpeedFault SpeedPI_GetFault(void)        { return s_spi.fault; }

float SpeedPI_GetK(void)                 { return s_spi.kp; }
void  SpeedPI_SetKp(float kp)            { s_spi.kp = clampf(kp, 0.0f, 10.0f); }
void  SpeedPI_SetKi(float ki)            { s_spi.ki = clampf(ki, 0.0f, 10.0f); }
void SpeedPI_SetBasePwm(uint8_t low, uint8_t mid, uint8_t high)
{
    /* ISSUE-040: clamp each band to 0..SPEED_PI_MAX_PWM so a stray
     * `base` command cannot push the feed-forward above the PI
     * saturation limit. */
    uint8_t mx = SPEED_PI_MAX_PWM;
    s_spi.base_low  = (low  > mx) ? mx : low;
    s_spi.base_mid  = (mid  > mx) ? mx : mid;
    s_spi.base_high = (high > mx) ? mx : high;
}
void SpeedPI_SetBoostPwm(uint8_t low, uint8_t mid, uint8_t high, uint16_t ms)
{
    uint8_t mx = SPEED_PI_MAX_PWM;
    s_spi.boost_low  = (low  > mx) ? mx : low;
    s_spi.boost_mid  = (mid  > mx) ? mx : mid;
    s_spi.boost_high = (high > mx) ? mx : high;
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
void SpeedPI_GetBasePwm(uint8_t *low, uint8_t *mid, uint8_t *high)
{
    if (low)  *low  = s_spi.base_low;
    if (mid)  *mid  = s_spi.base_mid;
    if (high) *high = s_spi.base_high;
}
void SpeedPI_GetBoostPwm(uint8_t *low, uint8_t *mid, uint8_t *high, uint16_t *ms)
{
    if (low)  *low  = s_spi.boost_low;
    if (mid)  *mid  = s_spi.boost_mid;
    if (high) *high = s_spi.boost_high;
    if (ms)   *ms   = s_spi.boost_ms;
}
void SpeedPI_GetRampRates(float *up, float *down)
{
    if (up)   *up   = s_spi.ramp_up;
    if (down) *down = s_spi.ramp_down;
}
