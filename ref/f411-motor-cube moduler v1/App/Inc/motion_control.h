/* ============================================================
 * App/Inc/motion_control.h
 * Motor state machine — duty/speed run logic, neutral switch,
 * kick/ramp, Hall freshness checks.
 * ============================================================ */
#ifndef MOTION_CONTROL_H
#define MOTION_CONTROL_H

#include "app_types.h"
#include <stdint.h>
#include <stdbool.h>

void MotionControl_Init(void);
void MotionControl_Service(void);

bool MotionControl_Allowed(void);
bool MotionControl_ServiceBusy(void);
void MotionControl_StopImmediate(void);
void MotionControl_ClampLoadedConfig(void);

void MotionControl_RequestRun(Direction dir, uint8_t duty);
void MotionControl_RequestStop(void);
void MotionControl_RequestDutyUpdate(uint8_t duty);
void MotionControl_BeginNeutralSwitch(int8_t new_direction);

#endif /* MOTION_CONTROL_H */
