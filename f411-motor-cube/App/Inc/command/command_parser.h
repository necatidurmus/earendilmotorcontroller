/* ============================================================
 * App/Inc/command_parser.h
 * UART command parser and dispatch.
 * ============================================================ */
#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include "uart_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void CommandParser_Handle(char *cmd, UartSource src);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_PARSER_H */
