/* ============================================================
 * App/Inc/command/command_handlers_config.h
 * Config command handlers: pi, kp, ki, base, boost, ramp,
 * map, kick*, ramp*, defpwm, defaults, loadcfg, save*.
 * ============================================================ */
#ifndef COMMAND_HANDLERS_CONFIG_H
#define COMMAND_HANDLERS_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if cmd was handled by a config command. */
bool CommandHandlers_Config_Handle(char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_HANDLERS_CONFIG_H */
