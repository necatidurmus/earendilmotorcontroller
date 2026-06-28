/* ============================================================
 * App/Inc/app_state.h
 * Central application state.  All runtime state lives here.
 * ============================================================ */
#ifndef APP_STATE_H
#define APP_STATE_H

#include "app_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    AppMode     mode;
    MotorPhase  phase;
    Direction   direction;
    int8_t      pending_direction;

    uint16_t    target_duty;
    uint16_t    current_duty;

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
    uint32_t    last_edge_count;
    uint32_t    last_edge_ms;

    bool        verboseDebug;
    bool        queue_overflow;

    /* gatetest state */
    bool        gatetest_active;
    uint8_t     gatetest_sector;
    uint16_t    gatetest_duty;
    uint32_t    gatetest_start_ms;
    uint32_t    gatetest_timeout_ms;

    /* kick/ramp config (duty mode) */
    bool        kick_enabled;
    bool        ramp_enabled;
    uint16_t    kick_duty;
    uint16_t    kick_ms;
    uint16_t    ramp_step;
    uint16_t    ramp_interval_ms;
    uint16_t    default_pwm;
    uint32_t    last_ramp_update_ms;

    /* kick/ramp runtime state (duty mode) */
    bool        kick_active;
    uint32_t    kick_start_ms;
    uint16_t    ramp_current_duty;

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
    uint8_t     hall_map_source;
    bool        hall_map_dirty;

    /* Candidate map for safe-apply workflow */
    uint8_t     candidate_map[8];
    bool        candidate_active;

    /* Identify result tracking */
    uint8_t     identify_last_result;
    bool        identify_was_run;
} AppState;

/* Get pointer to the singleton app state. */
AppState *AppState_Get(void);

/* Initialize state to safe defaults. */
void AppState_InitDefaults(AppState *s);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
