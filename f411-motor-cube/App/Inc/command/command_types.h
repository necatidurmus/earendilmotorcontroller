/* ============================================================
 * App/Inc/command/command_types.h
 * Command category enum. Used for documentation and dispatch.
 * ============================================================ */
#ifndef COMMAND_TYPES_H
#define COMMAND_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Command categories — matching handler file boundaries. */
typedef enum {
    CMD_CAT_MOTION   = 0,  /* f, b, stop, pwm, mode, rpm */
    CMD_CAT_QUERY    = 1,  /* status, help, hall, spstat, dbg, telper */
    CMD_CAT_CONFIG   = 2,  /* pi, kp, ki, base, boost, ramp, map, kick*, defpwm, defaults, loadcfg, save* */
    CMD_CAT_SERVICE  = 3,  /* arm, disarm, identify, scan, test, gatetest */
    CMD_CAT_FAULT    = 4,  /* clrerr */
    CMD_CAT_COUNT    = 5
} CommandCategory;

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_TYPES_H */
