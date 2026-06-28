/* ============================================================
 * App/Src/command/command_dispatcher.c
 * Routes a trimmed, lowercased command string to the correct
 * handler category.  Dispatch order preserves the original
 * if-chain priority.
 * ============================================================ */
#include "command_dispatcher.h"
#include "command_handlers_motion.h"
#include "command_handlers_query.h"
#include "command_handlers_config.h"
#include "command_handlers_service.h"
#include "command_handlers_fault.h"

bool CommandDispatcher_Dispatch(char *cmd)
{
    /* Order matches the original command_parser.c if-chain:
     *   motion → query → config → service → fault */
    if (CommandHandlers_Motion_Handle(cmd))   return true;
    if (CommandHandlers_Query_Handle(cmd))    return true;
    if (CommandHandlers_Config_Handle(cmd))   return true;
    if (CommandHandlers_Service_Handle(cmd))  return true;
    if (CommandHandlers_Fault_Handle(cmd))    return true;
    return false;
}
