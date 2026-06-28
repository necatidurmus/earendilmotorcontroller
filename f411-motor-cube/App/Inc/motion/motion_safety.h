/* ============================================================
 * App/Inc/motion/motion_safety.h
 * Safety guard queries for motion control.
 * ============================================================ */
#ifndef MOTION_SAFETY_H
#define MOTION_SAFETY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if no fault is latched. */
bool MotionControl_Allowed(void);

/* True if gate test or service task is active. */
bool MotionControl_ServiceBusy(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_SAFETY_H */
