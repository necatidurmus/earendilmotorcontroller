#ifndef MOTION_CONTROLLER_H
#define MOTION_CONTROLLER_H

#include "rover_types.h"

/* ── Public API ─────────────────────────────────────────────────────────── */
void MotionController_Init(void);
void MotionController_Execute(const MotionCmd_t *cmd);
void MotionController_Stop(void);
void MotionController_DisarmSafe(void);  /* stop + neutralize stale motion state */

/* ── Stop sequence variants ────────────────────────────────────────────────
 *  These implement two-command stop sequences with TX drain between,
 *  guaranteeing the second command arrives after the first is fully
 *  transmitted (no pending-slot overwrite risk).
 *
 *  StopNormal:  rpm 0  -> TX drain -> stop   (rampa via F411, no fault)
 *  StopCoast:   safe   -> TX drain -> stop   (coast, no fault)
 *  StopEstop:   estop (immediate, also locks service) */

/* Bounded wait for the motor TX DMA path to drain during a stop
 * sequence.  Returns true if all channels drained within timeoutMs,
 * false if timeout expired (stop sequence still proceeds for safety). */
bool  MotionController_WaitForTxDrain(uint32_t timeoutMs);

void MotionController_StopNormal(void);   /* rpm 0 -> TX drain -> stop */
void MotionController_StopCoast(void);    /* safe  -> TX drain -> stop */
void MotionController_StopEstop(void);    /* estop direkt + service lock */

#endif /* MOTION_CONTROLLER_H */
