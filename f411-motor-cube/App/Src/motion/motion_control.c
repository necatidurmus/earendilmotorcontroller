/* ============================================================
 * App/Src/motion_control.c
 * Motor state machine — stop, run request, neutral switch,
 * duty kick/ramp, Hall freshness, fault guards.
 * Extracted from app_main.c — behaviour must be identical.
 * ============================================================ */
#include "motion_control.h"
#include "app_state.h"
#include "app_config.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "speed_pi.h"
#include "fault_manager.h"
#include "service_task.h"
#include "uart_protocol.h"
#include "stm32f4xx_hal.h"

/* ---- Internal helpers ---- */

static void apply_duty_now(uint16_t duty)
{
    if (duty > PWM_MAX_DUTY) duty = PWM_MAX_DUTY;
    AppState *s = AppState_Get();
    s->current_duty = duty;
    MotorDriver_SetDuty(duty);
}

static void init_duty_start_runtime(void)
{
    AppState *s = AppState_Get();
    if (SpeedPI_IsEnabled()) return;
    if (s->kick_enabled && s->target_duty > 0U) {
        s->kick_active = true;
        s->kick_start_ms = HAL_GetTick();
        s->ramp_current_duty = s->kick_duty;
    } else {
        s->kick_active = false;
        if (s->ramp_enabled) {
            s->ramp_current_duty = 0U;
        } else {
            s->ramp_current_duty = s->target_duty;
        }
    }
    s->current_duty = s->ramp_current_duty;
    apply_duty_now(s->ramp_current_duty);
    s->last_ramp_update_ms = HAL_GetTick();
}

void MotionControl_ClampLoadedConfig(void)
{
    AppState *s = AppState_Get();
    if (s->kick_duty > KICK_DUTY_MAX) s->kick_duty = KICK_DUTY_MAX;
    if (s->kick_ms > KICK_MS_MAX) s->kick_ms = KICK_MS_MAX;
    if (s->ramp_step > RAMP_STEP_MAX) s->ramp_step = RAMP_STEP_MAX;
    if (s->ramp_interval_ms > RAMP_INTERVAL_MS_MAX) s->ramp_interval_ms = RAMP_INTERVAL_MS_MAX;
    if (s->default_pwm > DEFAULT_PWM_MAX) s->default_pwm = DEFAULT_PWM_MAX;
    if (s->brake_hold_ms < 100U) s->brake_hold_ms = 100U;
    if (s->brake_hold_ms > 10000U) s->brake_hold_ms = 10000U;
}

/* ---- Public API ---- */

void MotionControl_StopImmediate(void)
{
    AppState *s = AppState_Get();
    if (ServiceTask_IsActive()) ServiceTask_Cancel();
    s->gatetest_active = false;

    SpeedPI_Disable();
    MotorDriver_AllOff();

    s->phase = PHASE_STOPPED;
    s->direction = (Direction)0;
    s->pending_direction = 0;
    s->target_duty = 0U;
    s->current_duty = 0U;
    s->ramp_current_duty = 0U;
    s->kick_active = false;
    s->kick_start_ms = 0U;
    s->last_ramp_update_ms = 0U;
    s->run_request = false;
    s->stop_request = false;
    s->duty_update_request = false;
    s->last_motor_cmd_ms = 0U;
    s->has_ever_run = false;
    s->last_edge_count = HallSensor_GetEdgeCounter();
    s->last_edge_ms = 0U;
    s->gate_test_armed = false;
    s->service_armed = false;
    s->reversal_waiting = false;
    s->reversal_pending_dir = 0;
}

void MotionControl_RequestBrake(void)
{
    AppState *s = AppState_Get();

    MotionControl_StopImmediate();

    s->phase = PHASE_BRAKE;
    s->phase_start_ms = HAL_GetTick();

    /* Active brake shorts all three low-side MOSFETs.  If a hard fault
     * has set the safety lock, MotorDriver_ActiveBrake() degrades to
     * AllOff/coast automatically. */
    MotorDriver_ActiveBrake();
}

void MotionControl_RequestRun(Direction dir, uint16_t duty)
{
    AppState *s = AppState_Get();
    s->run_request = true;
    s->stop_request = false;
    s->direction = dir;
    s->pending_direction = (dir == DIR_FWD) ? +1 : -1;
    s->target_duty = duty;
}

void MotionControl_RequestStop(void)
{
    AppState_Get()->stop_request = true;
}

void MotionControl_RequestDutyUpdate(uint16_t duty)
{
    AppState *s = AppState_Get();
    s->target_duty = duty;
    if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
        s->duty_update_request = true;
        s->last_motor_cmd_ms = HAL_GetTick();
    }
}

void MotionControl_Init(void)
{
    /* Nothing extra — AppState_InitDefaults handles init. */
}

void MotionControl_Service(void)
{
    AppState *s = AppState_Get();

    /* Central fault guard.  Faults stop the motor immediately but are no
     * longer latched; a fresh motion command clears the fault and resumes. */
    if (FaultManager_GetLast() != FAULT_NONE) {
        MotionControl_StopImmediate();
        MotorDriver_SetSafetyLock(true);
        return;
    }

    /* RX overflow safety. */
    {
        uint32_t rx_drop = UartProtocol_GetRxDropCount();
        if (rx_drop != s->last_rx_drop_count) {
            s->last_rx_drop_count = rx_drop;
            if (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL) {
                FaultManager_Raise(FAULT_UART_RX_OVERFLOW);
                MotionControl_StopImmediate();
                UartProtocol_Print("\r\n[ERR] RX overflow during motor active");
                return;
            }
        }
    }

    /* Safety checks — must run whenever motor outputs are active,
     * including during service/gatetest tasks that energize the motor.
     * outputs_active covers: normal running, gate test driving, and
     * service tasks that apply motor steps (identify, test). Scan is
     * passive and does NOT set outputs_active. */
    bool outputs_active = (s->phase == PHASE_RUNNING)
                          || s->gatetest_active
                          || ServiceTask_IsDriving();
    if (outputs_active) {
        /* Immediate invalid raw Hall: kill outputs but do NOT latch a
         * fault yet — the persistent check below handles that after
         * INVALID_HALL_STOP_US (100 ms).  This mirrors the
         * PHASE_RUNNING drive path's IsCurrentRawValid guard. */
        if (!HallSensor_IsCurrentRawValid()) {
            MotorDriver_AllOff();
        }
        if (SpeedPI_IsEnabled() && SpeedPI_GetFault() == SPD_FAULT_NO_HALL) {
            FaultManager_Raise(FAULT_NO_HALL);
            MotionControl_StopImmediate();
            UartProtocol_Print("\r\n[ERR] SpeedPI: no Hall feedback");
            return;
        }
        HallFault hf = HallSensor_GetFault();
        if (hf == HALL_FAULT_INVALID_PERSIST) {
            FaultManager_Raise(FAULT_INVALID_HALL);
            MotionControl_StopImmediate();
            UartProtocol_Print("\r\n[ERR] Invalid Hall persisted");
            return;
        }
        if (hf == HALL_FAULT_ILLEGAL_TRANSITION) {
            FaultManager_Raise(FAULT_ILLEGAL_TRANSITION);
            MotionControl_StopImmediate();
            UartProtocol_Print("\r\n[ERR] Illegal Hall transition spam");
            return;
        }
    }

    /* Service/gatetest output ownership: safety checks above already
     * ran.  The service layer (ServiceTask_Update / GateTest_Service)
     * owns the motor outputs via MotorDriver_ApplyStep().  Do NOT
     * enter the normal motion state machine — it would AllOff() on
     * every iteration because phase != PHASE_RUNNING. */
    if (s->gatetest_active || ServiceTask_IsActive()) {
        /* Arming auto-disarm must still run during service/gatetest. */
        if (s->gate_test_armed && (HAL_GetTick() - s->gate_arm_start_ms) > ARM_TIMEOUT_MS) {
            s->gate_test_armed = false;
            UartProtocol_Print("\r\n[INFO] Gate test arming expired");
        }
        if (s->service_armed && (HAL_GetTick() - s->service_arm_start_ms) > ARM_TIMEOUT_MS) {
            s->service_armed = false;
            UartProtocol_Print("\r\n[INFO] Service arming expired");
        }
        return;
    }

    /* Apply deferred requests. */
    if (s->stop_request) {
        MotionControl_StopImmediate();
        s->stop_request = false;
    }
    if (s->duty_update_request) {
        if (!SpeedPI_IsEnabled() &&
            (s->phase == PHASE_RUNNING || s->phase == PHASE_NEUTRAL)) {
            if (s->ramp_enabled) {
                s->ramp_current_duty = s->current_duty;
                s->last_ramp_update_ms = HAL_GetTick();
            } else {
                s->ramp_current_duty = s->target_duty;
                s->current_duty = s->target_duty;
                apply_duty_now(s->target_duty);
            }
        }
        s->duty_update_request = false;
    }
    if (s->run_request) {
        int8_t req_dir;
        if (s->mode == MODE_SPEED) {
            int32_t t = SpeedPI_GetRawTargetRpm();
            req_dir = (t > 0) ? +1 : (t < 0 ? -1 : 0);
        } else {
            req_dir = (s->direction > 0) ? +1 : ((s->direction < 0) ? -1 : 0);
        }

        if (req_dir != 0 &&
            s->phase == PHASE_RUNNING && s->direction != 0 &&
            s->direction != (Direction)req_dir) {
            MotionControl_BeginNeutralSwitch(req_dir);
        } else if (req_dir != 0 &&
                   s->phase == PHASE_RUNNING &&
                   s->direction == (Direction)req_dir) {
            /* heartbeat */
        } else if (req_dir != 0 && s->phase != PHASE_NEUTRAL) {
            s->phase = PHASE_RUNNING;
            s->direction = (Direction)req_dir;
            s->phase_start_ms = HAL_GetTick();
            s->has_ever_run = false;
            s->last_edge_count = HallSensor_GetEdgeCounter();
            s->last_edge_ms = HAL_GetTick();
            init_duty_start_runtime();
        }
        s->run_request = false;
    }

    /* Neutral-wait phase. */
    if (s->phase == PHASE_NEUTRAL) {
        MotorDriver_AllOff();
        uint32_t now = HAL_GetTick();

        if (s->reversal_waiting) {
            uint32_t rpm = HallSensor_CalculateRpm();
            bool rpm_low = (rpm < 5U);
            bool timeout = ((now - s->reversal_start_ms) > 3000U);
            bool neutral_time = ((now - s->neutral_release_ms) >= DIRECTION_NEUTRAL_MS);

            if (rpm_low || timeout) {
                s->reversal_waiting = false;
                if (neutral_time) {
                    s->direction = (Direction)s->pending_direction;
                    s->phase = PHASE_RUNNING;
                    s->phase_start_ms = now;
                    s->has_ever_run = false;
                    s->last_edge_count = HallSensor_GetEdgeCounter();
                    s->last_edge_ms = now;
                    init_duty_start_runtime();
                    if (s->mode == MODE_SPEED) {
                        SpeedPI_Enable();
                        SpeedPI_SetTargetRpm(s->pending_rpm_target);
                    }
                }
            } else if (neutral_time && !rpm_low) {
                s->neutral_release_ms = now;
            }
            return;
        }

        if ((now - s->neutral_release_ms) >= DIRECTION_NEUTRAL_MS) {
            s->direction = (Direction)s->pending_direction;
            s->phase = PHASE_RUNNING;
            s->phase_start_ms = now;
            s->has_ever_run = false;
            s->last_edge_count = HallSensor_GetEdgeCounter();
            s->last_edge_ms = now;
            init_duty_start_runtime();
            if (s->mode == MODE_SPEED) {
                SpeedPI_Enable();
                SpeedPI_SetTargetRpm(s->pending_rpm_target);
            }
        }
        return;
    }

    /* Brake timeout. */
    if (s->phase == PHASE_BRAKE &&
        (HAL_GetTick() - s->phase_start_ms) >= s->brake_hold_ms) {
        MotionControl_StopImmediate();
        UartProtocol_Print("\r\n[WARN] Brake timeout");
    }

    /* SpeedPI computed duty. */
    uint16_t apply_duty = s->current_duty;
    if (SpeedPI_IsEnabled()) {
        apply_duty = SpeedPI_GetComputedDuty();
        s->current_duty = apply_duty;
    } else if (s->phase == PHASE_RUNNING) {
        uint32_t now = HAL_GetTick();
        if (s->kick_active) {
            if ((now - s->kick_start_ms) >= s->kick_ms) {
                s->kick_active = false;
                if (s->ramp_enabled) {
                    s->ramp_current_duty = s->kick_duty;
                    s->last_ramp_update_ms = now;
                } else {
                    s->ramp_current_duty = s->target_duty;
                }
                s->current_duty = s->ramp_current_duty;
                apply_duty_now(s->ramp_current_duty);
            }
            apply_duty = s->current_duty;
        } else if (s->ramp_enabled &&
                   s->ramp_current_duty != s->target_duty &&
                   s->ramp_interval_ms > 0U &&
                   (now - s->last_ramp_update_ms) >= s->ramp_interval_ms) {
            s->last_ramp_update_ms = now;
            if (s->ramp_current_duty < s->target_duty) {
                uint16_t delta = s->ramp_step;
                uint16_t gap = (uint16_t)(s->target_duty - s->ramp_current_duty);
                s->ramp_current_duty = (delta >= gap)
                    ? s->target_duty
                    : (uint16_t)(s->ramp_current_duty + delta);
            } else {
                uint16_t delta = s->ramp_step;
                uint16_t gap = (uint16_t)(s->ramp_current_duty - s->target_duty);
                s->ramp_current_duty = (delta >= gap)
                    ? s->target_duty
                    : (uint16_t)(s->ramp_current_duty - delta);
            }
            s->current_duty = s->ramp_current_duty;
            apply_duty = s->current_duty;
            apply_duty_now(s->ramp_current_duty);
        } else {
            apply_duty = s->current_duty;
        }
    }

    /* Drive motor outputs. */
    if (s->phase == PHASE_RUNNING) {
        if (!HallSensor_IsCurrentRawValid()) {
            MotorDriver_AllOff();
        } else {
            uint8_t state = HallSensor_GetMappedState();
            if (state <= 5U) {
                MotorDriver_ApplyStep(state, (int8_t)s->direction, apply_duty);
            } else {
                MotorDriver_AllOff();
            }
        }
    } else if (s->phase == PHASE_BRAKE) {
        MotorDriver_ActiveBrake();
    } else {
        MotorDriver_AllOff();
    }

    /* Hall freshness / startup timeout. */
    if (s->phase == PHASE_RUNNING) {
        uint32_t edges = HallSensor_GetEdgeCounter();
        if (edges != s->last_edge_count) {
            s->last_edge_count = edges;
            s->has_ever_run = true;
            s->last_edge_ms = HAL_GetTick();
        }

        if (HallSensor_GetFreshness() == HALL_STALE) {
            if (!s->has_ever_run) {
                if ((HAL_GetTick() - s->phase_start_ms) > START_NO_HALL_TIMEOUT_MS) {
                    FaultManager_Raise(FAULT_NO_HALL);
                    MotionControl_StopImmediate();
                    UartProtocol_Print("\r\n[ERR] No Hall edge in startup window");
                }
            } else if (SpeedPI_IsEnabled() &&
                       SpeedPI_GetRawTargetRpm() != 0 &&
                       (HAL_GetTick() - s->last_edge_ms) > RPM_FEEDBACK_TIMEOUT_MS) {
                FaultManager_Raise(FAULT_NO_HALL);
                MotionControl_StopImmediate();
                UartProtocol_Print("\r\n[ERR] Hall lost in speed mode");
            } else if (!SpeedPI_IsEnabled() &&
                       s->target_duty > 0U &&
                       s->has_ever_run &&
                       (HAL_GetTick() - s->last_edge_ms) > DUTY_HALL_LOSS_TIMEOUT_MS) {
                FaultManager_Raise(FAULT_NO_HALL);
                MotionControl_StopImmediate();
                UartProtocol_Print("\r\n[ERR] Hall lost in duty mode");
            }
        }
    }

    /* Arming auto-disarm. */
    if (s->gate_test_armed && (HAL_GetTick() - s->gate_arm_start_ms) > ARM_TIMEOUT_MS) {
        s->gate_test_armed = false;
        UartProtocol_Print("\r\n[INFO] Gate test arming expired");
    }
    if (s->service_armed && (HAL_GetTick() - s->service_arm_start_ms) > ARM_TIMEOUT_MS) {
        s->service_armed = false;
        UartProtocol_Print("\r\n[INFO] Service arming expired");
    }
}
