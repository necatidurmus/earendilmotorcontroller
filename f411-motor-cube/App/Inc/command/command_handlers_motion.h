/* ============================================================
 * App/Inc/command/command_handlers_motion.h
 * Motion command handlers: f, b, stop, pwm, mode, rpm.
 * ============================================================ */
#ifndef COMMAND_HANDLERS_MOTION_H
#define COMMAND_HANDLERS_MOTION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if cmd was handled by a motion command. */
bool CommandHandlers_Motion_Handle(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_HANDLERS_MOTION_H */
