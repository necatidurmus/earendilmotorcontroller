/* ============================================================
 * App/Src/motion/motion_reverse.c
 * Neutral/reverse switching logic for direction changes.
 * ============================================================ */
#include "motion_reverse.h"
#include "app_state.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "stm32f4xx_hal.h"

void MotionControl_BeginNeutralSwitch(int8_t new_direction)
{
    AppState *s = AppState_Get();
    MotorDriver_AllOff();
    s->pending_rpm_target = SpeedPI_GetRawTargetRpm();
    SpeedPI_Disable();
    s->current_duty = 0U;
    s->ramp_current_duty = 0U;
    s->kick_active = false;
    s->kick_start_ms = 0U;
    s->last_ramp_update_ms = HAL_GetTick();
    s->has_ever_run = false;
    s->last_edge_count = HallSensor_GetEdgeCounter();
    s->last_edge_ms = HAL_GetTick();
    s->phase = PHASE_NEUTRAL;
    s->pending_direction = new_direction;
    s->neutral_release_ms = HAL_GetTick();
    s->reversal_waiting = true;
    s->reversal_pending_dir = new_direction;
    s->reversal_start_ms = HAL_GetTick();
}
