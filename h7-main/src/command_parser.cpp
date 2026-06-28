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

bool CommandParser::parseValue(const char* token, int& value) {
    // Generic motion degeri: RPM veya PWM (0 - DRIVE_VALUE_MAX), unsigned
    if (token == nullptr || *token == '\0') return false;

    const char* p = token;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }

    value = atoi(token);
    if (value < 0 || value > DRIVE_VALUE_MAX) {
        return false;
    }

    return true;
}

bool CommandParser::parseSignedValue(const char* token, int& value) {
    // Isaretli rpm (per-motor): -DRIVE_VALUE_MAX .. +DRIVE_VALUE_MAX
    if (token == nullptr || *token == '\0') return false;

    const char* p = token;
    if (*p == '-') p++;
    if (*p == '\0') return false;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }

    value = atoi(token);
    if (value < -DRIVE_VALUE_MAX || value > DRIVE_VALUE_MAX) {
        return false;
    }
    return true;
}

bool CommandParser::resolveTarget(const char* token, uint8_t& motorIdOut, bool& targetAllOut) {
    // "all" -> targetAll=true; fl/rl/fr/rr -> motorId
    if (!token) return false;
    if (equalsIgnoreCase(token, "all")) { targetAllOut = true; return true; }
    targetAllOut = false;
    return parseMotorName(token, motorIdOut);
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

// --- file-scope helpers for parse() ---

// strtok_r saveptr sonrasi leading space/tab atla
static const char* skipSpaces(const char* s) {
    if (!s) return "";
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// F411 forward satirini olustur: "prefix remainder" / "prefix" / remainder (raw)
static void buildF411Line(char* dst, size_t dstSize, const char* prefix, const char* remainder) {
    if (prefix == nullptr || prefix[0] == '\0') {
        snprintf(dst, dstSize, "%s", remainder ? remainder : "");
    } else if (remainder == nullptr || remainder[0] == '\0') {
        snprintf(dst, dstSize, "%s", prefix);
    } else {
        snprintf(dst, dstSize, "%s %s", prefix, remainder);
    }
}

// Forwarding komut spec tablosu (pi/base/boost/ramp/spstat/...)
struct FwdSpec {
    const char* word;     // H7 komut kelimesi
    const char* prefix;   // F411 prefix (nullptr = raw)
    bool reqRem;          // remainder zorunlu
    bool allowRem;        // remainder izinli
};

static const FwdSpec FWD_SPECS[] = {
    {"pi",       "pi",       true,  true},
    {"base",     "base",     true,  true},
    {"boost",    "boost",    true,  true},
    {"ramp",     "ramp",     true,  true},
    {"spstat",   "spstat",   false, false},
    {"speedcfg", "spstat",   false, false},   // alias: speedcfg -> spstat
    {"hall",     "hall",     false, false},
    {"map",      "map",      false, false},
    {"save",     "save",     false, false},
    {"reload",   "reload",   false, false},
    {"mapreset", "mapreset", false, false},
    {"clrerr",   "clrerr",   false, false},
    {"raw",      nullptr,    true,  true},    // raw: prefix yok, remainder=payload
};

Command CommandParser::parse(const char* line) {
    Command cmd;
    cmd.type = CMD_INVALID;
    cmd.value = 0;
    cmd.signedValue = 0;
    cmd.valid = false;
    cmd.error = nullptr;
    cmd.motorId = 0;
    cmd.targetAll = false;
    cmd.f411Line[0] = '\0';
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
    if (!tok1) {
        cmd.error = "ERROR: empty command.";
        return cmd;
    }
    char* tok2 = strtok_r(nullptr, " \t", &save);
    // tok3 / remainder lazily extracted per branch

    // --- no-arg commands ---
    if (equalsIgnoreCase(tok1, "help") || equalsIgnoreCase(tok1, "h")) {
        cmd.valid = true; cmd.type = CMD_HELP; return cmd;
    }
    if (equalsIgnoreCase(tok1, "ping")) {
        cmd.valid = true; cmd.type = CMD_PING; return cmd;
    }
    if (equalsIgnoreCase(tok1, "estop")) {
        cmd.valid = true; cmd.type = CMD_ESTOP_STATUS; return cmd;
    }

    // status: arg yok -> H7 sistem status; target varsa F411 "status" forward
    if (equalsIgnoreCase(tok1, "status")) {
        if (!tok2) { cmd.valid = true; cmd.type = CMD_STATUS; return cmd; }
        const char* rem = skipSpaces(save);
        if (rem[0] != '\0') { cmd.error = "ERROR: status <target> takes no extra args."; return cmd; }
        uint8_t id = 0; bool all = false;
        if (!resolveTarget(tok2, id, all)) { cmd.error = "ERROR: target must be fl, rl, fr, rr, or all."; return cmd; }
        cmd.valid = true; cmd.type = CMD_F411_FORWARD; cmd.motorId = id; cmd.targetAll = all;
        snprintf(cmd.f411Line, sizeof(cmd.f411Line), "status");
        return cmd;
    }

    // armbridge on|off|status
    if (equalsIgnoreCase(tok1, "armbridge")) {
        if (!tok2 || equalsIgnoreCase(tok2, "status")) {
            cmd.valid = true; cmd.type = CMD_ARMBRIDGE_STATUS; return cmd;
        }
        if (equalsIgnoreCase(tok2, "on")) { cmd.valid = true; cmd.type = CMD_ARMBRIDGE_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_ARMBRIDGE_OFF; return cmd; }
        cmd.error = "ERROR: use armbridge on | off | status.";
        return cmd;
    }

    // wheelbridge on|off|status — F411 telemetri kopru
    if (equalsIgnoreCase(tok1, "wheelbridge")) {
        if (!tok2 || equalsIgnoreCase(tok2, "status")) {
            cmd.valid = true; cmd.type = CMD_WHEELBRIDGE_STATUS; return cmd;
        }
        if (equalsIgnoreCase(tok2, "on")) { cmd.valid = true; cmd.type = CMD_WHEELBRIDGE_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_WHEELBRIDGE_OFF; return cmd; }
        cmd.error = "ERROR: use wheelbridge on | off | status.";
        return cmd;
    }

    // identify [all|<motor>]
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

    // stop / stop all
    if (equalsIgnoreCase(tok1, "stop")) {
        if (tok2 && equalsIgnoreCase(tok2, "all")) {
            if (skipSpaces(save)[0] != '\0') { cmd.error = "ERROR: too many arguments."; return cmd; }
            cmd.valid = true; cmd.type = CMD_STOP_ALL; return cmd;
        }
        if (tok2) { cmd.error = "ERROR: stop command only accepts optional 'all'."; return cmd; }
        cmd.valid = true; cmd.type = CMD_STOP; return cmd;
    }

    // red/green/yellow on|off
    if (equalsIgnoreCase(tok1, "red")) {
        if (!tok2) { cmd.error = "ERROR: Use 'red on' or 'red off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_RED_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_RED_OFF; return cmd; }
        cmd.error = "ERROR: Use 'red on' or 'red off'."; return cmd;
    }
    if (equalsIgnoreCase(tok1, "green")) {
        if (!tok2) { cmd.error = "ERROR: Use 'green on' or 'green off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_GREEN_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_GREEN_OFF; return cmd; }
        cmd.error = "ERROR: Use 'green on' or 'green off'."; return cmd;
    }
    if (equalsIgnoreCase(tok1, "yellow")) {
        if (!tok2) { cmd.error = "ERROR: Use 'yellow on' or 'yellow off'."; return cmd; }
        if (equalsIgnoreCase(tok2, "on"))  { cmd.valid = true; cmd.type = CMD_YELLOW_ON; return cmd; }
        if (equalsIgnoreCase(tok2, "off")) { cmd.valid = true; cmd.type = CMD_YELLOW_OFF; return cmd; }
        cmd.error = "ERROR: Use 'yellow on' or 'yellow off'."; return cmd;
    }

    // speed [target] on|off -> F411 "mode speed"/"mode duty" forward
    if (equalsIgnoreCase(tok1, "speed")) {
        if (!tok2) { cmd.error = "ERROR: use 'speed [target] on|off'."; return cmd; }
        const char* tgtTok = nullptr;
        const char* onoffTok = nullptr;
        if (equalsIgnoreCase(tok2, "on") || equalsIgnoreCase(tok2, "off")) {
            tgtTok = "all"; onoffTok = tok2;          // speed on/off -> all motors
        } else {
            tgtTok = tok2;
            char* tok3 = strtok_r(nullptr, " \t", &save);
            if (!tok3 || !(equalsIgnoreCase(tok3, "on") || equalsIgnoreCase(tok3, "off"))) {
                cmd.error = "ERROR: use 'speed [target] on|off'."; return cmd;
            }
            onoffTok = tok3;
        }
        if (skipSpaces(save)[0] != '\0') { cmd.error = "ERROR: too many arguments."; return cmd; }
        const char* modeTxt = equalsIgnoreCase(onoffTok, "on") ? "mode speed" : "mode duty";
        uint8_t id = 0; bool all = false;
        if (!resolveTarget(tgtTok, id, all)) { cmd.error = "ERROR: target must be fl, rl, fr, rr, or all."; return cmd; }
        cmd.valid = true; cmd.type = CMD_F411_FORWARD; cmd.motorId = id; cmd.targetAll = all;
        snprintf(cmd.f411Line, sizeof(cmd.f411Line), "%s", modeTxt);
        return cmd;
    }

    // rpm forward|backward|left|right <val> | rpm stop | rpm <motor|all> <signed>
    if (equalsIgnoreCase(tok1, "rpm")) {
        if (!tok2) { cmd.valid = true; cmd.type = CMD_RPM_QUERY; return cmd; }

        CommandType rpmMotion = CMD_INVALID;
        if (equalsIgnoreCase(tok2, "forward"))  rpmMotion = CMD_RPM_FORWARD;
        else if (equalsIgnoreCase(tok2, "backward")) rpmMotion = CMD_RPM_BACKWARD;
        else if (equalsIgnoreCase(tok2, "left"))     rpmMotion = CMD_RPM_LEFT;
        else if (equalsIgnoreCase(tok2, "right"))    rpmMotion = CMD_RPM_RIGHT;
        else if (equalsIgnoreCase(tok2, "stop"))     rpmMotion = CMD_RPM_STOP;

        if (rpmMotion == CMD_RPM_STOP) {
            if (skipSpaces(save)[0] != '\0') { cmd.error = "ERROR: rpm stop takes no args."; return cmd; }
            cmd.valid = true; cmd.type = CMD_RPM_STOP; return cmd;
        }
        if (rpmMotion != CMD_INVALID) {
            char* tok3 = strtok_r(nullptr, " \t", &save);
            if (!tok3) { cmd.error = "ERROR: rpm <dir> requires a value. Example: rpm forward 23"; return cmd; }
            int v = 0;
            if (!parseValue(tok3, v)) { cmd.error = "ERROR: rpm value must be 0-400."; return cmd; }
            if (skipSpaces(save)[0] != '\0') { cmd.error = "ERROR: too many arguments."; return cmd; }
            cmd.valid = true; cmd.type = rpmMotion; cmd.value = v; return cmd;
        }

        // per-motor: rpm <motor|all> <signed>
        uint8_t id = 0; bool all = false;
        if (!resolveTarget(tok2, id, all)) {
            cmd.error = "ERROR: use rpm forward|backward|left|right|stop <val>, or rpm <motor> <signed>.";
            return cmd;
        }
        char* tok3 = strtok_r(nullptr, " \t", &save);
        if (!tok3) { cmd.error = "ERROR: rpm <motor> requires a signed value. Example: rpm fl -23"; return cmd; }
        int sv = 0;
        if (!parseSignedValue(tok3, sv)) { cmd.error = "ERROR: rpm signed value out of range."; return cmd; }
        if (skipSpaces(save)[0] != '\0') { cmd.error = "ERROR: too many arguments."; return cmd; }
        cmd.valid = true; cmd.type = CMD_RPM_MOTOR; cmd.motorId = id; cmd.targetAll = all; cmd.signedValue = sv;
        return cmd;
    }

    // forwarding table: pi/base/boost/ramp/spstat/speedcfg/hall/map/save/reload/mapreset/clrerr/raw
    for (size_t i = 0; i < sizeof(FWD_SPECS) / sizeof(FWD_SPECS[0]); i++) {
        const FwdSpec& sp = FWD_SPECS[i];
        if (!equalsIgnoreCase(tok1, sp.word)) continue;

        if (!tok2) { cmd.error = "ERROR: target missing. Use all/fl/rl/fr/rr."; return cmd; }
        uint8_t id = 0; bool all = false;
        if (!resolveTarget(tok2, id, all)) { cmd.error = "ERROR: target must be fl, rl, fr, rr, or all."; return cmd; }
        const char* rem = skipSpaces(save);
        bool hasRem = (rem[0] != '\0');
        if (sp.reqRem && !hasRem) { cmd.error = "ERROR: this command requires arguments."; return cmd; }
        if (!sp.allowRem && hasRem) { cmd.error = "ERROR: this command takes no extra arguments."; return cmd; }

        cmd.valid = true; cmd.type = CMD_F411_FORWARD; cmd.motorId = id; cmd.targetAll = all;
        buildF411Line(cmd.f411Line, sizeof(cmd.f411Line), sp.prefix, rem);
        return cmd;
    }

    // motion: forward/backward/left/right (PWM, existing) + aliases
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
        if (!tok2) { cmd.error = "ERROR: PWM value missing. Usage: forward 100"; return cmd; }
        int v = 0;
        if (!parseValue(tok2, v)) { cmd.error = "ERROR: PWM value must be 0-250."; return cmd; }
        if (v > PWM_MAX) { cmd.error = "ERROR: PWM value must be 0-250."; return cmd; }
        char* tok3 = strtok_r(nullptr, " \t", &save);
        if (tok3) { cmd.error = "ERROR: too many arguments."; return cmd; }
        cmd.valid = true; cmd.type = motionType; cmd.value = v; return cmd;
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
    terminal.println("-- PWM movement (duty mode) --");
    terminal.println("  forward <pwm> / fwd / w");
    terminal.println("  backward <pwm> / back / s");
    terminal.println("  left <pwm> / a");
    terminal.println("  right <pwm> / d");
    terminal.println("-- RPM movement (speed PI mode, 0-400 RPM) --");
    terminal.println("  rpm forward <rpm> | rpm backward <rpm>");
    terminal.println("  rpm left <rpm> | rpm right <rpm>");
    terminal.println("  rpm stop");
    terminal.println("  rpm <motor|all> <signed>  (rpm fl 23, rpm fr -23, rpm all 0)");
    terminal.println("  rpm                       -> target query");
    terminal.println("-- Speed mode toggle (-> F411 mode speed/duty) --");
    terminal.println("  speed on | speed off      (all motors)");
    terminal.println("  speed all on | speed fl off");
    terminal.println("-- Speed PI config forwarding --");
    terminal.println("  pi <tgt> <kp> <ki>        (pi all 0.8 0.1)");
    terminal.println("  base <tgt> <lo> <mid> <hi>");
    terminal.println("  boost <tgt> <lo> <mid> <hi> <ms>");
    terminal.println("  ramp <tgt> <up> <down>");
    terminal.println("  spstat <tgt> | speedcfg <tgt>");
    terminal.println("-- Raw pass-through --");
    terminal.println("  raw <tgt> <payload>       (raw fl spstat, raw all mode speed)");
    terminal.println("-- Service (per-motor / all) --");
    terminal.println("  hall <tgt> | map <tgt> | save <tgt>");
    terminal.println("  reload <tgt> | mapreset <tgt> | clrerr <tgt>");
    terminal.println("  status <tgt>              (F411 status; bare status = H7 system)");
    terminal.println("-- Stop / safety --");
    terminal.println("  stop | stop all           (stop + rpm 0 to all F411s)");
    terminal.println("  identify / id all | id fl|rl|fr|rr");
    terminal.println("-- Telemetry bridge --");
    terminal.println("  wheelbridge on | off | status");
    terminal.println("----------------------------------------");
    terminal.println("  status                    -> H7 system status");
    terminal.println("  ping | estop | help");
    terminal.println("  arm <payload> | armbridge on|off|status");
    terminal.println("  red on|off | green on|off | yellow on|off");
    terminal.println("  (target = all | fl | rl | fr | rr)");
    terminal.println("========================================");
}
