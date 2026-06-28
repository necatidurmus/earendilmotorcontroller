/* ============================================================
 * App/Inc/motion/motion_reverse.h
 * Neutral/reverse switching logic for direction changes.
 * ============================================================ */
#ifndef MOTION_REVERSE_H
#define MOTION_REVERSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enter neutral coast phase before reversing direction. */
void MotionControl_BeginNeutralSwitch(int8_t new_direction);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_REVERSE_H */
