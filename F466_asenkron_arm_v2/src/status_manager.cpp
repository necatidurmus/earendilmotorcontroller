#include "status_manager.h"
#include "config.h"

static const char* dirName(Direction d) {
    switch (d) {
        case DIR_FORWARD: return "forward";
        case DIR_BACKWARD: return "backward";
        case DIR_STOP:
        default: return "stop";
    }
}

void StatusManager::reset() {
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        directions_[i] = DIR_STOP;
        pwms_[i] = 0;
    }
    commandCount_ = 0;
}

void StatusManager::updateMotor(uint8_t motorId, Direction direction, uint8_t pwm) {
    if (motorId >= MOTOR_COUNT) return;
    directions_[motorId] = direction;
    pwms_[motorId] = pwm;
}

void StatusManager::incrementCommandCount() {
    commandCount_++;
}

uint32_t StatusManager::commandCount() const {
    return commandCount_;
}

void StatusManager::printStatus() {
    TERMINAL_UART.println("Status:");
    TERMINAL_UART.print("  command_count: ");
    TERMINAL_UART.println(commandCount_);
#if MOTOR_UART_RX_ENABLED
    TERMINAL_UART.println("  motor_uart_mode: TX+RX for FL/FR only");
#else
    TERMINAL_UART.println("  motor_uart_mode: TX-only for FL/FR only");
#endif
#if ESTOP_ENABLED
    TERMINAL_UART.println("  estop: enabled");
#else
    TERMINAL_UART.println("  estop: disabled in config.h");
#endif
#if ARM_UART_ENABLED
    TERMINAL_UART.println("  arm_uart: enabled on USART6 PC6/PC7");
#else
    TERMINAL_UART.println("  arm_uart: disabled");
#endif
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        TERMINAL_UART.print("  ");
        TERMINAL_UART.print(motorName(i));
        TERMINAL_UART.print(": ");
        TERMINAL_UART.print(dirName(directions_[i]));
        TERMINAL_UART.print(" pwm=");
        TERMINAL_UART.println(pwms_[i]);
    }
}
