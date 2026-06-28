/* ============================================================
 * App/Inc/command/command_handlers_query.h
 * Query/status command handlers: status, help, hall, spstat,
 * debug, dbg, telper.
 * ============================================================ */
#ifndef COMMAND_HANDLERS_QUERY_H
#define COMMAND_HANDLERS_QUERY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if cmd was handled by a query command. */
bool CommandHandlers_Query_Handle(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_HANDLERS_QUERY_H */
