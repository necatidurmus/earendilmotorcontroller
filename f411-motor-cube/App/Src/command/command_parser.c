/* ============================================================
 * App/Src/command/command_parser.c
 * UART command parser.  Trims and lowercases the input, then
 * delegates to the dispatcher.
 * ============================================================ */
#include "command_parser.h"
#include "command_dispatcher.h"
#include "app_utils.h"
#include "uart_protocol.h"

void CommandParser_Handle(char *cmd, UartSource src)
{
    (void)src;
    AppUtils_TrimInPlace(cmd);
    AppUtils_LowerInPlace(cmd);
    if (cmd[0] == '\0') return;

    if (!CommandDispatcher_Dispatch(cmd)) {
        UartProtocol_Print("\r\n[ERR] Unknown command");
    }
}
