/* ============================================================
 * App/Src/storage/config_snapshot.c
 * Config snapshot helpers.
 *
 * FromRuntime: reads live PI gains, base/boost, ramp, and
 * AppState kick/ramp/default/brake fields into a PersistentConfig_t.
 *
 * ApplyToRuntime:  writes PersistentConfig_t values back into
 * the live SpeedPI, AppState, and Telemetry modules, then
 * calls MotionControl_ClampLoadedConfig() and SpeedPI_Reset().
 *
 * Validate: checks all values are within acceptable limits.
 * ============================================================ */

#include "config_snapshot.h"
#include "speed_pi.h"
#include "app_state.h"
#include "app_config.h"
#include "motion_control.h"
#include "telemetry.h"

#include <math.h>
#include <string.h>

void ConfigSnapshot_FromRuntime(PersistentConfig_t *cfg)
{
    SpeedPI_GetGains(&cfg->kp, &cfg->ki);
    SpeedPI_GetBasePwm(cfg->base_pwm);
    SpeedPI_GetBoostPwm(cfg->boost_pwm, &cfg->boost_ms);
    SpeedPI_GetRampRates(&cfg->ramp_up, &cfg->ramp_down);

    AppState *s = AppState_Get();
    cfg->kick_enabled       = s->kick_enabled;
    cfg->ramp_enabled       = s->ramp_enabled;
    cfg->kick_duty          = s->kick_duty;
    cfg->kick_ms            = s->kick_ms;
    cfg->ramp_step          = s->ramp_step;
    cfg->ramp_interval_ms   = s->ramp_interval_ms;
    cfg->default_pwm        = s->default_pwm;
    cfg->brake_hold_ms      = s->brake_hold_ms;
    cfg->telemetry_interval_ms = Telemetry_GetIntervalMs();
}

void ConfigSnapshot_ApplyToRuntime(const PersistentConfig_t *cfg)
{
    SpeedPI_SetKp(cfg->kp);
    SpeedPI_SetKi(cfg->ki);
    SpeedPI_SetBasePwm(cfg->base_pwm);
    SpeedPI_SetBoostPwm(cfg->boost_pwm, cfg->boost_ms);
    SpeedPI_SetRamp(cfg->ramp_up, cfg->ramp_down);

    AppState *s = AppState_Get();
    s->kick_enabled       = cfg->kick_enabled;
    s->ramp_enabled       = cfg->ramp_enabled;
    s->kick_duty          = cfg->kick_duty;
    s->kick_ms            = cfg->kick_ms;
    s->ramp_step          = cfg->ramp_step;
    s->ramp_interval_ms   = cfg->ramp_interval_ms;
    s->default_pwm        = cfg->default_pwm;
    s->brake_hold_ms      = cfg->brake_hold_ms;

    Telemetry_SetIntervalMs(cfg->telemetry_interval_ms);

    /* Clamp any out-of-range values (safety net for old flash data). */
    MotionControl_ClampLoadedConfig();

    /* Reset PI integrator to avoid stale state. */
    SpeedPI_Reset();
}

bool ConfigSnapshot_Validate(const PersistentConfig_t *cfg)
{
    if (!isfinite(cfg->kp) || !isfinite(cfg->ki)) return false;
    if (!isfinite(cfg->ramp_up) || !isfinite(cfg->ramp_down)) return false;

    if (cfg->kp < 0.0f || cfg->kp > 100.0f) return false;
    if (cfg->ki < 0.0f || cfg->ki > 100.0f) return false;

    if (cfg->ramp_up <= 0.0f || cfg->ramp_up > 10000.0f) return false;
    if (cfg->ramp_down <= 0.0f || cfg->ramp_down > 10000.0f) return false;

    for (uint8_t i = 0U; i < STORAGE_BASE_PWM_COUNT; i++) {
        if (cfg->base_pwm[i] > PWM_MAX_DUTY) return false;
    }
    for (uint8_t i = 0U; i < STORAGE_BOOST_PWM_COUNT; i++) {
        if (cfg->boost_pwm[i] > PWM_MAX_DUTY) return false;
    }
    if (cfg->boost_ms > 1000U) return false;

    if (cfg->kick_duty > KICK_DUTY_MAX) return false;
    if (cfg->kick_ms > KICK_MS_MAX) return false;
    if (cfg->ramp_step > RAMP_STEP_MAX) return false;
    if (cfg->ramp_interval_ms > RAMP_INTERVAL_MS_MAX) return false;
    if (cfg->default_pwm > DEFAULT_PWM_MAX) return false;
    if (cfg->brake_hold_ms < 100U || cfg->brake_hold_ms > 10000U) return false;
    if (cfg->telemetry_interval_ms < 20U || cfg->telemetry_interval_ms > 5000U) return false;

    return true;
}
