#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include "config.h"
#include "types.h"
#include "terminal_interface.h"
#include "command_parser.h"
#include "motion_controller.h"
#include "motor_dispatcher.h"
#include "status_manager.h"
#include "variant_NUCLEO_H723ZG.h"

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

// Motor polling
static bool g_enableMotorPolling = false;
static uint32_t lastMotorPollTime = 0;
#define MOTOR_POLL_INTERVAL_MS 500

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
static void handleIdentifyAll();
static void handleIdentifyOne(uint8_t motorId);
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
    updateHeartbeat();
    updateEstop();
    updateArmRxBridge();

    if (g_enableMotorPolling) {
        pollMotors();
    }

    if (!terminal.update()) {
        return;
    }

    const char* line = terminal.readLine();
    processCommand(line);
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

    // E-STOP active: block most commands
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
            case CMD_ARMBRIDGE_STATUS:
                printArmBridgeStatus();
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

        status.updateMotor(motors[i].motorId, motors[i].direction, motors[i].pwm);

        char logLine[96];
        if (motors[i].direction == DIR_STOP) {
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
    dispatcher.stopAll();
    dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        status.updateMotor(i, DIR_STOP, 0);
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
    dispatcher.stopAll();
    dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        status.updateMotor(i, DIR_STOP, 0);
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
// Motor polling
// ============================================================
static void pollMotors() {
    uint32_t now = millis();
    if ((now - lastMotorPollTime) < MOTOR_POLL_INTERVAL_MS) {
        return;
    }
    lastMotorPollTime = now;

    MotorResponse responses[MOTOR_COUNT];
    dispatcher.pollAllResponses(responses, PROTOCOL_TIMEOUT_MS);
    status.updateFromResponses(responses);
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
    terminal.println("System ready. Type 'help'.");
    terminal.print("E-STOP      : ");
    terminal.println(g_estopActive ? "ACTIVE" : "NORMAL");
    printArmBridgeStatus();
}
