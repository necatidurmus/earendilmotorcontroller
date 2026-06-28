/* ============================================================
 * App/Inc/command/command_handlers_fault.h
 * Fault command handlers: clrerr.
 * ============================================================ */
#ifndef COMMAND_HANDLERS_FAULT_H
#define COMMAND_HANDLERS_FAULT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if cmd was handled by a fault command. */
bool CommandHandlers_Fault_Handle(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_HANDLERS_FAULT_H */
