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
static uint32_t lastBlinkMs  = 0;
static bool     ledState     = false;

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

static void safeStopAll() {
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        sendToMotor(i, "rpm 0");
    }
    delay(SAFE_STOP_DELAY_MS);
    for (size_t i = 0; i < MOTOR_COUNT; i++) {
        sendToMotor(i, "stop");
    }
}

static bool isDirectPassthrough(const char* cmd) {
    static const char* prefixes[] = {
        "f", "b", "rpm ", "pwm ", "mode ", "status", "hall", "help",
        "clrerr", "debug ", "telper ", "kick ", "kickduty ", "kickms ",
        "ramp ", "ramprate ", "rampms ", "defpwm ", "gatetest ",
        "scan", "test", "identify", "map", "mapreset", "reload",
        "spstat", "base ", "boost ", "pi ", "kp ", "ki "
    };
    for (const char* p : prefixes) {
        if (strStartsNoCase(cmd, p)) return true;
    }
    return false;
}

static void printHelp() {
    Serial.println("F446 bridge commands:");
    Serial.println("  ping                 -> pong");
    Serial.println("  help                 -> this help");
    Serial.println("  bridge on/off        -> enable/disable M1 telemetry forwarding");
    Serial.println("  m1 <cmd>             -> send <cmd> to F411 motor controller");
    Serial.println("  raw <cmd>            -> send <cmd> to F411 motor controller");
    Serial.println("  stop                 -> safe stop: rpm 0 + stop");
    Serial.println("  safe / estop         -> safe stop: rpm 0 + stop");
    Serial.println("  all <cmd>            -> send <cmd> to all motors (single-motor build)");
    Serial.println("Direct F411 passthrough examples:");
    Serial.println("  f50, b50, rpm 30, rpm -30, mode duty, mode speed, hall, status");
}

static void handlePcLine(char* line) {
    trimInPlace(line);
    if (!line[0]) return;

    if (strEqNoCase(line, "ping")) {
        Serial.println("pong");
        return;
    }

    if (strEqNoCase(line, "help") || strEqNoCase(line, "?")) {
        printHelp();
        return;
    }

    if (strEqNoCase(line, "stop") || strEqNoCase(line, "safe") || strEqNoCase(line, "estop")) {
        safeStopAll();
        Serial.println("OK|safe stop sent");
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
        } else {
            Serial.println("ERR|usage: bridge on/off");
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
}
