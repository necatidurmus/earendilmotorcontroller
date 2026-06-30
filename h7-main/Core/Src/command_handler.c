#include "command_handler.h"
#include "control_mode.h"
#include "motion_controller.h"
#include "motor_dispatcher.h"
#include "motor_tx_dma.h"
#include "activity_light.h"
#include "operating_mode.h"
#include "safety_manager.h"
#include "service_lock.h"
#include "telemetry_bridge.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* ── Tunables ───────────────────────────────────────────────────────────────
 *  Bounded wait for the motor TX DMA path to drain during a synchronized
 *  control-mode switch.  This is a main-loop context call (never ISR), so a
 *  short blocking poll on the TX busy/pending flags is safe and bounded. */
#define MODE_SWITCH_TX_DRAIN_MS 100U

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Returns the short terminal prefix for a motion command ("f", "fd", ...). */
static const char *MotionPrefix(Direction_t dir, bool isDuty)
{
    switch (dir)
    {
        case DIR_FORWARD:  return isDuty ? "fd" : "f";
        case DIR_BACKWARD: return isDuty ? "bd" : "b";
        case DIR_RIGHT:    return isDuty ? "rd" : "r";
        case DIR_LEFT:     return isDuty ? "ld" : "l";
        default:           return "?";
    }
}

/* Direct-motor raw payloads that are safe in DISARM (queries / stop /
 * brake / the documented control-mode switches).  Used by the DISARM
 * gate to reject motion-causing raw payloads like `FL f100` while still
 * allowing `FL status`, `FL stop`, `FL x`,
 * `FL mode speed`, `FL mode duty`.
 *
 * NOTE: "identify" is NOT safe — it requires service unlock. */
static bool IsSafeRawPayload(const char *p)
{
    if (p == NULL)
        return false;
    return (strcmp(p, "status")      == 0 ||
            strcmp(p, "hall")         == 0 ||
            strcmp(p, "h")            == 0 ||
            strcmp(p, "stop")         == 0 ||
            strcmp(p, "s")            == 0 ||
            strcmp(p, "x")            == 0 ||
            strcmp(p, "brake")        == 0 ||
            strcmp(p, "mode speed")   == 0 ||
            strcmp(p, "mode duty")    == 0 ||
            strcmp(p, "mode normal")  == 0 ||
            strcmp(p, "mode control") == 0);
}

static const char *MotorTagName(MotorId_t id)
{
    switch (id)
    {
        case MOTOR_FL: return "FL";
        case MOTOR_FR: return "FR";
        case MOTOR_RL: return "RL";
        case MOTOR_RR: return "RR";
        default:        return "??";
    }
}

/* ── Command classification helpers ─────────────────────────────────────────
 *  Used by the DISARM gate to decide what is allowed while locked. */

bool Command_IsModeTransition(const TerminalCommand_t *cmd)
{
    return (cmd != NULL && cmd->type == TCMD_OP_MODE);
}

bool Command_IsMotionCommand(const TerminalCommand_t *cmd)
{
    return (cmd != NULL && cmd->type == TCMD_MOTION);
}

/* ── Operating-mode transition ──────────────────────────────────────────────
 *  Enforces the DISARM safety lock side-effects: on entering DISARM all
 *  motors are safe-zeroed and stale motion is neutralized; on leaving,
 *  motors stay stopped and a fresh command is required to move. */
static void HandleOperatingMode(RoverMode_t target)
{
    if (target == ROVER_MODE_DISARM)
    {
        OperatingMode_Set(ROVER_MODE_DISARM);
        SafetyManager_EnterDisarm();
        Logger_Log(LOG_INFO, "[MODE] DISARM active, motion commands locked");
        return;
    }

    /* Leaving DISARM -> MANUAL or AUTONOMOUS */
    OperatingMode_Set(target);
    SafetyManager_LeaveDisarm();

    if (target == ROVER_MODE_MANUAL)
        Logger_Log(LOG_INFO, "[MODE] MANUAL active");
    else if (target == ROVER_MODE_AUTONOMOUS)
        Logger_Log(LOG_INFO, "[MODE] AUTONOMOUS active");
    else
        Logger_Log(LOG_INFO, "[MODE] %s active", OperatingMode_ToString(target));

    Logger_Log(LOG_INFO, "Motors stopped; send a motion command to move");
}

/* ── Synchronized control-mode switch (RPM <-> PWM) ──────────────────────────
 *  The F411 motor controllers reject `mode speed` / `mode duty` while a motor
 *  is still running, replying e.g. "[ERR] Stop motor first".  That would leave
 *  H7 and F411 desynchronized (H7 believes it changed mode, F411 did not).
 *
 *  Safe policy implemented here:
 *    1. Announce the switch.
 *    2. Stop all motors first (`stop`), dropping any queued motion frame so
 *       the stop leaves immediately instead of being stalled behind a pending
 *       motion frame.
 *    3. Wait for the stop frame to fully drain on every motor UART (bounded
 *       poll on the TX DMA busy/pending flags).
 *    4. Send the requested `mode speed` / `mode duty` to all controllers.
 *    5. Only after every channel accepted the frame for TX dispatch, update
 *       the local control mode. */
static bool WaitForTxDrain(uint32_t timeoutMs)
{
    return MotionController_WaitForTxDrain(timeoutMs);
}

static void HandleControlModeSwitch(ControlMode_t target)
{
    const char *modeName = (target == CONTROL_MODE_RPM) ? "SPEED" : "DUTY";
    const char *modeCmd  = (target == CONTROL_MODE_RPM) ? "mode speed" : "mode duty";

    Logger_Log(LOG_INFO, "[MODE] Switching motor controllers to %s...", modeName);

    /* 1. Stop all motors first so the F411s accept the mode command.
     *    Cancel any queued (non-active) motion frame so `stop` leaves
     *    immediately rather than sitting behind a pending motion frame. */
    Logger_Log(LOG_INFO, "[MODE] Sending stop before mode change");
    MotorTxDma_CancelPending();
    MotorDispatcher_SendRaw("stop");

    /* 2. Let the stop frame fully drain on every channel before issuing the
     *    mode command—otherwise the mode command could be staged behind the
     *    stop and arrive too early (or be dropped by the pending-slot policy). */
    if (!WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS))
    {
        Logger_Log(LOG_ERROR,
                   "[MODE] Stop TX did not drain within %ums; mode switch aborted, "
                   "local control mode unchanged (%s)",
                   (unsigned)MODE_SWITCH_TX_DRAIN_MS,
                   ControlMode_ToString(ControlMode_Get()));
        return;
    }

    /* 3. Dispatch the mode command to all motor controllers. */
    if (!MotorDispatcher_SendRaw(modeCmd))
    {
        Logger_Log(LOG_ERROR,
                   "[MODE] Failed to queue '%s' to one or more motors; "
                   "local control mode unchanged (%s)",
                   modeCmd,
                   ControlMode_ToString(ControlMode_Get()));
        return;
    }
    Logger_Log(LOG_INFO, "[MODE] '%s' dispatched to all motor UARTs", modeCmd);

    /* 4. Only now advance the local control mode — after successful TX
     *    dispatch of the mode command.  Subsequent motion commands (e.g.
     *    f100 in RPM mode) will then be encoded as `rpm 100` / `rpm -100`. */
    ControlMode_Set(target);
    Logger_Log(LOG_INFO, "[MODE] Local control mode set to %s", modeName);
    Logger_Log(LOG_INFO,
               "[MODE] Mode command dispatched, not fully ACK-confirmed "
               "(F411 ACK parsing not wired for raw commands)");
}

/* ── Handle identify for a specific motor ───────────────────────────────────
 *  Sends arm command, waits for TX drain, then sends identify. */
static void HandleIdentifyMotor(MotorId_t motor)
{
    char frame[64];
    int len = snprintf(frame, sizeof(frame),
                       "arm service CURRENT_LIMITED_BENCH_SUPPLY");
    if (len <= 0 || (uint16_t)len >= sizeof(frame))
    {
        Logger_Log(LOG_ERROR, "[IDENTIFY] Arm frame too long");
        return;
    }

    MotorDispatcher_SendRawToMotor(motor, frame);
    if (!WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS))
    {
        Logger_Log(LOG_ERROR,
                   "[IDENTIFY] Arm TX drain timeout for %s; identify not sent",
                   MotorTagName(motor));
        return;
    }
    MotorDispatcher_SendRawToMotor(motor, "identify");
    Logger_Log(LOG_INFO, "[IDENTIFY] Sent to %s", MotorTagName(motor));
}

/* ── Handle global identify (all motors) ──────────────────────────────────── */
static void HandleIdentifyAll(void)
{
    MotorDispatcher_SendRaw("arm service CURRENT_LIMITED_BENCH_SUPPLY");
    if (!WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS))
    {
        Logger_Log(LOG_ERROR,
                   "[IDENTIFY] Arm TX did not drain within %ums; "
                   "identify not sent",
                   (unsigned)MODE_SWITCH_TX_DRAIN_MS);
        return;
    }
    MotorDispatcher_SendRaw("identify");
    Logger_Log(LOG_INFO, "[IDENTIFY] Sent to all motors");
}

/* ── Bridge command handler ────────────────────────────────────────────────── */
static void HandleBridgeCommand(const TerminalCommand_t *cmd)
{
    switch (cmd->bridgeAction)
    {
        case 0: /* BRIDGE_ON */
            TelemetryBridge_SetEnabled(true);
            Logger_Log(LOG_INFO, "bridge=ON (telemetry forwarding enabled)");
            break;

        case 1: /* BRIDGE_OFF */
            TelemetryBridge_SetEnabled(false);
            Logger_Log(LOG_INFO, "bridge=OFF (telemetry forwarding disabled; "
                                "RX processing still active)");
            break;

        case 2: /* BRIDGE_STATUS */
        {
            ServiceLockStatus_t sls;
            ServiceLock_GetStatus(&sls);
            Logger_Log(LOG_INFO, "bridge=%s service=%s",
                       TelemetryBridge_IsEnabled() ? "ON" : "OFF",
                       sls.unlocked ? "UNLOCKED" : "LOCKED");
            if (sls.unlocked && sls.remaining_ms > 0)
            {
                Logger_Log(LOG_INFO, "unlock_remain_ms=%lu blocked_cmds=%lu",
                           (unsigned long)sls.remaining_ms,
                           (unsigned long)sls.blocked_count);
            }
            else
            {
                Logger_Log(LOG_INFO, "blocked_cmds=%lu",
                           (unsigned long)sls.blocked_count);
            }
            break;
        }

        case 3: /* BRIDGE_UNLOCK_SERVICE */
            if (ServiceLock_TryUnlock(cmd->bridgeToken))
            {
                Logger_Log(LOG_INFO, "service unlocked for %us",
                           (unsigned)(SERVICE_TIMEOUT_MS / 1000));
            }
            else
            {
                Logger_Log(LOG_ERROR,
                           "usage: bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY");
            }
            break;

        case 4: /* BRIDGE_LOCK_SERVICE */
            ServiceLock_Lock();
            Logger_Log(LOG_INFO, "service locked");
            break;

        default:
            Logger_Log(LOG_ERROR,
                       "usage: bridge on/off/status/unlock_service/lock_service");
            break;
    }
}

/* ── Service command handler (alias) ────────────────────────────────────────── */
static void HandleServiceCommand(const TerminalCommand_t *cmd)
{
    switch (cmd->serviceAction)
    {
        case 0: /* SERVICE_UNLOCK */
            if (ServiceLock_TryUnlock(cmd->serviceToken))
            {
                Logger_Log(LOG_INFO, "service unlocked for %us",
                           (unsigned)(SERVICE_TIMEOUT_MS / 1000));
            }
            else
            {
                Logger_Log(LOG_ERROR,
                           "usage: service unlock CURRENT_LIMITED_BENCH_SUPPLY");
            }
            break;

        case 1: /* SERVICE_LOCK */
            ServiceLock_Lock();
            Logger_Log(LOG_INFO, "service locked");
            break;

        case 2: /* SERVICE_STATUS */
        {
            ServiceLockStatus_t sls;
            ServiceLock_GetStatus(&sls);
            Logger_Log(LOG_INFO, "service=%s",
                       sls.unlocked ? "UNLOCKED" : "LOCKED");
            if (sls.unlocked && sls.remaining_ms > 0)
            {
                Logger_Log(LOG_INFO, "unlock_remain_ms=%lu blocked_cmds=%lu",
                           (unsigned long)sls.remaining_ms,
                           (unsigned long)sls.blocked_count);
            }
            else
            {
                Logger_Log(LOG_INFO, "blocked_cmds=%lu",
                           (unsigned long)sls.blocked_count);
            }
            break;
        }

        default:
            Logger_Log(LOG_ERROR,
                       "usage: service unlock/lock/status");
            break;
    }
}

/* ── Public functions ─────────────────────────────────────────────────────── */

void CommandHandler_PrintHelp(void)
{
    Logger_Log(LOG_INFO, "Available commands:");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Control mode:");
    Logger_Log(LOG_INFO, "  m speed         Set RPM mode and forward \"mode speed\"");
    Logger_Log(LOG_INFO, "  m duty          Set PWM/duty mode and forward \"mode duty\"");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Rover mode (operating mode):");
    Logger_Log(LOG_INFO, "  mode             Show current rover mode (disarm/manual/auto)");
    Logger_Log(LOG_INFO, "  mode disarm      Disarm rover (red LED, STOP motors, lock motion)");
    Logger_Log(LOG_INFO, "  mode manual      Manual mode (green LED, motors stopped)");
    Logger_Log(LOG_INFO, "  mode auto        Autonomous mode (yellow LED, motors stopped)");
    Logger_Log(LOG_INFO, "  mode autonomous  Alias for 'mode auto'");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "RPM mode commands:");
    Logger_Log(LOG_INFO, "  f0..f200         Forward RPM command");
    Logger_Log(LOG_INFO, "  b0..b200         Backward RPM command");
    Logger_Log(LOG_INFO, "  r0..r200         Right turn RPM command");
    Logger_Log(LOG_INFO, "  l0..l200         Left turn RPM command");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "PWM mode commands:");
    Logger_Log(LOG_INFO, "  fd0..fd4000      Forward PWM/duty command");
    Logger_Log(LOG_INFO, "  bd0..bd4000      Backward PWM/duty command");
    Logger_Log(LOG_INFO, "  rd0..rd4000      Right turn PWM/duty command");
    Logger_Log(LOG_INFO, "  ld0..ld4000      Left turn PWM/duty command");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Common commands:");
    Logger_Log(LOG_INFO, "  stop             Normal stop (rpm 0 + stop)");
    Logger_Log(LOG_INFO, "  safe / alloff    Coast stop (no fault latch)");
    Logger_Log(LOG_INFO, "  brake            Active brake: x + stop");
    Logger_Log(LOG_INFO, "  estop            Emergency stop (all motors, locks service)");
    Logger_Log(LOG_INFO, "  identify         Send identify to all motors (requires service unlock)");
    Logger_Log(LOG_INFO, "  status           Send status to all motor UARTs");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Service lock (dangerous command gate):");
    Logger_Log(LOG_INFO, "  service unlock CURRENT_LIMITED_BENCH_SUPPLY  Unlock for 30s");
    Logger_Log(LOG_INFO, "  service lock                                 Re-lock");
    Logger_Log(LOG_INFO, "  service status                                Show lock status");
    Logger_Log(LOG_INFO, "  bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY  (F446 compat)");
    Logger_Log(LOG_INFO, "  bridge lock_service                                 (F446 compat)");
    Logger_Log(LOG_INFO, "  bridge status                                       Bridge + lock status");
    Logger_Log(LOG_INFO, "  bridge on/off                                       Telemetry forwarding");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Direct motor command:");
    Logger_Log(LOG_INFO, "  FL <text>        Send raw text only to Front Left motor");
    Logger_Log(LOG_INFO, "  FR <text>        Send raw text only to Front Right motor");
    Logger_Log(LOG_INFO, "  RL <text>        Send raw text only to Rear Left motor");
    Logger_Log(LOG_INFO, "  RR <text>        Send raw text only to Rear Right motor");
    Logger_Log(LOG_INFO, "Examples:");
    Logger_Log(LOG_INFO, "  FL status");
    Logger_Log(LOG_INFO, "  FR identify  (requires service unlock)");
    Logger_Log(LOG_INFO, "  RL f100");
    Logger_Log(LOG_INFO, "  RR mode speed");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "Motor tuning (per-motor or ALL):");
    Logger_Log(LOG_INFO, "  FL base P1 P2 P3 P4 P5 P6 P7 P8");
    Logger_Log(LOG_INFO, "  FL boost P1 P2 P3 P4 P5 P6 P7 P8 MS");
    Logger_Log(LOG_INFO, "  FL kickduty VALUE    (or: FL kick duty VALUE)");
    Logger_Log(LOG_INFO, "  FL kickms VALUE      (or: FL kick ms VALUE)");
    Logger_Log(LOG_INFO, "  FL ramp UP DOWN");
    Logger_Log(LOG_INFO, "  FL pi KP KI");
    Logger_Log(LOG_INFO, "  FL telper MS");
    Logger_Log(LOG_INFO, "  ALL base / boost / kickduty / kickms / ramp / pi / telper");
    Logger_Log(LOG_INFO, "");
    Logger_Log(LOG_INFO, "  help             Show this command list");
}

void CommandHandler_Handle(const TerminalCommand_t *cmd)
{
    if (cmd == NULL)
        return;

    /* ── Primary DISARM safety gate ────────────────────────────────────
     * While DISARM is active, only mode transitions, harmless query/
     * stop/brake/safe/estop commands, and bridge/service status are
     * accepted.  Every motion-causing or control-changing command is
     * rejected so the rover cannot move.
     *
     * Service unlock and service status are allowed in DISARM because
     * they need to be accessible before any dangerous command can work.
     * Estop is allowed because it's an emergency shutdown. */
    if (OperatingMode_IsDisarm())
    {
        bool allowed = false;
        switch (cmd->type)
        {
            case TCMD_OP_MODE:      /* mode disarm/manual/auto/autonomous */
            case TCMD_HELP:         /* help */
            case TCMD_STATUS:       /* status (query) */
            case TCMD_HALL:         /* hall (query) */
            case TCMD_MODE_QUERY:   /* mode (query) */
            case TCMD_STOP:         /* stop (safe) */
            case TCMD_BRAKE:        /* brake (safe) */
            case TCMD_ESTOP:        /* estop (emergency, always allowed) */
            case TCMD_SAFE:         /* safe/alloff (safe, coast stop) */
            case TCMD_BRIDGE:       /* bridge status/unlock/lock */
            case TCMD_SERVICE:      /* service unlock/lock/status */
                allowed = true;
                break;

            case TCMD_MOTOR_TUNE:
                /* telper is safe; other tune commands need MANUAL + service unlock
                 * so they are blocked in DISARM. */
                allowed = (cmd->tuneKind == TUNE_KIND_TELPER);
                if (!allowed)
                {
                    Logger_Log(LOG_WARN,
                               "[DISARM] Tuning command blocked (except telper)");
                    return;
                }
                break;

            case TCMD_IDENTIFY:
                /* Identify is a dangerous service command — blocked in DISARM
                 * even with service unlock (requires MANUAL mode). */
                Logger_Log(LOG_WARN, "[DISARM] identify blocked. "
                           "Use: mode manual, then service unlock, then identify");
                return;

            case TCMD_MOTOR_RAW:
                /* Empty payload (bare "FL") -> usage error, emit here so
                 * the handler's main switch does not need a DISARM copy. */
                if (cmd->rawPayload[0] == '\0')
                {
                    Logger_Log(LOG_ERROR,
                               "Usage: FL <text> | FR <text> | "
                               "RL <text> | RR <text>");
                    return;
                }
                /* Otherwise only safe payloads may pass; motion-causing
                 * raw commands (e.g. FL f100) are blocked.
                 * Dangerous service commands (identify, gatetest) also blocked
                 * in DISARM regardless of service unlock. */
                if (ServiceLock_IsDangerousCmd(cmd->rawPayload))
                {
                    Logger_Log(LOG_WARN,
                               "[DISARM] %s blocked (dangerous command). "
                               "Switch to MANUAL mode first.", cmd->rawPayload);
                    return;
                }
                allowed = IsSafeRawPayload(cmd->rawPayload);
                if (!allowed)
                {
                    Logger_Log(LOG_WARN,
                               "[DISARM] Direct motor command blocked");
                    return;
                }
                break;

            default:
                allowed = false;
                break;
        }

        if (!allowed)
        {
            Logger_Log(LOG_WARN, "[DISARM] Command ignored. Change mode first.");
            return;
        }
    }

    switch (cmd->type)
    {
        case TCMD_HELP:
            CommandHandler_PrintHelp();
            break;

        case TCMD_OP_MODE:
            HandleOperatingMode(cmd->opMode);
            break;

        case TCMD_STOP:
            MotionController_StopNormal();
            break;

        case TCMD_ESTOP:
            MotionController_StopEstop();
            break;

        case TCMD_SAFE:
            MotionController_StopCoast();
            break;

        case TCMD_MOTION:
        {
            /* Report clamping first (was previously emitted by the parser). */
            if (cmd->wasClamped)
            {
                Logger_Log(LOG_WARN, "%s value %u clamped to %u",
                           MotionPrefix(cmd->motion.direction, cmd->isDuty),
                           cmd->originalValue, cmd->value);
            }

            ControlMode_t mode = ControlMode_Get();

            if (cmd->isDuty && mode != CONTROL_MODE_PWM)
            {
                Logger_Log(LOG_ERROR, "Invalid mode: duty commands require PWM mode");
                break;
            }

            if (!cmd->isDuty && mode != CONTROL_MODE_RPM)
            {
                Logger_Log(LOG_ERROR, "Invalid mode: RPM commands require RPM mode");
                break;
            }

            MotionController_Execute(&cmd->motion);
            break;
        }

        case TCMD_BRAKE:
            MotorDispatcher_SendRaw("x");
            if (MotionController_WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS))
            {
                MotorDispatcher_SendRaw("stop");
            }
            MotionController_Stop();
            Logger_Log(LOG_INFO, "[STOP] Brake sequence sent");
            break;

        case TCMD_MODE_RPM:
            HandleControlModeSwitch(CONTROL_MODE_RPM);
            break;

        case TCMD_MODE_PWM:
            HandleControlModeSwitch(CONTROL_MODE_PWM);
            break;

        case TCMD_MODE_QUERY:
            Logger_Log(LOG_INFO, "Rover mode: %s",
                       OperatingMode_ToString(OperatingMode_Get()));
            break;

        case TCMD_IDENTIFY:
            /* Global identify — requires service unlock */
            if (!ServiceLock_IsUnlocked())
            {
                ServiceLock_CheckCommand("identify", true);
                Logger_Log(LOG_ERROR,
                           "identify blocked: service locked. "
                           "Use: service unlock CURRENT_LIMITED_BENCH_SUPPLY");
                break;
            }
            HandleIdentifyAll();
            break;

        case TCMD_STATUS:
            MotorDispatcher_SendRaw("status");
            break;

        case TCMD_HALL:
            MotorDispatcher_SendRaw("hall");
            break;

        case TCMD_BRIDGE:
            HandleBridgeCommand(cmd);
            break;

        case TCMD_SERVICE:
            HandleServiceCommand(cmd);
            break;

        case TCMD_MOTOR_RAW:
        {
            /* Bare motor tag with no payload -> usage error.
             * (DISARM-empty-payload path already returned earlier.) */
            if (cmd->rawPayload[0] == '\0')
            {
                Logger_Log(LOG_ERROR,
                           "Usage: FL <text> | FR <text> | "
                           "RL <text> | RR <text>");
                break;
            }

            /* Service lock gate for dangerous commands (not in DISARM which
             * was already checked above).  This applies in MANUAL/AUTONOMOUS. */
            if (ServiceLock_IsDangerousCmd(cmd->rawPayload))
            {
                if (!ServiceLock_IsUnlocked())
                {
                    ServiceLock_CheckCommand(cmd->rawPayload, true);
                    Logger_Log(LOG_ERROR,
                               "[SERVICE] %s blocked: service locked. "
                               "Use: service unlock CURRENT_LIMITED_BENCH_SUPPLY",
                               cmd->rawPayload);
                    break;
                }

                /* Per-motor identify: arm + identify with TX drain */
                if (strcmp(cmd->rawPayload, "identify") == 0)
                {
                    HandleIdentifyMotor(cmd->rawMotor);
                    break;
                }

                /* Per-motor scan: arm + scan */
                if (strcmp(cmd->rawPayload, "scan") == 0 ||
                    strcmp(cmd->rawPayload, "test") == 0 ||
                    strncmp(cmd->rawPayload, "gatetest ", 9) == 0)
                {
                    MotorDispatcher_SendRawToMotor(cmd->rawMotor,
                        "arm service CURRENT_LIMITED_BENCH_SUPPLY");
                    MotionController_WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS);
                }
            }

            Logger_Log(LOG_INFO, "[RAW][%s] %s",
                       MotorTagName(cmd->rawMotor), cmd->rawPayload);

            if (!MotorDispatcher_SendRawToMotor(cmd->rawMotor,
                                                cmd->rawPayload))
            {
                Logger_Log(LOG_ERROR, "Direct motor TX failed for %s",
                           MotorTagName(cmd->rawMotor));
            }
            break;
        }

        case TCMD_MOTOR_TUNE:
        {
            /* Tune komutlarının bir kısmı dangerous (base, boost, pi,
             * kickduty, kickms, ramp).  telper güvenli. */
            bool requiresUnlock = (cmd->tuneKind != TUNE_KIND_TELPER);

            if (requiresUnlock && !ServiceLock_IsUnlocked())
            {
                ServiceLock_CheckCommand(cmd->tunePayload, true);
                Logger_Log(LOG_ERROR,
                           "[SERVICE] %s blocked: service locked",
                           cmd->tunePayload);
                break;
            }

            /* For dangerous tune commands, send arm first */
            if (requiresUnlock)
            {
                MotorDispatcher_SendTunePayload(cmd->tuneTarget,
                    "arm service CURRENT_LIMITED_BENCH_SUPPLY");
                MotionController_WaitForTxDrain(MODE_SWITCH_TX_DRAIN_MS);
            }

            if (!MotorDispatcher_SendTunePayload(cmd->tuneTarget,
                                                 cmd->tunePayload))
            {
                Logger_Log(LOG_ERROR, "[TUNE] Dispatch failed");
            }
            break;
        }

        default:
            break;
    }
}
