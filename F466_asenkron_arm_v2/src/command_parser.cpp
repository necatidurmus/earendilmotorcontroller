#include "command_parser.h"
#include "config.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

bool CommandParser::parsePwm(const char* token, int* pwmOut) {
    if (!token || !*token || !pwmOut) return false;
    char* end = nullptr;
    long value = strtol(token, &end, 10);
    if (end == token || *end != '\0') return false;
    if (value < PWM_MIN || value > PWM_MAX) return false;
    *pwmOut = static_cast<int>(value);
    return true;
}

bool CommandParser::parseMotorName(const char* token, uint8_t* motorIdOut) {
    if (!token || !motorIdOut) return false;
    if (strcmp(token, "fl") == 0) { *motorIdOut = MOTOR_FL; return true; }
    if (strcmp(token, "fr") == 0) { *motorIdOut = MOTOR_FR; return true; }
    return false;
}

Command CommandParser::parse(const char* line) {
    Command cmd;

    char raw[TERMINAL_LINE_MAX];
    if (!line) {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    strncpy(raw, line, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';
    trimInPlace(raw);

    if (raw[0] == '\0') {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    // Preserve payload case for arm commands. The lower-controller may expect "J,".
    // Important: match only the standalone token "arm", not "armbridge".
    if (startsWithNoCase(raw, "arm") && (raw[3] == '\0' || isspace(static_cast<unsigned char>(raw[3])))) {
        const char* p = raw + 3;
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
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    lowerInPlace(buf);

    char* save = nullptr;
    char* tok1 = strtok_r(buf, " \t", &save);
    char* tok2 = strtok_r(nullptr, " \t", &save);
    char* tok3 = strtok_r(nullptr, " \t", &save);

    if (!tok1) {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }

    if (strcmp(tok1, "help") == 0 || strcmp(tok1, "h") == 0) {
        cmd.valid = true; cmd.type = CMD_HELP; return cmd;
    }

    if (strcmp(tok1, "status") == 0 || strcmp(tok1, "i") == 0) {
        cmd.valid = true; cmd.type = CMD_STATUS; return cmd;
    }

    if (strcmp(tok1, "ping") == 0) {
        cmd.valid = true; cmd.type = CMD_PING; return cmd;
    }

    if (strcmp(tok1, "estop") == 0) {
        cmd.valid = true; cmd.type = CMD_ESTOP_STATUS; return cmd;
    }

    if (strcmp(tok1, "armbridge") == 0) {
        if (!tok2 || strcmp(tok2, "status") == 0) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_STATUS; return cmd;
        }
        if (strcmp(tok2, "on") == 0) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_ON; return cmd;
        }
        if (strcmp(tok2, "off") == 0) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_OFF; return cmd;
        }
        cmd.error = "ERROR: use armbridge on | off | status.";
        return cmd;
    }

    if (strcmp(tok1, "identify") == 0 || strcmp(tok1, "id") == 0) {
        if (!tok2 || strcmp(tok2, "all") == 0) {
            cmd.valid = true; cmd.type = CMD_IDENTIFY_ALL; return cmd;
        }
        if (strcmp(tok2, "rl") == 0 || strcmp(tok2, "rr") == 0) {
            cmd.error = "ERROR: RL/RR are disabled in this FL/FR-only build.";
            return cmd;
        }
        uint8_t id = 0;
        if (parseMotorName(tok2, &id)) {
            cmd.valid = true; cmd.type = CMD_IDENTIFY_ONE; cmd.motorId = id; return cmd;
        }
        cmd.error = "ERROR: identify target must be fl, fr, or all.";
        return cmd;
    }

    if (strcmp(tok1, "stop") == 0) {
        if (tok2 && strcmp(tok2, "all") == 0) {
            cmd.valid = true; cmd.type = CMD_STOP_ALL; cmd.pwm = 0; return cmd;
        }
        if (tok2) {
            cmd.error = "ERROR: stop command only accepts optional 'all'.";
            return cmd;
        }
        cmd.valid = true; cmd.type = CMD_STOP; cmd.pwm = 0; return cmd;
    }

    CommandType motionType = CMD_INVALID;
    if (strcmp(tok1, "forward") == 0 || strcmp(tok1, "fwd") == 0 || strcmp(tok1, "w") == 0) motionType = CMD_FORWARD;
    else if (strcmp(tok1, "backward") == 0 || strcmp(tok1, "back") == 0 || strcmp(tok1, "s") == 0) motionType = CMD_BACKWARD;
    else if (strcmp(tok1, "left") == 0 || strcmp(tok1, "a") == 0) motionType = CMD_LEFT;
    else if (strcmp(tok1, "right") == 0 || strcmp(tok1, "d") == 0) motionType = CMD_RIGHT;

    if (motionType != CMD_INVALID) {
        if (!tok2) {
            cmd.error = "ERROR: PWM parameter missing. Usage: forward 100";
            return cmd;
        }
        int pwm = 0;
        if (!parsePwm(tok2, &pwm)) {
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

void CommandParser::printHelp() {
    TERMINAL_UART.println("Commands:");
    TERMINAL_UART.println("  help / h");
    TERMINAL_UART.println("  status / i");
    TERMINAL_UART.println("  ping");
    TERMINAL_UART.println("  estop            -> prints E-STOP raw pin + active state");
    TERMINAL_UART.println("  forward <0-255>");
    TERMINAL_UART.println("  backward <0-255>");
    TERMINAL_UART.println("  left <0-255>");
    TERMINAL_UART.println("  right <0-255>");
    TERMINAL_UART.println("  stop              -> stops FL/FR only");
    TERMINAL_UART.println("  stop all          -> stops FL/FR and sends arm stop");
    TERMINAL_UART.println("  identify / id all -> sends identify only to FL and FR");
    TERMINAL_UART.println("  id fl | id fr");
    TERMINAL_UART.println("  arm <payload>     -> forwards payload to arm controller");
    TERMINAL_UART.println("     examples: arm stop | arm J,0,0,0,0,0,0");
    TERMINAL_UART.println("  armbridge on | off | status");
    TERMINAL_UART.println("");
    TERMINAL_UART.println("RL/RR are completely disabled: UART4/UART5 are not initialized or used.");
    TERMINAL_UART.println("Arm TX is enabled on USART6 PC6. Arm RX telemetry bridge is controlled by armbridge.");
}
