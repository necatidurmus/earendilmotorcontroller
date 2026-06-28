/* ============================================================
 * App/Inc/app_types.h
 * Shared application types used across multiple modules.
 * Extracted from app_main.c — values must not change.
 * ============================================================ */
#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MODE_DUTY = 0,
    MODE_SPEED = 1
} AppMode;

typedef enum {
    PHASE_STOPPED  = 0,
    PHASE_RUNNING  = 1,
    PHASE_BRAKE    = 2,
    PHASE_NEUTRAL  = 3,
    PHASE_FAULT    = 4
} MotorPhase;

typedef enum {
    DIR_FWD = +1,
    DIR_REV = -1
} Direction;

#endif /* APP_TYPES_H */
