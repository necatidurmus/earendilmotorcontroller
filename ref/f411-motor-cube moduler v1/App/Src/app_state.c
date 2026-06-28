/* ============================================================
 * App/Src/app_state.c
 * Central application state — singleton instance.
 * ============================================================ */
#include "app_state.h"
#include "app_config.h"
#include <string.h>

static AppState s_state;

AppState *AppState_Get(void)
{
    return &s_state;
}

void AppState_InitDefaults(AppState *s)
{
    memset(s, 0, sizeof(*s));
    s->brake_hold_ms = BRAKE_HOLD_MS;
    s->target_duty = 0U;
    s->current_duty = 0U;
    s->mode = MODE_DUTY;
    s->kick_enabled = false;
    s->ramp_enabled = true;
    s->kick_duty = 60;
    s->kick_ms = 50;
    s->ramp_step = 8;
    s->ramp_interval_ms = 5;
    s->default_pwm = 100;
    s->gate_test_armed = false;
    s->service_armed = false;
    s->gate_arm_start_ms = 0U;
    s->service_arm_start_ms = 0U;
    s->reversal_waiting = false;
    s->reversal_pending_dir = 0;
    s->reversal_start_ms = 0U;
    s->last_rx_drop_count = 0U;
    s->hall_map_source = 0U;
    s->hall_map_dirty = false;
    s->candidate_active = false;
    memset(s->candidate_map, 255, sizeof(s->candidate_map));
    s->identify_last_result = 0U;
    s->identify_was_run = false;
}
