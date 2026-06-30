#include "motion_controller.h"
#include "motor_dispatcher.h"
#include "motor_tx_dma.h"
#include "service_lock.h"
#include "logger.h"
#include <string.h>

/* ── Tunables ─────────────────────────────────────────────────────────────── */
#define STOP_TX_DRAIN_MS  100U

static MotorCmd_t motorCmds[MOTOR_COUNT];

static void SetMotor(MotorId_t id, MotorDir_t dir, uint16_t pwm)
{
    motorCmds[id].dir = dir;
    motorCmds[id].pwm = pwm;
}

static void SetAllMotors(MotorDir_t dir, uint16_t pwm)
{
    for (int i = 0; i < MOTOR_COUNT; i++)
        SetMotor((MotorId_t)i, dir, pwm);
}

static void SetMotorCmd(MotorId_t id, MotorDir_t dir, uint16_t pwm)
{
    motorCmds[id].dir = dir;
    motorCmds[id].pwm = pwm;
}

void MotionController_Init(void)
{
    memset(motorCmds, 0, sizeof(motorCmds));
}

void MotionController_Execute(const MotionCmd_t *cmd)
{
    if (cmd == NULL)
        return;

    uint16_t spd = cmd->speed;

    switch (cmd->direction)
    {
        case DIR_FORWARD:
            SetAllMotors(MCMD_FORWARD, spd);
            break;

        case DIR_BACKWARD:
            SetAllMotors(MCMD_BACKWARD, spd);
            break;

        case DIR_LEFT:
            SetMotorCmd(MOTOR_FL, MCMD_BACKWARD, spd);
            SetMotorCmd(MOTOR_FR, MCMD_FORWARD, spd);
            SetMotorCmd(MOTOR_RL, MCMD_BACKWARD, spd);
            SetMotorCmd(MOTOR_RR, MCMD_FORWARD, spd);
            break;

        case DIR_RIGHT:
            SetMotorCmd(MOTOR_FL, MCMD_FORWARD, spd);
            SetMotorCmd(MOTOR_FR, MCMD_BACKWARD, spd);
            SetMotorCmd(MOTOR_RL, MCMD_FORWARD, spd);
            SetMotorCmd(MOTOR_RR, MCMD_BACKWARD, spd);
            break;

        case DIR_STOP:
        default:
            SetAllMotors(MCMD_STOP, 0);
            break;
    }

    Logger_Log(LOG_INFO, "Motion: dir=%d spd=%d", cmd->direction, cmd->speed);
    MotorDispatcher_SendAll(motorCmds);
}

void MotionController_Stop(void)
{
    SetAllMotors(MCMD_STOP, 0);
    MotorDispatcher_SendAll(motorCmds);
    Logger_Log(LOG_INFO, "Motion: STOP");
}

void MotionController_DisarmSafe(void)
{
    /* Neutralize any stale motion state so a command queued before DISARM
     * can never execute after leaving DISARM.  This zeroes the internal
     * command table without relying on a fresh command arriving. */
    SetAllMotors(MCMD_STOP, 0);
}

/* ── Stop sequence variants ────────────────────────────────────────────────── */

bool MotionController_WaitForTxDrain(uint32_t timeoutMs)
{
    uint32_t start = HAL_GetTick();
    while (!MotorTxDma_AllIdle())
    {
        if ((HAL_GetTick() - start) >= timeoutMs)
            return false;
    }
    return true;
}

void MotionController_StopNormal(void)
{
    /* Normal stop: ramp down to zero, then stop.
     * 1. Cancel any queued (non-active) motion frame so `rpm 0` leaves
     *    immediately instead of sitting behind a pending motion frame.
     * 2. Send "rpm 0" to ramp down (F411 speed-PI ramps to zero).
     * 3. Wait for TX drain so the stop command arrives after rpm 0.
     * 4. Send "stop" to release/coast.
     * 5. Zero the internal motor command table. */
    MotorTxDma_CancelPending();
    MotorDispatcher_SendRaw("rpm 0");
    MotionController_WaitForTxDrain(STOP_TX_DRAIN_MS);
    MotorDispatcher_SendRaw("stop");
    MotionController_Stop();
    Logger_Log(LOG_INFO, "[STOP] Normal stop sequence sent");
}

void MotionController_StopCoast(void)
{
    /* Coast stop: send "safe" (F411 coast, no fault latch), then "stop".
     * Belt-and-suspenders: the stop command after safe ensures the
     * motor controller is fully in STOPPED phase, not just NEUTRAL. */
    MotorTxDma_CancelPending();
    MotorDispatcher_SendRaw("safe");
    MotionController_WaitForTxDrain(STOP_TX_DRAIN_MS);
    MotorDispatcher_SendRaw("stop");
    MotionController_Stop();
    Logger_Log(LOG_INFO, "[STOP] Coast stop sequence sent (no fault latch)");
}

void MotionController_StopEstop(void)
{
    /* Emergency stop: immediate estop to all motors.
     * Also revokes service unlock (dangerous commands blocked again). */
    MotorTxDma_CancelPending();
    MotorDispatcher_SendRaw("estop");
    MotionController_Stop();
    ServiceLock_Lock();
    Logger_Log(LOG_INFO, "[STOP] ESTOP sent, service locked");
}
