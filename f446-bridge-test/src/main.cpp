#include <Arduino.h>
#include "f446_bridge_config.h"

static HardwareSerial MotorSerial(MOTOR_RX_PIN, MOTOR_TX_PIN);

struct MotorPort {
    const char*     name;
    HardwareSerial* serial;
    char            lineBuf[M1_LINE_MAX];
    size_t          pos;
    bool            discarding;
    bool            enabled;
};

static MotorPort motors[MOTOR_COUNT] = {
    { "M1", &MotorSerial, {}, 0, false, true }
};

static char     pcLine[PC_LINE_MAX];
static size_t   pcPos        = 0;
static bool     pcDiscarding = false;
static bool     bridgeEnabled = true;
static bool     serviceUnlocked = false;
static uint32_t serviceUnlockMs = 0;
static uint32_t blockedCmdCount = 0;
static uint32_t lastBlinkMs  = 0;
static bool     ledState     = false;
static bool     delayedStopPending = false;
static uint32_t delayedStopDeadlineMs = 0;

static bool strEqNoCase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool strStartsNoCase(const char* s, const char* prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++; prefix++;
    }
    return true;
}

static void trimInPlace(char* s) {
    if (!s) return;
    char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static void sendToMotor(uint8_t idx, const char* cmd) {
    if (idx >= MOTOR_COUNT || !cmd || !cmd[0]) return;
    motors[idx].serial->print(cmd);
    motors[idx].serial->print('\n');
    Serial.print("TX|");
    Serial.println(cmd);
}

static void normalStopAll() {
    /* Normal stop: ramp down to zero, then stop.
     * Does NOT create a fault latch on the F411. */
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        sendToMotor(i, "rpm 0");
    }
    delayedStopPending = true;
    delayedStopDeadlineMs = millis() + SAFE_STOP_DELAY_MS;
    serviceUnlocked = false;
}

static void coastStopAll() {
    /* Coast stop: send safe (coast on F411, no fault latch).
     * Belt-and-suspenders: also sends stop after a short delay. */
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        sendToMotor(i, "safe");
    }
    delayedStopPending = true;
    delayedStopDeadlineMs = millis() + SAFE_STOP_DELAY_MS;
    serviceUnlocked = false;
}

static void serviceDelayedStop() {
    if (!delayedStopPending) return;
    if ((int32_t)(millis() - delayedStopDeadlineMs) < 0) return;
    delayedStopPending = false;
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        sendToMotor(i, "stop");
    }
}

static bool isValidFDuty(const char* cmd) {
    if (cmd[0] != 'f' && cmd[0] != 'F') return false;
    if (cmd[1] == '\0') return true;
    if (cmd[1] < '0' || cmd[1] > '9') return false;
    for (const char* p = cmd + 1; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static bool isValidBDuty(const char* cmd) {
    if (cmd[0] != 'b' && cmd[0] != 'B') return false;
    if (cmd[1] == '\0') return true;
    if (cmd[1] < '0' || cmd[1] > '9') return false;
    for (const char* p = cmd + 1; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static bool isDangerousServiceCmd(const char* cmd) {
    static const char* dangerous[] = {
        "gatetest", "identify", "test", "scan",
        "savecfg", "loadcfg", "saveall", "save",
        /* Map-changing commands (map default is safe, in passthrough) */
        "map set", "map apply", "map reset",
        "map save", "map load", "map edit", "map discard",
        NULL
    };
    for (const char** d = dangerous; *d; d++) {
        if (strEqNoCase(cmd, *d)) return true;
        if (strStartsNoCase(cmd, *d) && cmd[strlen(*d)] == ' ') return true;
    }
    static const char* dangerousPrefixes[] = {
        "gatetest ", "base ", "boost ", "pi ",
        "kp ", "ki ", "kickduty ", "kickms ",
        "ramp ", "ramprate ", "rampms ", "defpwm ",
        "map set ",
        NULL
    };
    for (const char** p = dangerousPrefixes; *p; p++) {
        if (strStartsNoCase(cmd, *p)) return true;
    }
    return false;
}

static bool isDirectPassthrough(const char* cmd) {
    if (isValidFDuty(cmd)) return true;
    if (isValidBDuty(cmd)) return true;
    static const char* exact[] = {
        "status", "hall", "help", "clrerr",
        "forward", "backward", "stop", "s", "x", "brake",
        "mode", "rpm", "pwm", "h", "?",
        "kick on", "kick off", "ramp on", "ramp off",
        "defaults", "map default", "mapreset", "reload",
        "debug on", "debug off", "dbg on", "dbg off",
        "mode duty", "mode speed", "mode normal", "mode control",
        "pid on", "pid off",
        "estop", "safe", "alloff",
        /* Read-only map commands */
        "map", "map validate", "map candidate"
        /* NOTE: "identify" and "scan" are NOT here — they are
         * dangerous service commands and must go through the
         * m1 prefix path with service unlock check. */
    };
    for (const char* e : exact) {
        if (strEqNoCase(cmd, e)) return true;
    }
    static const char* prefixes[] = {
        "rpm ", "pwm ", "mode ", "debug ", "telper "
    };
    for (const char* p : prefixes) {
        if (strStartsNoCase(cmd, p)) return true;
    }
    /* Dangerous commands: require service unlock */
    if (isDangerousServiceCmd(cmd)) return false;
    return false;
}

static void printHelp() {
    Serial.println("F446 bridge commands:");
    Serial.println("  ping                 -> pong");
    Serial.println("  help                 -> this help");
    Serial.println("  bridge on/off        -> enable/disable M1 telemetry forwarding");
    Serial.println("  bridge unlock_service-> unlock dangerous service commands (30s)");
    Serial.println("  bridge lock_service  -> re-lock service commands");
    Serial.println("  bridge status        -> show bridge status");
    Serial.println("  m1 <cmd>             -> send <cmd> to F411 motor controller");
    Serial.println("  raw <cmd>            -> send <cmd> to F411 motor controller");
    Serial.println("  all <cmd>            -> send <cmd> to all motors");
    Serial.println("  stop                 -> normal stop: rpm 0 + stop (no fault)");
    Serial.println("  safe / alloff        -> coast stop (motor coasts, no fault latch)");
    Serial.println("  estop                -> emergency all-off; next motion command may clear fault");
    Serial.println("Direct F411 passthrough examples:");
    Serial.println("  f50, b50, rpm 30, rpm -30, mode duty, mode speed, hall, status");
    Serial.println("  x/brake (ACTIVE BRAKE; current-limited PSU), kick on/off, ramp on/off");
    Serial.println("Service commands (requires bridge unlock_service + F411 arming):");
    Serial.println("  m1 identify, m1 scan, m1 test, m1 gatetest, m1 save, m1 loadcfg");
}

static void handlePcLine(char* line) {
    trimInPlace(line);
    if (!line[0]) return;

    /* Auto-disarm service unlock */
    if (serviceUnlocked && (millis() - serviceUnlockMs) >= SERVICE_TIMEOUT_MS) {
        serviceUnlocked = false;
        Serial.println("INFO|service lock expired");
    }

    if (strEqNoCase(line, "ping")) {
        Serial.println("pong");
        return;
    }

    if (strEqNoCase(line, "help") || strEqNoCase(line, "?")) {
        printHelp();
        return;
    }

    if (strEqNoCase(line, "stop")) {
        normalStopAll();
        Serial.println("OK|normal stop");
        return;
    }

    if (strEqNoCase(line, "safe") || strEqNoCase(line, "alloff")) {
        coastStopAll();
        Serial.println("OK|coast stop sent (no fault latch)");
        return;
    }

    if (strEqNoCase(line, "estop")) {
        /* Emergency all-off.  Current F411 policy allows a subsequent
         * motion command to clear the displayed fault and restart. */
        delayedStopPending = false;
        for (size_t i = 0; i < MOTOR_COUNT; i++) {
            sendToMotor(i, "estop");
        }
        serviceUnlocked = false;
        Serial.println("OK|estop sent (next motion command may clear fault)");
        return;
    }

    if (strStartsNoCase(line, "bridge ")) {
        const char* arg = line + 7;
        while (*arg == ' ') arg++;
        if (strEqNoCase(arg, "on")) {
            bridgeEnabled = true;
            Serial.println("OK|bridge on");
        } else if (strEqNoCase(arg, "off")) {
            bridgeEnabled = false;
            Serial.println("OK|bridge off");
        } else if (strStartsNoCase(arg, "unlock_service ")) {
            const char* token = arg + 15;
            while (*token == ' ') token++;
            if (strEqNoCase(token, "current_limited_bench_supply")) {
                serviceUnlocked = true;
                serviceUnlockMs = millis();
                Serial.println("OK|service unlocked for 30s");
            } else {
                Serial.println("ERR|usage: bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY");
            }
        } else if (strEqNoCase(arg, "lock_service")) {
            serviceUnlocked = false;
            Serial.println("OK|service locked");
        } else if (strEqNoCase(arg, "status")) {
            Serial.print("OK|bridge=");
            Serial.print(bridgeEnabled ? "ON" : "OFF");
            Serial.print(" service=");
            Serial.print(serviceUnlocked ? "UNLOCKED" : "LOCKED");
            if (serviceUnlocked) {
                uint32_t elapsed = millis() - serviceUnlockMs;
                uint32_t remain = (elapsed < SERVICE_TIMEOUT_MS) ? (SERVICE_TIMEOUT_MS - elapsed) : 0;
                Serial.print(" unlock_remain_ms=");
                Serial.print(remain);
            }
            Serial.print(" blocked_cmds=");
            Serial.println(blockedCmdCount);
        } else {
            Serial.println("ERR|usage: bridge on/off/unlock_service/lock_service/status");
        }
        return;
    }

    if (strStartsNoCase(line, "m1 ")) {
        const char* cmd = line + 3;
        while (*cmd == ' ') cmd++;
        if (!cmd[0]) {
            Serial.println("ERR|empty m1 command");
            return;
        }
        /* Dangerous command filter */
        if (isDangerousServiceCmd(cmd) && !serviceUnlocked) {
            blockedCmdCount++;
            Serial.print("ERR|command blocked (service locked): ");
            Serial.println(cmd);
            Serial.println("ERR|Use: bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        sendToMotor(0, cmd);
        return;
    }

    if (strStartsNoCase(line, "raw ")) {
        const char* cmd = line + 4;
        while (*cmd == ' ') cmd++;
        if (!cmd[0]) {
            Serial.println("ERR|empty raw command");
            return;
        }
        /* Raw commands always require unlock */
        if (!serviceUnlocked) {
            blockedCmdCount++;
            Serial.print("ERR|raw command blocked (service locked): ");
            Serial.println(cmd);
            Serial.println("ERR|Use: bridge unlock_service CURRENT_LIMITED_BENCH_SUPPLY");
            return;
        }
        sendToMotor(0, cmd);
        return;
    }

    if (strStartsNoCase(line, "all ")) {
        const char* cmd = line + 4;
        while (*cmd == ' ') cmd++;
        if (!cmd[0]) {
            Serial.println("ERR|empty all command");
            return;
        }
        if (isDangerousServiceCmd(cmd) && !serviceUnlocked) {
            blockedCmdCount++;
            Serial.print("ERR|command blocked (service locked): ");
            Serial.println(cmd);
            return;
        }
        if (MOTOR_COUNT == 1) {
            sendToMotor(0, cmd);
        } else {
            for (size_t i = 0; i < MOTOR_COUNT; i++) {
                sendToMotor(i, cmd);
            }
        }
        return;
    }

    if (isDirectPassthrough(line)) {
        sendToMotor(0, line);
        return;
    }

    Serial.print("ERR|unknown F446 command: ");
    Serial.println(line);
}

static void pumpPc() {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (!pcDiscarding) {
                pcLine[pcPos] = '\0';
                handlePcLine(pcLine);
            }
            pcPos = 0;
            pcDiscarding = false;
            continue;
        }
        if (pcDiscarding) continue;
        if (pcPos + 1 < sizeof(pcLine)) {
            pcLine[pcPos++] = c;
        } else {
            pcDiscarding = true;
            pcPos = 0;
            Serial.println("ERR|PC line overflow");
        }
    }
}

static void publishMotorLine(uint8_t idx, const char* line) {
    if (!bridgeEnabled || !line || !line[0]) return;
    if (idx >= MOTOR_COUNT) return;
    Serial.print(motors[idx].name);
    Serial.print("|");
    Serial.println(line);
}

static void pumpMotorPort(uint8_t idx) {
    if (idx >= MOTOR_COUNT) return;
    MotorPort& m = motors[idx];
    while (m.serial->available() > 0) {
        char c = (char)m.serial->read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (!m.discarding) {
                m.lineBuf[m.pos] = '\0';
                publishMotorLine(idx, m.lineBuf);
            }
            m.pos = 0;
            m.discarding = false;
            continue;
        }
        if (m.discarding) continue;
        if (m.pos + 1 < sizeof(m.lineBuf)) {
            m.lineBuf[m.pos++] = c;
        } else {
            m.discarding = true;
            m.pos = 0;
            Serial.print("WARN|");
            Serial.print(m.name);
            Serial.println(" line overflow");
        }
    }
}

static void heartbeatLed() {
    uint32_t now = millis();
    if (now - lastBlinkMs >= LED_BLINK_MS) {
        lastBlinkMs = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.begin(PC_BAUD);
    MotorSerial.begin(MOTOR_BAUD);
    delay(300);

    Serial.println();
    Serial.println("BOOT|F446 single-motor bridge ready");
    Serial.print("BOOT|PC_BAUD=");
    Serial.println(PC_BAUD);
    Serial.print("BOOT|MOTOR_BAUD=");
    Serial.println(MOTOR_BAUD);
    Serial.print("BOOT|MOTOR_TX=");
    Serial.println(MOTOR_TX_PIN);
    Serial.print("BOOT|MOTOR_RX=");
    Serial.println(MOTOR_RX_PIN);
    Serial.println("BOOT|Use: m1 <cmd>, stop, ping, help");
}

void loop() {
    pumpPc();
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        pumpMotorPort(i);
    }
    heartbeatLed();
    serviceDelayedStop();
    /* Auto-disarm service unlock */
    if (serviceUnlocked && (millis() - serviceUnlockMs) >= SERVICE_TIMEOUT_MS) {
        serviceUnlocked = false;
    }
}
