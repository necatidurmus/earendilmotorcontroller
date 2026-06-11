#include "command_parser.h"
#include "terminal_interface.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trimInPlace(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace(static_cast<unsigned char>(s[len - 1]))) {
        s[--len] = '\0';
    }
    char* start = s;
    while (*start && isspace(static_cast<unsigned char>(*start))) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void lowerInPlace(char* s) {
    while (s && *s) {
        *s = static_cast<char>(tolower(static_cast<unsigned char>(*s)));
        s++;
    }
}

static bool startsWithNoCase(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (tolower(static_cast<unsigned char>(*s)) != tolower(static_cast<unsigned char>(*prefix))) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

bool CommandParser::parseMotorName(const char* token, uint8_t& motorIdOut) {
    if (!token) return false;
    if (equalsIgnoreCase(token, "fl")) { motorIdOut = MOTOR_FL; return true; }
    if (equalsIgnoreCase(token, "rl")) { motorIdOut = MOTOR_RL; return true; }
    if (equalsIgnoreCase(token, "fr")) { motorIdOut = MOTOR_FR; return true; }
    if (equalsIgnoreCase(token, "rr")) { motorIdOut = MOTOR_RR; return true; }
    return false;
}

bool CommandParser::parsePWM(const char* token, int& pwm) {
    if (token == nullptr || *token == '\0') return false;

    const char* p = token;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }

    pwm = atoi(token);
    if (pwm < PWM_MIN || pwm > PWM_MAX) {
        return false;
    }

    return true;
}

bool CommandParser::parseInteger(const char* token, long& value) {
    if (token == nullptr || *token == '\0') {
        return false;
    }

    char* endPtr = nullptr;
    value = strtol(token, &endPtr, 10);
    return (endPtr != nullptr && *endPtr == '\0');
}

bool CommandParser::equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

CommandType CommandParser::identifyCommand(const char* token) {
    if (equalsIgnoreCase(token, "forward"))  return CMD_FORWARD;
    if (equalsIgnoreCase(token, "fwd"))      return CMD_FORWARD;
    if (equalsIgnoreCase(token, "w"))        return CMD_FORWARD;
    if (equalsIgnoreCase(token, "backward")) return CMD_BACKWARD;
    if (equalsIgnoreCase(token, "back"))     return CMD_BACKWARD;
    if (equalsIgnoreCase(token, "s"))        return CMD_BACKWARD;
    if (equalsIgnoreCase(token, "left"))     return CMD_LEFT;
    if (equalsIgnoreCase(token, "a"))        return CMD_LEFT;
    if (equalsIgnoreCase(token, "right"))    return CMD_RIGHT;
    if (equalsIgnoreCase(token, "d"))        return CMD_RIGHT;
    if (equalsIgnoreCase(token, "stop"))     return CMD_STOP;
    if (equalsIgnoreCase(token, "identify")) return CMD_IDENTIFY;
    if (equalsIgnoreCase(token, "id"))       return CMD_IDENTIFY;
    if (equalsIgnoreCase(token, "status"))   return CMD_STATUS;
    if (equalsIgnoreCase(token, "help"))     return CMD_HELP;
    if (equalsIgnoreCase(token, "h"))        return CMD_HELP;
    if (equalsIgnoreCase(token, "ping"))     return CMD_PING;
    if (equalsIgnoreCase(token, "estop"))    return CMD_ESTOP_STATUS;
    if (equalsIgnoreCase(token, "arm"))      return CMD_ARM;
    if (equalsIgnoreCase(token, "armbridge"))return CMD_ARMBRIDGE_STATUS;
    return CMD_INVALID;
}

Command CommandParser::parse(const char* line) {
    Command cmd;
    cmd.type = CMD_INVALID;
    cmd.pwm = 0;
    cmd.valid = false;
    cmd.error = nullptr;
    cmd.motorId = 0;
    cmd.armText[0] = '\0';

    if (line == nullptr || line[0] == '\0') {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    // Preserve payload case for arm commands
    if (startsWithNoCase(line, "arm") && (line[3] == '\0' || isspace(static_cast<unsigned char>(line[3])))) {
        const char* p = line + 3;
        while (*p && isspace(static_cast<unsigned char>(*p))) p++;
        if (*p == '\0') {
            cmd.error = "ERROR: arm command requires a payload. Example: arm stop";
            return cmd;
        }
        strncpy(cmd.armText, p, sizeof(cmd.armText) - 1);
        cmd.armText[sizeof(cmd.armText) - 1] = '\0';
        cmd.valid = true;
        cmd.type = CMD_ARM;
        return cmd;
    }

    char buf[TERMINAL_LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trimInPlace(buf);
    lowerInPlace(buf);

    if (buf[0] == '\0') {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    char* save = nullptr;
    char* tok1 = strtok_r(buf, " \t", &save);
    char* tok2 = strtok_r(nullptr, " \t", &save);
    char* tok3 = strtok_r(nullptr, " \t", &save);

    if (!tok1) {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    // help
    if (equalsIgnoreCase(tok1, "help") || equalsIgnoreCase(tok1, "h")) {
        cmd.valid = true; cmd.type = CMD_HELP; return cmd;
    }

    // status
    if (equalsIgnoreCase(tok1, "status")) {
        cmd.valid = true; cmd.type = CMD_STATUS; return cmd;
    }

    // ping
    if (equalsIgnoreCase(tok1, "ping")) {
        cmd.valid = true; cmd.type = CMD_PING; return cmd;
    }

    // estop
    if (equalsIgnoreCase(tok1, "estop")) {
        cmd.valid = true; cmd.type = CMD_ESTOP_STATUS; return cmd;
    }

    // armbridge on|off|status
    if (equalsIgnoreCase(tok1, "armbridge")) {
        if (!tok2 || equalsIgnoreCase(tok2, "status")) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_STATUS; return cmd;
        }
        if (equalsIgnoreCase(tok2, "on")) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_ON; return cmd;
        }
        if (equalsIgnoreCase(tok2, "off")) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_OFF; return cmd;
        }
        cmd.error = "ERROR: use armbridge on | off | status.";
        return cmd;
    }

    // identify
    if (equalsIgnoreCase(tok1, "identify") || equalsIgnoreCase(tok1, "id")) {
        if (!tok2 || equalsIgnoreCase(tok2, "all")) {
            cmd.valid = true; cmd.type = CMD_IDENTIFY; return cmd;
        }
        uint8_t id = 0;
        if (parseMotorName(tok2, id)) {
            cmd.valid = true; cmd.type = CMD_IDENTIFY_ONE; cmd.motorId = id; return cmd;
        }
        cmd.error = "ERROR: identify target must be fl, rl, fr, rr, or all.";
        return cmd;
    }

    // stop
    if (equalsIgnoreCase(tok1, "stop")) {
        if (tok2 && equalsIgnoreCase(tok2, "all")) {
            cmd.valid = true; cmd.type = CMD_STOP_ALL; cmd.pwm = 0; return cmd;
        }
        if (tok2) {
            cmd.error = "ERROR: stop command only accepts optional 'all'.";
            return cmd;
        }
        cmd.valid = true; cmd.type = CMD_STOP; cmd.pwm = 0; return cmd;
    }

    // red on|off
    if (equalsIgnoreCase(tok1, "red")) {
        if (!tok2) { cmd.error = "ERROR: Use 'red on' or 'red off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_RED_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_RED_OFF; return cmd; }
        cmd.error = "ERROR: Use 'red on' or 'red off'."; return cmd;
    }

    // green on|off
    if (equalsIgnoreCase(tok1, "green")) {
        if (!tok2) { cmd.error = "ERROR: Use 'green on' or 'green off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_GREEN_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_GREEN_OFF; return cmd; }
        cmd.error = "ERROR: Use 'green on' or 'green off'."; return cmd;
    }

    // yellow on|off
    if (equalsIgnoreCase(tok1, "yellow")) {
        if (!tok2) { cmd.error = "ERROR: Use 'yellow on' or 'yellow off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_YELLOW_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_YELLOW_OFF; return cmd; }
        cmd.error = "ERROR: Use 'yellow on' or 'yellow off'."; return cmd;
    }

    // Motion commands: forward, backward, left, right
    CommandType motionType = CMD_INVALID;
    if (equalsIgnoreCase(tok1, "forward") || equalsIgnoreCase(tok1, "fwd") || equalsIgnoreCase(tok1, "w"))
        motionType = CMD_FORWARD;
    else if (equalsIgnoreCase(tok1, "backward") || equalsIgnoreCase(tok1, "back") || equalsIgnoreCase(tok1, "s"))
        motionType = CMD_BACKWARD;
    else if (equalsIgnoreCase(tok1, "left") || equalsIgnoreCase(tok1, "a"))
        motionType = CMD_LEFT;
    else if (equalsIgnoreCase(tok1, "right") || equalsIgnoreCase(tok1, "d"))
        motionType = CMD_RIGHT;

    if (motionType != CMD_INVALID) {
        if (!tok2) {
            cmd.error = "ERROR: PWM parameter missing. Usage: forward 100";
            return cmd;
        }
        int pwm = 0;
        if (!parsePWM(tok2, pwm)) {
            cmd.error = "ERROR: PWM must be an integer between 0 and 255.";
            return cmd;
        }
        if (tok3) {
            cmd.error = "ERROR: too many arguments.";
            return cmd;
        }
        cmd.valid = true; cmd.type = motionType; cmd.pwm = pwm; return cmd;
    }

    cmd.error = "ERROR: Unknown command. Type 'help'.";
    return cmd;
}

bool CommandParser::validateArmSetValue(const char* field, long value, const char*& error) {
    if (equalsIgnoreCase(field, "posgain")) {
        if (value < 100 || value > 3000) {
            error = "ERROR: ARM posgain value must be in range 100-3000.";
            return false;
        }
        return true;
    }
    if (equalsIgnoreCase(field, "neggain")) {
        if (value < 100 || value > 3000) {
            error = "ERROR: ARM neggain value must be in range 100-3000.";
            return false;
        }
        return true;
    }
    if (equalsIgnoreCase(field, "dead")) {
        return true;
    }
    if (equalsIgnoreCase(field, "maxpwm")) {
        if (value < 0 || value > 255) {
            error = "ERROR: ARM maxpwm value must be in range 0-255.";
            return false;
        }
        return true;
    }
    if (equalsIgnoreCase(field, "invert")) {
        if (!(value == 0 || value == 1)) {
            error = "ERROR: ARM invert value must be 0 or 1.";
            return false;
        }
        return true;
    }
    error = "ERROR: ARM set field must be one of posgain, neggain, dead, maxpwm, invert.";
    return false;
}

bool CommandParser::isValidArmId(long id) {
    return (id == 1 || id == 2 || id == 3);
}

bool CommandParser::validateArmCommand(const char* payload, const char*& error) {
    char work[ARM_COMMAND_TEXT_MAX];
    strncpy(work, payload, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char* saveptr = nullptr;
    char* cmd = strtok_r(work, " \t", &saveptr);
    if (cmd == nullptr) {
        error = "ERROR: ARM command missing after 'arm'.";
        return false;
    }

    if (equalsIgnoreCase(cmd, "params") ||
        equalsIgnoreCase(cmd, "status") ||
        equalsIgnoreCase(cmd, "stop")) {
        char* extra = strtok_r(nullptr, " \t", &saveptr);
        if (extra != nullptr) {
            error = "ERROR: Too many parameters for ARM command.";
            return false;
        }
        return true;
    }

    if (equalsIgnoreCase(cmd, "zero")) {
        char* idTok = strtok_r(nullptr, " \t", &saveptr);
        char* extra = strtok_r(nullptr, " \t", &saveptr);
        long id = 0;
        if (idTok == nullptr || extra != nullptr || !parseInteger(idTok, id) || !isValidArmId(id)) {
            error = "ERROR: ARM zero id must be 1, 2, or 3.";
            return false;
        }
        return true;
    }

    if (equalsIgnoreCase(cmd, "set")) {
        char* idTok = strtok_r(nullptr, " \t", &saveptr);
        char* fieldTok = strtok_r(nullptr, " \t", &saveptr);
        char* valueTok = strtok_r(nullptr, " \t", &saveptr);
        char* extra = strtok_r(nullptr, " \t", &saveptr);
        long id = 0;
        long value = 0;
        if (idTok == nullptr || fieldTok == nullptr || valueTok == nullptr || extra != nullptr) {
            error = "ERROR: ARM set format is: arm set <id> <field> <value>.";
            return false;
        }
        if (!parseInteger(idTok, id) || !isValidArmId(id)) {
            error = "ERROR: ARM set id must be 1, 2, or 3.";
            return false;
        }
        if (!parseInteger(valueTok, value)) {
            error = "ERROR: ARM set value must be an integer.";
            return false;
        }
        return validateArmSetValue(fieldTok, value, error);
    }

    error = "ERROR: Invalid ARM command.";
    return false;
}

void CommandParser::printHelp() {
    terminal.println("========================================");
    terminal.println("  Motor Control System - Command List");
    terminal.println("========================================");
    terminal.println("  forward <pwm> / fwd / w");
    terminal.println("  backward <pwm> / back / s");
    terminal.println("  left <pwm> / a");
    terminal.println("  right <pwm> / d");
    terminal.println("  stop                 -> stop all motors");
    terminal.println("  stop all             -> stop motors + arm");
    terminal.println("  identify / id all    -> identify all motors");
    terminal.println("  id fl | rl | fr | rr -> identify one motor");
    terminal.println("  status               -> system status");
    terminal.println("  ping                 -> alive check");
    terminal.println("  estop                -> E-STOP diagnostics");
    terminal.println("----------------------------------------");
    terminal.println("  arm <payload>        -> forward to arm MCU");
    terminal.println("     arm stop | arm params | arm status");
    terminal.println("     arm zero 1 | arm set 1 posgain 1200");
    terminal.println("  armbridge on | off | status");
    terminal.println("----------------------------------------");
    terminal.println("  red on | off");
    terminal.println("  green on | off");
    terminal.println("  yellow on | off");
    terminal.println("========================================");
}
