/* ============================================================
 * App/Inc/command/command_dispatcher.h
 * Routes a parsed command string to the correct handler.
 * ============================================================ */
#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch a trimmed, lowercased command string.
 * Returns true if a handler consumed the command. */
bool CommandDispatcher_Dispatch(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_DISPATCHER_H */
