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

// ============================================================
// Explicit UART objects for STM32F446RE
// Constructor order: HardwareSerial(RX, TX)
// ============================================================
HardwareSerial TerminalSerial(TERMINAL_RX_PIN, TERMINAL_TX_PIN);     // USART2 RX=PA3 TX=PA2
HardwareSerial WheelSerialFL(MOTOR_FL_RX_INIT_PIN, MOTOR_FL_TX_PIN); // USART1 TX=PA9
HardwareSerial WheelSerialFR(MOTOR_FR_RX_INIT_PIN, MOTOR_FR_TX_PIN); // USART3 TX=PC10
#if ARM_UART_ENABLED
HardwareSerial ArmSerial(ARM_RX_INIT_PIN, ARM_TX_PIN);               // USART6 RX=PC7 TX=PC6
#endif

TerminalInterface terminal;
CommandParser     parser;
MotionController  motion;
MotorDispatcher   dispatcher;
StatusManager     status;

static bool g_estopActive = false;
static bool g_estopRawLast = false;
static uint32_t g_estopLastChangeMs = 0;
static uint32_t g_lastHeartbeatMs = 0;
static uint32_t g_lastEstopReassertMs = 0;
static bool g_ledState = false;
static bool g_armRxBridgeEnabled = (ARM_RX_BRIDGE_DEFAULT_ON != 0);

#if ARM_UART_ENABLED && ARM_UART_RX_ENABLED
static char g_armRxBuf[ARM_RX_MAX_LINE];
static size_t g_armRxPos = 0;
static bool g_armRxDiscarding = false;
static uint32_t g_lastArmTruncateNoticeMs = 0;
#endif

static void initEstop();
static void updateEstop();
static bool readEstopRaw();
static void handleEstopActivated();
static void handleEstopReleased();
static void updateHeartbeat();
static void printBootBanner();
static void processCommand(const char* line);
static void handleMotionCommand(const Command& cmd);
static void handleIdentifyAll();
static void handleIdentifyOne(uint8_t motorId);
static void updateArmRxBridge();
static bool isArmJoystickPayload(const char* payload);
static bool startsWithNoCaseLocal(const char* s, const char* prefix);
static void printArmBridgeStatus();
static void setArmBridge(bool enabled);
static void stopWheelsAndArm();
static void sendEstopStopCommands(bool verbose);
static void printEstopStatusDetailed();

void setup() {
#if HEARTBEAT_ENABLED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
#endif

#if ARM_UART_ENABLED && ARM_UART_RX_ENABLED
    // This helps if the arm RX line is temporarily disconnected before UART AF takes over.
    pinMode(ARM_RX_PIN, INPUT_PULLUP);
#endif

    // Start PC terminal first so every next step can report what happened.
    terminal.begin(TERMINAL_BAUD);
    delay(150);

    terminal.println();
    terminal.println("BOOT 1/5: terminal started on explicit USART2 PA2/PA3");

    initEstop();
    terminal.println("BOOT 2/5: estop initialized");

    dispatcher.begin();
    terminal.println("BOOT 3/5: FL=USART1, FR=USART3 initialized; RL/RR disabled; ARM=USART6 initialized");

    status.reset();
    terminal.println("BOOT 4/5: status reset");

    terminal.print("BOOT 5/5: arm RX bridge default = ");
    terminal.println(g_armRxBridgeEnabled ? "ON" : "OFF");

    printBootBanner();
    terminal.showPrompt();
}

void loop() {
    updateHeartbeat();
    updateEstop();
    updateArmRxBridge();

    if (!terminal.update()) {
        return;
    }

    const char* line = terminal.readLine();
    processCommand(line);
}

static bool startsWithNoCaseLocal(const char* s, const char* prefix) {
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

static bool isArmJoystickPayload(const char* payload) {
    if (!payload) return false;
    while (*payload && isspace(static_cast<unsigned char>(*payload))) payload++;
    return startsWithNoCaseLocal(payload, "j,");
}

static void processCommand(const char* line) {
    const bool rawArmJoystick = startsWithNoCaseLocal(line, "arm j,");

    // Do not echo high-frequency gamepad packets to the terminal.
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

    if (g_estopActive) {
        if (cmd.type == CMD_HELP) {
            parser.printHelp();
            terminal.showPrompt();
        } else if (cmd.type == CMD_STATUS) {
            status.printStatus();
            printEstopStatusDetailed();
            printArmBridgeStatus();
            terminal.showPrompt();
        } else if (cmd.type == CMD_ESTOP_STATUS) {
            printEstopStatusDetailed();
            terminal.showPrompt();
        } else if (cmd.type == CMD_PING) {
            terminal.println("pong");
            terminal.showPrompt();
        } else if (cmd.type == CMD_ARMBRIDGE_STATUS) {
            printArmBridgeStatus();
            terminal.showPrompt();
        } else {
            if (!rawArmJoystick) {
                terminal.println("E-STOP ACTIVE! Motion, identify and arm commands are blocked.");
                terminal.showPrompt();
            }
        }
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
            const bool joystickPayload = isArmJoystickPayload(cmd.armText);
            bool ok = dispatcher.sendArmTextCommand(cmd.armText);

            if (joystickPayload) {
                // High-frequency gamepad stream: no console output, no prompt spam.
                return;
            }

            if (!ok) {
                terminal.println("ERROR: ARM command send failure / TX timeout.");
            } else {
                terminal.print("ARM -> ");
                terminal.println(cmd.armText);
            }
            terminal.showPrompt();
            return;
        }

        case CMD_IDENTIFY_ALL:
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

static void handleIdentifyAll() {
    terminal.println("identify: sending only to FL and FR. RL/RR are disabled.");

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

    if (okAll) terminal.println("identify command sent to FL and FR only.");
    else terminal.println("identify finished with at least one TX timeout/failure.");

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
            terminal.print("ERROR: Motor ");
            terminal.print(motorName(motors[i].motorId));
            terminal.println(" send failure / TX timeout.");
            continue;
        }

        status.updateMotor(motors[i].motorId, motors[i].direction, motors[i].pwm);

        terminal.print("TX -> ");
        terminal.print(motorName(motors[i].motorId));
        terminal.print(" : ");
        if (motors[i].direction == DIR_STOP) {
            terminal.println("stop");
        } else if (motors[i].direction == DIR_FORWARD) {
            terminal.print("f");
            terminal.println(motors[i].pwm);
        } else {
            terminal.print("b");
            terminal.println(motors[i].pwm);
        }
    }

    status.incrementCommandCount();

    if (allSuccess) {
        terminal.println("OK: motion command sent to FL/FR only.");
    } else {
        terminal.println("WARN: at least one active motor TX failed.");
    }
}

static void stopWheelsAndArm() {
    terminal.println("STOP ALL: stopping FL/FR and mechanical arm.");
    bool wheelsOk = dispatcher.stopAll();
    bool armOk = dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        status.updateMotor(i, DIR_STOP, 0);
    }
    status.incrementCommandCount();

    terminal.print("  wheels: ");
    terminal.println(wheelsOk ? "OK" : "TX failure/timeout");
    terminal.print("  arm   : ");
    terminal.println(armOk ? "OK" : "TX failure/timeout");
}

static void updateArmRxBridge() {
#if ARM_UART_ENABLED && ARM_UART_RX_ENABLED
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
#if ARM_UART_ENABLED && ARM_UART_RX_ENABLED
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
#if ARM_UART_ENABLED
    terminal.print("ARM UART: enabled, TX=PC6 RX=PC7, RX bridge=");
    terminal.println(g_armRxBridgeEnabled ? "ON" : "OFF");
#else
    terminal.println("ARM UART: disabled");
#endif
}

static void printEstopStatusDetailed() {
#if ESTOP_ENABLED
    int rawLevel = digitalRead(ESTOP_PIN);
    terminal.println("E-STOP diagnostics:");
    terminal.println("  pin          : PB0");
    terminal.println("  input mode   : INPUT_PULLUP");
    terminal.print("  raw level    : ");
    terminal.println(rawLevel == HIGH ? "HIGH" : "LOW");
    terminal.print("  active level : ");
    terminal.println(ESTOP_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
    terminal.print("  state        : ");
    terminal.println(g_estopActive ? "ACTIVE" : "NORMAL");
    terminal.println("  default wiring: PB0 -> E-STOP contact -> GND; closed/pressed = ACTIVE LOW");
    terminal.println("  note: if your switch is fail-safe NC-to-GND, set ESTOP_ACTIVE_LEVEL to HIGH in config.h");
#else
    terminal.println("E-STOP diagnostics: DISABLED at compile time");
#endif
}

static bool readEstopRaw() {
#if ESTOP_ENABLED
    return (digitalRead(ESTOP_PIN) == ESTOP_ACTIVE_LEVEL);
#else
    return false;
#endif
}

static void initEstop() {
#if ESTOP_ENABLED
    pinMode(ESTOP_PIN, INPUT_PULLUP);
#endif
    bool raw = readEstopRaw();
    g_estopRawLast = raw;
    g_estopActive = raw;
    g_estopLastChangeMs = millis();
}

static void sendEstopStopCommands(bool verbose) {
    bool wheelsOk = dispatcher.stopAll();
    bool armOk = dispatcher.sendArmTextCommand("stop");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        status.updateMotor(i, DIR_STOP, 0);
    }

    if (verbose) {
        terminal.print("E-STOP stop TX -> wheels: ");
        terminal.print(wheelsOk ? "OK" : "TX timeout/fail");
        terminal.print(", arm: ");
        terminal.println(armOk ? "OK" : "TX timeout/fail");
    }
}

static void handleEstopActivated() {
    g_lastEstopReassertMs = millis();
    sendEstopStopCommands(false);

    terminal.println();
    terminal.println("!!! EMERGENCY STOP ACTIVE !!!");
    terminal.println("FL/FR stopped.");
    terminal.println("ARM stop command sent.");
    terminal.println("Motion, identify and arm commands are blocked until release.");
    terminal.showPrompt();
}

static void handleEstopReleased() {
    terminal.println();
    terminal.println("E-STOP released. Commands enabled again.");
    terminal.showPrompt();
}

static void updateEstop() {
#if ESTOP_ENABLED
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

    // While E-STOP is held, keep re-asserting stop so a lower controller
    // cannot keep a stale PWM command after a noisy packet stream.
    if (g_estopActive && ((now - g_lastEstopReassertMs) >= ESTOP_REASSERT_INTERVAL_MS)) {
        g_lastEstopReassertMs = now;
        sendEstopStopCommands(false);
    }
#endif
}

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

static void printBootBanner() {
    terminal.println();
    terminal.println("========================================");
    terminal.println("  NUCLEO-F446RE FL/FR + ARM Bridge");
    terminal.println("========================================");
    terminal.println("PC Terminal : USART2  RX=PA3  TX=PA2");
    terminal.println("Wheel FL    : USART1  TX=PA9  RX=PA10(optional)");
    terminal.println("Wheel FR    : USART3  TX=PC10 RX=PC11(optional)");
    terminal.println("Wheel RL/RR : DISABLED - no UART init, no TX");
    terminal.println("ARM bridge  : USART6  TX=PC6  RX=PC7");
    terminal.println("Motion map  : physical-test corrected FL/FR map");
    terminal.println();
    terminal.println("System ready. Type 'help'.");
    terminal.print("E-STOP      : ");
#if ESTOP_ENABLED
    terminal.print(g_estopActive ? "ACTIVE" : "NORMAL");
    terminal.println(" (PB0, INPUT_PULLUP, active LOW default)");
#else
    terminal.println("DISABLED");
#endif
    printArmBridgeStatus();
}
