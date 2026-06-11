#include <Arduino.h>
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

// Motor polling — periodically request status from F411 cards
uint32_t lastMotorPollTime = 0;
#define MOTOR_POLL_INTERVAL_MS 500  // Poll every 500ms
void pollMotors();

// ============================================================
// setup() — System initialization
// ============================================================
void setup() {
    // Configure FTDI UART (USART6) with custom pins
    FTDI_UART.setRx(PG9);
    FTDI_UART.setTx(PG14);

    // Initialize terminal UART
    terminal.begin(TERMINAL_BAUD);

    // Initialize motor UARTs
    dispatcher.begin();

    // Reset status tracking
    status.reset();

    // Startup message
    delay(500); // Wait for UART to stabilize
    terminal.println("");
    terminal.println("========================================");
    terminal.println("  NUCLEO-H723ZG Motor Control System");
    terminal.println("  4x UART -> 4x STM32F411 Motor Control");
    terminal.println("========================================");
    terminal.println("System ready. Type 'help'.");
    terminal.showPrompt();
}

// ============================================================
// loop() — Main loop
// ============================================================
void loop() {
    if (!terminal.update()) {
        return;
    }

    const char* line = terminal.readLine();
    Command cmd = parser.parse(line);

    if (!cmd.valid) {
        if (cmd.error != nullptr) {
            terminal.println(cmd.error);
        } else if (cmd.type == CMD_INVALID) {
            terminal.println("ERROR: Unknown command. Type 'help' for usage.");
        } else {
            terminal.println("ERROR: PWM parameter missing or invalid (0-255).");
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
            terminal.showPrompt();
            return;

        default:
            terminal.println("COMMAND OK");
            terminal.showPrompt();
            return;
    }
}   