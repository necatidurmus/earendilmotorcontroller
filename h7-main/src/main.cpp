#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <IWatchdog.h>
#include "config.h"
#include "types.h"
#include "terminal_interface.h"
#include "command_parser.h"
#include "motion_controller.h"
#include "motor_dispatcher.h"
#include "status_manager.h"

// ============================================================
// Global module objects
// ============================================================
TerminalInterface terminal;
CommandParser     parser;
MotionController  motion;
MotorDispatcher   dispatcher;
StatusManager     status;

// E-STOP state
static bool g_estopActive = false;
static bool g_estopRawLast = false;
static uint32_t g_estopLastChangeMs = 0;
static uint32_t g_lastEstopReassertMs = 0;

// Heartbeat LED
static bool g_ledState = false;
static uint32_t g_lastHeartbeatMs = 0;

// Relay state
static bool g_redRelayOn = false;
static bool g_greenRelayOn = false;
static bool g_yellowRelayOn = false;

// ARM RX bridge
static bool g_armRxBridgeEnabled = (ARM_RX_BRIDGE_DEFAULT_ON != 0);
static char g_armRxBuf[ARM_RX_MAX_LINE];
static size_t g_armRxPos = 0;
static bool g_armRxDiscarding = false;
static uint32_t g_lastArmTruncateNoticeMs = 0;

// Motor polling — NOT IMPLEMENTED.
// F411 telemetry is received via the wheelbridge (updateWheelTelemetryBridge)
// which reads F411 UART lines and prefixes them with motor name (FL|RL|FR|RR|).
// The polling path (readResponse/pollAllResponses) is intentionally stubbed.
// g_enableMotorPolling is kept false and not user-accessible.
static bool g_enableMotorPolling = false;
static uint32_t lastMotorPollTime = 0;
#define MOTOR_POLL_INTERVAL_MS 500

// Wheel telemetry bridge — F411 UART RX -> PC (motor prefix ile)
static bool g_wheelBridgeEnabled = (WHEEL_BRIDGE_DEFAULT_ON != 0);

// Per-UART RX state for wheel telemetry bridge
struct WheelRxState {
    HardwareSerial* uart;
    const char* prefix;     // "FL"/"RL"/"FR"/"RR"
    char buf[WHEEL_RX_MAX_LINE];
    size_t pos;
    bool discarding;
    uint32_t lastTruncateMs;
};
static WheelRxState g_wheelRx[MOTOR_COUNT];

// Forward declarations
static void initEstop();
static void updateEstop();
static bool readEstopRaw();
static void handleEstopActivated();
static void handleEstopReleased();
static void sendEstopStopCommands(bool verbose);
static void printEstopStatusDetailed();
static void updateHeartbeat();
static void initRedRelay();
static void initGreenRelay();
static void initYellowRelay();
static void setRedRelay(bool on);
static void setGreenRelay(bool on);
static void setYellowRelay(bool on);
static void writeRelayOutput(uint8_t pin, bool on, uint8_t activeLevel, bool contactIsNc);
static void updateArmRxBridge();
static void setArmBridge(bool enabled);
static void printArmBridgeStatus();
static void stopWheelsAndArm();
static void pollMotors();
static void processCommand(const char* line);
static void handleMotionCommand(const Command& cmd);
static void handleRpmMotorCommand(const Command& cmd);
static void handleRpmQuery();
static void handleF411Forward(const Command& cmd);
static void handleIdentifyAll();
static void handleIdentifyOne(uint8_t motorId);
static void initWheelBridge();
static void updateWheelTelemetryBridge();
static void setWheelBridge(bool enabled);
static void printWheelBridgeStatus();
static void printBootBanner();

// ============================================================
// setup()
// ============================================================
void setup() {
#if HEARTBEAT_ENABLED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
#endif

#if ARM_UART_RX_ENABLED
    // Prevent floating ARM RX pin from flooding terminal before UART AF takes over
    pinMode(ARM_RX_PIN, INPUT_PULLUP);
#endif

    // Start terminal first
    terminal.begin(TERMINAL_BAUD);
    delay(150);

    terminal.println();
    terminal.println("BOOT 1/6: terminal started on USB Serial");

#if IWDG_ENABLED
    if (IWatchdog.isReset(true)) {
        terminal.println("BOOT WARN: IWDG reset detected");
    }
    IWatchdog.begin(IWDG_TIMEOUT_MS);
    if (!IWatchdog.isEnabled()) {
        terminal.println("BOOT ERR: IWDG init failed");
    } else {
        terminal.print("BOOT OK: IWDG armed, timeout=");
        terminal.print(IWDG_TIMEOUT_MS);
        terminal.println("ms");
    }
#endif

    initEstop();
    terminal.println("BOOT 2/6: estop initialized");

    // Wheel motor UART pin routing
    MOTOR_UART_FL.setRx(PD6);
    MOTOR_UART_FL.setTx(PD5);

    MOTOR_UART_RL.setRx(PE7);
    MOTOR_UART_RL.setTx(PE8);

    MOTOR_UART_FR.setRx(PD0);
    MOTOR_UART_FR.setTx(PD1);

    MOTOR_UART_RR.setRx(PD2);
    MOTOR_UART_RR.setTx(PC12);

    // ARM UART pin routing
    ARM_UART.setRx(PE0);
    ARM_UART.setTx(PE1);

    dispatcher.begin();
    terminal.println("BOOT 3/6: motor UARTs initialized (FL/RL/FR/RR + ARM)");

    status.reset();
    terminal.println("BOOT 4/6: status reset");

    initRedRelay();
    initGreenRelay();
    initYellowRelay();
    terminal.println("BOOT 5/6: relays initialized");

    initWheelBridge();
    terminal.print("BOOT 5.5: wheel telemetry bridge = ");
    terminal.println(g_wheelBridgeEnabled ? "ON" : "OFF");

    terminal.print("BOOT 6/6: arm RX bridge default = ");
    terminal.println(g_armRxBridgeEnabled ? "ON" : "OFF");

    // UART diagnostics
    bool uartOk[MOTOR_COUNT];
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        uartOk[i] = dispatcher.isReady(i);
    }
    bool armOk = dispatcher.isArmReady();

    char diagLine[64];
    snprintf(diagLine, sizeof(diagLine), "UART FL  : %s", uartOk[MOTOR_FL] ? "OK" : "HATA!");
    terminal.println(diagLine);
    snprintf(diagLine, sizeof(diagLine), "UART RL  : %s", uartOk[MOTOR_RL] ? "OK" : "HATA!");
    terminal.println(diagLine);
    snprintf(diagLine, sizeof(diagLine), "UART FR  : %s", uartOk[MOTOR_FR] ? "OK" : "HATA!");
    terminal.println(diagLine);
    snprintf(diagLine, sizeof(diagLine), "UART RR  : %s", uartOk[MOTOR_RR] ? "OK" : "HATA!");
    terminal.println(diagLine);
    snprintf(diagLine, sizeof(diagLine), "UART ARM : %s", armOk ? "OK" : "HATA!");
    terminal.println(diagLine);

    printBootBanner();
    terminal.showPrompt();
}

// ============================================================
// loop()
// ============================================================
void loop() {
#if IWDG_ENABLED
    IWatchdog.reload();
#endif
    updateHeartbeat();
    updateEstop();
    updateArmRxBridge();
    updateWheelTelemetryBridge();

    if (g_enableMotorPolling) {
        pollMotors();
    }

    if (!terminal.update()) {
#if IWDG_ENABLED
        IWatchdog.reload();
#endif
        return;
    }

    const char* line = terminal.readLine();
    processCommand(line);

#if IWDG_ENABLED
    IWatchdog.reload();
#endif
}

// ============================================================
// processCommand()
// ============================================================
static void processCommand(const char* line) {
    // Don't echo high-frequency gamepad packets
    bool rawArmJoystick = (strncasecmp(line, "arm j,", 6) == 0);

    if (!rawArmJoystick) {
        terminal.print("LINE=[");
        terminal.print(line);
        terminal.println("]");
    }

    Command cmd = parser.parse(line);

    if (!cmd.valid) {
        terminal.println(cmd.error ? cmd.error : "ERROR: Invalid command.");
        terminal.showPrompt();
        return;
    }

    // E-STOP active: block most commands, but allow stop/idempotent commands
    if (g_estopActive) {
        switch (cmd.type) {
            case CMD_HELP:
                parser.printHelp();
                break;
            case CMD_STATUS:
                status.printStatus();
                printEstopStatusDetailed();
                printArmBridgeStatus();
                break;
            case CMD_ESTOP_STATUS:
                printEstopStatusDetailed();
                break;
            case CMD_PING:
                terminal.println("pong");
                break;
            case CMD_WHEELBRIDGE_STATUS:
                printWheelBridgeStatus();
                break;
            case CMD_ARMBRIDGE_STATUS:
                printArmBridgeStatus();
                break;
            case CMD_STOP:
            case CMD_STOP_ALL:
                handleMotionCommand(cmd);
                break;
            default:
                if (!rawArmJoystick) {
                    terminal.println("E-STOP ACTIVE! Motion, identify and arm commands are blocked.");
                }
                break;
        }
        terminal.showPrompt();
        return;
    }

    switch (cmd.type) {
        case CMD_HELP:
            parser.printHelp();
            terminal.showPrompt();
            return;

        case CMD_STATUS:
            status.printStatus();
            printEstopStatusDetailed();
            printArmBridgeStatus();
            terminal.showPrompt();
            return;

        case CMD_PING:
            terminal.println("pong: main loop alive");
            terminal.showPrompt();
            return;

        case CMD_WHEELBRIDGE_ON:
            setWheelBridge(true);
            terminal.showPrompt();
            return;

        case CMD_WHEELBRIDGE_OFF:
            setWheelBridge(false);
            terminal.showPrompt();
            return;

        case CMD_WHEELBRIDGE_STATUS:
            printWheelBridgeStatus();
            terminal.showPrompt();
            return;

        case CMD_RPM_QUERY:
            handleRpmQuery();
            terminal.showPrompt();
            return;

        case CMD_RPM_FORWARD:
        case CMD_RPM_BACKWARD:
        case CMD_RPM_LEFT:
        case CMD_RPM_RIGHT:
        case CMD_RPM_STOP:
            handleMotionCommand(cmd);
            terminal.showPrompt();
            return;

        case CMD_RPM_MOTOR:
            handleRpmMotorCommand(cmd);
            terminal.showPrompt();
            return;

        case CMD_F411_FORWARD:
            handleF411Forward(cmd);
            terminal.showPrompt();
            return;

        case CMD_ESTOP_STATUS:
            printEstopStatusDetailed();
            terminal.showPrompt();
            return;

        case CMD_ARMBRIDGE_ON:
            setArmBridge(true);
            terminal.showPrompt();
            return;

        case CMD_ARMBRIDGE_OFF:
            setArmBridge(false);
            terminal.showPrompt();
            return;

        case CMD_ARMBRIDGE_STATUS:
            printArmBridgeStatus();
            terminal.showPrompt();
            return;

        case CMD_ARM: {
            bool ok = dispatcher.sendArmTextCommand(cmd.armText);
            if (!ok) {
                terminal.println("ERROR: ARM command send failure!");
            } else {
                terminal.print("ARM -> ");
                terminal.println(cmd.armText);
            }
            terminal.showPrompt();
            return;
        }

        case CMD_IDENTIFY:
            handleIdentifyAll();
            terminal.showPrompt();
            return;

        case CMD_IDENTIFY_ONE:
            handleIdentifyOne(cmd.motorId);
            terminal.showPrompt();
            return;

        case CMD_STOP_ALL:
            stopWheelsAndArm();
            terminal.showPrompt();
            return;

        case CMD_RED_ON:
            setRedRelay(true);
            terminal.println("RED relay ON (NC contact closed).");
            terminal.showPrompt();
            return;

        case CMD_RED_OFF:
            setRedRelay(false);
            terminal.println("RED relay OFF (NC contact opened).");
            terminal.showPrompt();
            return;

        case CMD_GREEN_ON:
            setGreenRelay(true);
            terminal.println("GREEN relay ON (NC contact closed).");
            terminal.showPrompt();
            return;

        case CMD_GREEN_OFF:
            setGreenRelay(false);
            terminal.println("GREEN relay OFF (NC contact opened).");
            terminal.showPrompt();
            return;

        case CMD_YELLOW_ON:
            setYellowRelay(true);
            terminal.println("YELLOW relay ON (NC contact closed).");
            terminal.showPrompt();
            return;

        case CMD_YELLOW_OFF:
            setYellowRelay(false);
            terminal.println("YELLOW relay OFF (NC contact opened).");
            terminal.showPrompt();
            return;

        case CMD_FORWARD:
        case CMD_BACKWARD:
        case CMD_LEFT:
        case CMD_RIGHT:
        case CMD_STOP:
            handleMotionCommand(cmd);
            terminal.showPrompt();
            return;

        default:
            terminal.println("ERROR: Command parsed but not handled.");
            terminal.showPrompt();
            return;
    }
}

// ============================================================
// Motion command handler
// ============================================================
static void handleMotionCommand(const Command& cmd) {
    // RPM modunda degeri RPM_MAX ile karsilastir, clamping yapilirsa uyar
    bool isRpmMotion = (cmd.type == CMD_RPM_FORWARD || cmd.type == CMD_RPM_BACKWARD ||
                        cmd.type == CMD_RPM_LEFT   || cmd.type == CMD_RPM_RIGHT);
    if (isRpmMotion && cmd.value > RPM_MAX) {
        char warn[80];
        snprintf(warn, sizeof(warn), "WARN: RPM %d clamped to %d", cmd.value, RPM_MAX);
        terminal.println(warn);
    }

    // Bare `stop` should stop both duty and speed modes — send "rpm 0" then "stop"
    if (cmd.type == CMD_STOP) {
        dispatcher.stopAll();
        for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
            MotorCommand m;
            m.motorId = (MotorId)i;
            m.mode = DRIVE_DUTY;
            m.direction = DIR_STOP;
            m.pwm = 0;
            m.rpm = 0;
            status.updateMotor(m);
        }
        terminal.println("OK: All motors stopped (rpm 0 + stop)");
        return;
    }

    MotorCommand motors[MOTOR_COUNT];
    uint8_t count = motion.compute(cmd, motors);

    if (count == 0) {
        terminal.println("ERROR: Motion could not be calculated.");
        return;
    }

    bool allSuccess = true;

    for (uint8_t i = 0; i < count; i++) {
        bool ok = dispatcher.sendMotorCommand(motors[i]);

        if (!ok) {
            allSuccess = false;
            char err[64];
            snprintf(err, sizeof(err), "ERROR: Motor %s send failure!",
                     motorName(motors[i].motorId));
            terminal.println(err);
            continue;
        }

        status.updateMotor(motors[i]);

        char logLine[96];
        if (motors[i].mode == DRIVE_SPEED) {
            // speed mod: rpm <signed>
            snprintf(logLine, sizeof(logLine), "TX -> %s : rpm %d",
                     motorName(motors[i].motorId), motors[i].rpm);
        } else if (motors[i].direction == DIR_STOP) {
            snprintf(logLine, sizeof(logLine), "TX -> %s : stop",
                     motorName(motors[i].motorId));
        } else if (motors[i].direction == DIR_FORWARD) {
            snprintf(logLine, sizeof(logLine), "TX -> %s : f%u",
                     motorName(motors[i].motorId), motors[i].pwm);
        } else {
            snprintf(logLine, sizeof(logLine), "TX -> %s : b%u",
                     motorName(motors[i].motorId), motors[i].pwm);
        }
        terminal.println(logLine);
    }

    status.incrementCommandCount();

    if (allSuccess) {
        terminal.println("OK: motion command sent.");
    } else {
        terminal.println("WARN: at least one motor TX failed.");
    }
}

// ============================================================
// Per-motor RPM command handler: rpm <motor|all> <signed>
// ============================================================
static void handleRpmMotorCommand(const Command& cmd) {
    int16_t rpm = (int16_t)cmd.signedValue;

    // Per-motor RPM clamp — motion_controller'daki clamp ile tutarli
    if (rpm > RPM_MAX) {
        char warn[80];
        snprintf(warn, sizeof(warn), "WARN: RPM %d clamped to %d", rpm, RPM_MAX);
        terminal.println(warn);
        rpm = RPM_MAX;
    } else if (rpm < -RPM_MAX) {
        char warn[80];
        snprintf(warn, sizeof(warn), "WARN: RPM %d clamped to %d", rpm, -RPM_MAX);
        terminal.println(warn);
        rpm = -RPM_MAX;
    }

    // yon: rpm>0 -> forward, rpm<0 -> backward, 0 -> stop
    MotorDirection dir = (rpm > 0) ? DIR_FORWARD : ((rpm < 0) ? DIR_BACKWARD : DIR_STOP);

    if (cmd.targetAll) {
        bool allSuccess = true;
        for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
            MotorCommand mc = { (MotorId)i, DRIVE_SPEED, dir, 0, rpm };
            if (!dispatcher.sendMotorCommand(mc)) {
                allSuccess = false;
                char err[64];
                snprintf(err, sizeof(err), "ERROR: Motor %s send failure!", motorName(i));
                terminal.println(err);
                continue;
            }
            status.updateMotor(mc);
            char logLine[64];
            snprintf(logLine, sizeof(logLine), "TX -> %s : rpm %d", motorName(i), rpm);
            terminal.println(logLine);
        }
        status.incrementCommandCount();
        terminal.println(allSuccess ? "OK: rpm command sent to all." : "WARN: at least one motor TX failed.");
        return;
    }

    if (cmd.motorId >= MOTOR_COUNT) {
        terminal.println("ERROR: invalid motor id.");
        return;
    }
    MotorCommand mc = { (MotorId)cmd.motorId, DRIVE_SPEED, dir, 0, rpm };
    if (!dispatcher.sendMotorCommand(mc)) {
        terminal.println("ERROR: Motor send failure!");
        return;
    }
    status.updateMotor(mc);
    char logLine[64];
    snprintf(logLine, sizeof(logLine), "TX -> %s : rpm %d", motorName(cmd.motorId), rpm);
    terminal.println(logLine);
    status.incrementCommandCount();
}

// ============================================================
// RPM query: rpm
// ============================================================
static void handleRpmQuery() {
    terminal.println("RPM targets (last sent):");
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        const MotorStatus& ms = status.getMotorStatus(i);
        char line[64];
        if (ms.driveMode == DRIVE_SPEED) {
            snprintf(line, sizeof(line), "  %s: SPEED rpm=%d", motorName(i), ms.rpm);
        } else {
            snprintf(line, sizeof(line), "  %s: DUTY pwm=%u", motorName(i), ms.pwm);
        }
        terminal.println(line);
    }
}

// ============================================================
// F411 forward handler: pi/base/boost/ramp/spstat/hall/.../raw/speed
// ============================================================
static void handleF411Forward(const Command& cmd) {
    if (cmd.f411Line[0] == '\0') {
        terminal.println("ERROR: empty F411 command.");
        return;
    }

    if (cmd.targetAll) {
        terminal.print("TX -> ALL : ");
        terminal.println(cmd.f411Line);
        bool allSuccess = true;
        for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
            if (!dispatcher.sendTextCommand(i, cmd.f411Line)) {
                allSuccess = false;
                char err[64];
                snprintf(err, sizeof(err), "ERROR: Motor %s send failure!", motorName(i));
                terminal.println(err);
                continue;
            }
            delay(F411_FWD_GAP_MS);
        }
        status.incrementCommandCount();
        terminal.println(allSuccess ? "OK: forwarded to all." : "WARN: at least one motor TX failed.");
        return;
    }

    if (cmd.motorId >= MOTOR_COUNT) {
        terminal.println("ERROR: invalid motor id.");
        return;
    }
    char logLine[96];
    snprintf(logLine, sizeof(logLine), "TX -> %s : %s", motorName(cmd.motorId), cmd.f411Line);
    terminal.println(logLine);
    if (!dispatcher.sendTextCommand(cmd.motorId, cmd.f411Line)) {
        terminal.println("ERROR: Motor send failure!");
        return;
    }
    status.incrementCommandCount();
}

// ============================================================
// Identify handlers
// ============================================================
static void handleIdentifyAll() {
    terminal.println("identify: sending to all motors.");

    bool okAll = true;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        terminal.print("IDENTIFY BEGIN -> ");
        terminal.println(motorName(i));

        bool ok = dispatcher.sendTextCommand(i, "identify");

        terminal.print(ok ? "IDENTIFY OK -> " : "IDENTIFY FAIL -> ");
        terminal.println(motorName(i));

        if (!ok) okAll = false;
        delay(IDENTIFY_BETWEEN_MS);
    }

    if (okAll) terminal.println("identify command sent to all motors.");
    else terminal.println("identify finished with at least one TX failure.");

    status.incrementCommandCount();
}

static void handleIdentifyOne(uint8_t motorId) {
    terminal.print("IDENTIFY BEGIN -> ");
    terminal.println(motorName(motorId));

    bool ok = dispatcher.sendTextCommand(motorId, "identify");

    terminal.print(ok ? "IDENTIFY OK -> " : "IDENTIFY FAIL -> ");
    terminal.println(motorName(motorId));

    status.incrementCommandCount();
}

// ============================================================
// Stop all (wheels + arm)
// ============================================================
static void stopWheelsAndArm() {
    terminal.println("STOP ALL: stopping all motors and arm.");
    // Guvenli stop: hem "stop" hem "rpm 0" (PWM + RPM cover)
    dispatcher.stopAll();
    dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        MotorCommand stopCmd = { (MotorId)i, DRIVE_DUTY, DIR_STOP, 0, 0 };
        status.updateMotor(stopCmd);
    }
    status.incrementCommandCount();
    terminal.println("  all stopped.");
}

// ============================================================
// E-STOP
// ============================================================
static bool readEstopRaw() {
    return (digitalRead(ESTOP_PIN) == ESTOP_ACTIVE_LEVEL);
}

static void initEstop() {
    pinMode(ESTOP_PIN, INPUT);
    bool raw = readEstopRaw();
    g_estopRawLast = raw;
    g_estopActive = raw;
    g_estopLastChangeMs = millis();
}

static void handleEstopActivated() {
    g_lastEstopReassertMs = millis();
    sendEstopStopCommands(false);

    terminal.println();
    terminal.println("!!! EMERGENCY STOP ACTIVE !!!");
    terminal.println("All motors stopped.");
    terminal.println("ARM stop command sent.");
    terminal.println("Motion, identify and arm commands are blocked until release.");
    terminal.showPrompt();
}

static void handleEstopReleased() {
    terminal.println();
    terminal.println("E-STOP released. Commands enabled again.");
    terminal.showPrompt();
}

static void sendEstopStopCommands(bool verbose) {
    // E-STOP: hem "stop" hem "rpm 0" — iki modu da guvenle durdur
    dispatcher.stopAll();
    dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        MotorCommand stopCmd = { (MotorId)i, DRIVE_DUTY, DIR_STOP, 0, 0 };
        status.updateMotor(stopCmd);
    }

    if (verbose) {
        terminal.println("E-STOP stop TX sent to all motors + arm.");
    }
}

static void updateEstop() {
    uint32_t now = millis();
    bool raw = readEstopRaw();

    if (raw != g_estopRawLast) {
        g_estopRawLast = raw;
        g_estopLastChangeMs = now;
    }

    if ((now - g_estopLastChangeMs) >= ESTOP_DEBOUNCE_MS) {
        if (raw != g_estopActive) {
            g_estopActive = raw;
            if (g_estopActive) handleEstopActivated();
            else handleEstopReleased();
        }
    }

    // While E-STOP is held, keep re-asserting stop
    if (g_estopActive && ((now - g_lastEstopReassertMs) >= ESTOP_REASSERT_INTERVAL_MS)) {
        g_lastEstopReassertMs = now;
        sendEstopStopCommands(false);
    }
}

static void printEstopStatusDetailed() {
    int rawLevel = digitalRead(ESTOP_PIN);
    terminal.println("E-STOP diagnostics:");
    terminal.print("  pin          : ");
    terminal.println("PB14");
    terminal.println("  input mode   : INPUT");
    terminal.print("  raw level    : ");
    terminal.println(rawLevel == HIGH ? "HIGH" : "LOW");
    terminal.print("  active level : ");
    terminal.println(ESTOP_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
    terminal.print("  state        : ");
    terminal.println(g_estopActive ? "ACTIVE" : "NORMAL");
}

// ============================================================
// Heartbeat LED
// ============================================================
static void updateHeartbeat() {
#if HEARTBEAT_ENABLED
    uint32_t now = millis();
    if ((now - g_lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
        g_lastHeartbeatMs = now;
        g_ledState = !g_ledState;
        digitalWrite(LED_BUILTIN, g_ledState ? HIGH : LOW);
    }
#endif
}

// ============================================================
// Motor polling — NOT IMPLEMENTED
// F411 telemetry is received via the wheelbridge
// (updateWheelTelemetryBridge).  This function is intentionally
// a no-op; g_enableMotorPolling is always false and not
// user-accessible.  Wheelbridge is the active telemetry path.
// ============================================================
static void pollMotors() {
    // No-op: wheelbridge telemetry is the active path.
    // readResponse/pollAllResponses are stubbed (see motor_dispatcher.cpp).
    (void)lastMotorPollTime;
}

// ============================================================
// ARM RX bridge
// ============================================================
static void updateArmRxBridge() {
#if ARM_UART_RX_ENABLED
    if (!g_armRxBridgeEnabled) return;

    uint8_t budget = ARM_RX_MAX_BYTES_PER_LOOP;
    while (budget-- > 0 && ARM_UART.available() > 0) {
        char c = static_cast<char>(ARM_UART.read());

        if (g_armRxDiscarding) {
            if (c == '\n') {
                g_armRxDiscarding = false;
                g_armRxPos = 0;
            }
            continue;
        }

        if (c == '\r') continue;

        if (c == '\n') {
            g_armRxBuf[g_armRxPos] = '\0';
            if (g_armRxPos > 0) {
                terminal.println(g_armRxBuf);
            }
            g_armRxPos = 0;
            continue;
        }

        if (g_armRxPos < (sizeof(g_armRxBuf) - 1)) {
            g_armRxBuf[g_armRxPos++] = c;
        } else {
            g_armRxPos = 0;
            g_armRxDiscarding = true;
            uint32_t now = millis();
            if ((now - g_lastArmTruncateNoticeMs) >= ARM_TRUNCATE_NOTICE_MS) {
                g_lastArmTruncateNoticeMs = now;
                terminal.println("WARN: ARM RX line too long/noisy; truncated. Use 'armbridge off' if arm is disconnected.");
            }
        }
    }
#endif
}

static void setArmBridge(bool enabled) {
#if ARM_UART_RX_ENABLED
    g_armRxBridgeEnabled = enabled;
    g_armRxPos = 0;
    g_armRxDiscarding = false;
    while (ARM_UART.available() > 0) {
        ARM_UART.read();
    }
    terminal.print("ARM RX bridge: ");
    terminal.println(g_armRxBridgeEnabled ? "ON" : "OFF");
#else
    (void)enabled;
    terminal.println("ARM RX bridge unavailable: ARM UART RX disabled at compile time.");
#endif
}

static void printArmBridgeStatus() {
    terminal.print("ARM UART: enabled, RX bridge=");
    terminal.println(g_armRxBridgeEnabled ? "ON" : "OFF");
}

// ============================================================
// Relay helpers
// ============================================================
static void writeRelayOutput(uint8_t pin, bool on, uint8_t activeLevel, bool contactIsNc) {
    const uint8_t inactiveLevel = (activeLevel == HIGH) ? LOW : HIGH;
    const bool coilEnergized = contactIsNc ? (!on) : on;
    digitalWrite(pin, coilEnergized ? activeLevel : inactiveLevel);
}

static void initRedRelay() {
    pinMode(RED_RELAY_PIN, OUTPUT);
    setRedRelay(RED_RELAY_DEFAULT_ON != 0);
}

static void initGreenRelay() {
    pinMode(GREEN_RELAY_PIN, OUTPUT);
    setGreenRelay(GREEN_RELAY_DEFAULT_ON != 0);
}

static void initYellowRelay() {
    pinMode(YELLOW_RELAY_PIN, OUTPUT);
    setYellowRelay(YELLOW_RELAY_DEFAULT_ON != 0);
}

static void setRedRelay(bool on) {
    writeRelayOutput(RED_RELAY_PIN, on, RED_RELAY_MODULE_ACTIVE_LEVEL, (RED_RELAY_CONTACT_IS_NC != 0));
    g_redRelayOn = on;
}

static void setGreenRelay(bool on) {
    writeRelayOutput(GREEN_RELAY_PIN, on, GREEN_RELAY_MODULE_ACTIVE_LEVEL, (GREEN_RELAY_CONTACT_IS_NC != 0));
    g_greenRelayOn = on;
}

static void setYellowRelay(bool on) {
    writeRelayOutput(YELLOW_RELAY_PIN, on, YELLOW_RELAY_MODULE_ACTIVE_LEVEL, (YELLOW_RELAY_CONTACT_IS_NC != 0));
    g_yellowRelayOn = on;
}

// ============================================================
// Wheel telemetry bridge (F411 -> PC)
// ============================================================
static void initWheelBridge() {
    // Per-UART RX state — explicit alan atama (array init sorununu onle)
    g_wheelRx[MOTOR_FL].uart = &MOTOR_UART_FL; g_wheelRx[MOTOR_FL].prefix = "FL";
    g_wheelRx[MOTOR_RL].uart = &MOTOR_UART_RL; g_wheelRx[MOTOR_RL].prefix = "RL";
    g_wheelRx[MOTOR_FR].uart = &MOTOR_UART_FR; g_wheelRx[MOTOR_FR].prefix = "FR";
    g_wheelRx[MOTOR_RR].uart = &MOTOR_UART_RR; g_wheelRx[MOTOR_RR].prefix = "RR";
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        g_wheelRx[i].pos = 0;
        g_wheelRx[i].discarding = false;
        g_wheelRx[i].lastTruncateMs = 0;
        g_wheelRx[i].buf[0] = '\0';
    }
}

static void updateWheelTelemetryBridge() {
    if (!g_wheelBridgeEnabled) return;

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        WheelRxState& st = g_wheelRx[i];
        HardwareSerial* uart = st.uart;
        if (!uart) continue;

        uint8_t budget = WHEEL_RX_MAX_BYTES_PER_LOOP;
        while (budget-- > 0 && uart->available() > 0) {
            char c = static_cast<char>(uart->read());

            if (st.discarding) {
                if (c == '\n') { st.discarding = false; st.pos = 0; }
                continue;
            }
            if (c == '\r') continue;

            if (c == '\n') {
                st.buf[st.pos] = '\0';
                if (st.pos > 0) {
                    // Motor prefix ile PC'ye yaz: FL|<line>
                    terminal.print(st.prefix);
                    terminal.print('|');
                    terminal.println(st.buf);
                }
                st.pos = 0;
                continue;
            }

            if (st.pos < (WHEEL_RX_MAX_LINE - 1)) {
                st.buf[st.pos++] = c;
            } else {
                // overflow: satir cok uzun, at ve uyar
                st.pos = 0;
                st.discarding = true;
                uint32_t now = millis();
                if ((now - st.lastTruncateMs) >= WHEEL_TRUNCATE_NOTICE_MS) {
                    st.lastTruncateMs = now;
                    terminal.print(st.prefix);
                    terminal.println("|WARN: RX line too long; truncated.");
                }
            }
        }
    }
}

static void setWheelBridge(bool enabled) {
    g_wheelBridgeEnabled = enabled;
    // Tum UART RX buffer'larini temizle ve state'i sifirla
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        g_wheelRx[i].pos = 0;
        g_wheelRx[i].discarding = false;
        if (g_wheelRx[i].uart) {
            while (g_wheelRx[i].uart->available() > 0) {
                g_wheelRx[i].uart->read();
            }
        }
    }
    terminal.print("Wheel telemetry bridge: ");
    terminal.println(g_wheelBridgeEnabled ? "ON" : "OFF");
}

static void printWheelBridgeStatus() {
    terminal.print("Wheel telemetry bridge: ");
    terminal.println(g_wheelBridgeEnabled ? "ON" : "OFF");
    terminal.println("F411 UART RX lines are prefixed with motor name (FL|RL|FR|RR|).");
}

// ============================================================
// Boot banner
// ============================================================
static void printBootBanner() {
    terminal.println();
    terminal.println("========================================");
    terminal.println("  NUCLEO-H723ZG Motor Control System");
    terminal.println("  4x Wheel UART + 1x ARM UART");
    terminal.println("========================================");
    terminal.println("Terminal UART : Serial (USB)");
    terminal.println("MOTOR FL      : Serial2  RX=PD6  TX=PD5");
    terminal.println("MOTOR RL      : Serial7  RX=PE7  TX=PE8");
    terminal.println("MOTOR FR      : Serial4  RX=PD0  TX=PD1");
    terminal.println("MOTOR RR      : Serial5  RX=PD2  TX=PC12");
    terminal.println("ARM UART      : Serial8  RX=PE0  TX=PE1");
    terminal.println();
    terminal.print("WheelBridge   : ");
    terminal.println(g_wheelBridgeEnabled ? "ON" : "OFF");
    terminal.println("Commands: forward/rpm forward, speed on/off, pi/base/boost/ramp, raw, wheelbridge");
    terminal.println("System ready. Type 'help'.");
    terminal.print("E-STOP      : ");
    terminal.println(g_estopActive ? "ACTIVE" : "NORMAL");
#if IWDG_ENABLED
    terminal.print("IWDG        : ");
    terminal.println(IWatchdog.isEnabled() ? "ARMED" : "FAILED");
#endif
    printArmBridgeStatus();
}
