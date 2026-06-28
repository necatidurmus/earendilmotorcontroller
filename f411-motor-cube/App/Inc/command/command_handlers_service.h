/* ============================================================
 * App/Inc/command/command_handlers_service.h
 * Service command handlers: arm, disarm, identify, scan,
 * test, gatetest.
 * ============================================================ */
#ifndef COMMAND_HANDLERS_SERVICE_H
#define COMMAND_HANDLERS_SERVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if cmd was handled by a service command. */
bool CommandHandlers_Service_Handle(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_HANDLERS_SERVICE_H */
