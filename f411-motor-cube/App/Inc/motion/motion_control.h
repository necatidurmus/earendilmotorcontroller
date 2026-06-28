/* ============================================================
 * App/Inc/motion/motion_control.h
 * Motor state machine — duty/speed run logic, kick/ramp,
 * Hall freshness checks.
 *
 * Sub-modules (included here for backward compatibility):
 *   motion_safety.h  — Allowed(), ServiceBusy()
 *   motion_reverse.h — BeginNeutralSwitch()
 * ============================================================ */
#ifndef MOTION_CONTROL_H
#define MOTION_CONTROL_H

#include "app_types.h"
#include "motion_safety.h"
#include "motion_reverse.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void MotionControl_Init(void);
void MotionControl_Service(void);

void MotionControl_StopImmediate(void);
void MotionControl_ClampLoadedConfig(void);

void MotionControl_RequestRun(Direction dir, uint16_t duty);
void MotionControl_RequestStop(void);
void MotionControl_RequestDutyUpdate(uint16_t duty);
void MotionControl_RequestBrake(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_CONTROL_H */
