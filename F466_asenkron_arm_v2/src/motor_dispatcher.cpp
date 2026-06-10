#include "motor_dispatcher.h"
#include <string.h>

void MotorDispatcher::begin() {
    // FL/FR ONLY: initialize only USART1 and USART3 for wheels.
    // UART4 and UART5 are not initialized, so the firmware never talks to RL/RR.
    MOTOR_UART_FL.begin(MOTOR_UART_BAUD);
    MOTOR_UART_FR.begin(MOTOR_UART_BAUD);

#if ARM_UART_ENABLED
    ARM_UART.begin(ARM_UART_BAUD);
#endif
}

HardwareSerial* MotorDispatcher::portFor(uint8_t motorId) {
    switch (motorId) {
        case MOTOR_FL: return &MOTOR_UART_FL;
        case MOTOR_FR: return &MOTOR_UART_FR;
        default:       return nullptr;
    }
}

bool MotorDispatcher::isReady(uint8_t motorId) const {
    return motorId < MOTOR_COUNT;
}

bool MotorDispatcher::isArmReady() const {
#if ARM_UART_ENABLED
    return true;
#else
    return false;
#endif
}

bool MotorDispatcher::sendLineSafe(HardwareSerial& port, const char* text) {
    if (!text) return false;

    const size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        uint32_t start = millis();
        while (port.availableForWrite() <= 0) {
            if ((millis() - start) >= UART_TX_CHAR_TIMEOUT_MS) {
                return false;
            }
            yield();
        }
        if (port.write(static_cast<uint8_t>(text[i])) != 1) {
            return false;
        }
    }

    uint32_t start = millis();
    while (port.availableForWrite() <= 0) {
        if ((millis() - start) >= UART_TX_CHAR_TIMEOUT_MS) {
            return false;
        }
        yield();
    }
    if (port.write(static_cast<uint8_t>('\n')) != 1) {
        return false;
    }

    // Do NOT call flush(); the main loop must remain alive even if a line
    // or a lower controller is faulty.
    delay(UART_TX_GAP_MS);
    return true;
}

bool MotorDispatcher::sendTextCommand(uint8_t motorId, const char* text) {
    HardwareSerial* p = portFor(motorId);
    if (!p) return false;
    return sendLineSafe(*p, text);
}

bool MotorDispatcher::sendMotorCommand(const MotorCommand& cmd) {
    char out[16];

    if (cmd.direction == DIR_STOP || cmd.pwm == 0) {
        strcpy(out, "stop");
    } else if (cmd.direction == DIR_FORWARD) {
        snprintf(out, sizeof(out), "f%u", cmd.pwm);
    } else if (cmd.direction == DIR_BACKWARD) {
        snprintf(out, sizeof(out), "b%u", cmd.pwm);
    } else {
        return false;
    }

    return sendTextCommand(cmd.motorId, out);
}

bool MotorDispatcher::stopAll() {
    bool ok = true;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        if (!sendTextCommand(i, "stop")) ok = false;
    }
    return ok;
}

bool MotorDispatcher::identifyAll() {
    bool ok = true;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        if (!sendTextCommand(i, "identify")) ok = false;
        delay(IDENTIFY_BETWEEN_MS);
    }
    return ok;
}

bool MotorDispatcher::sendArmTextCommand(const char* text) {
#if ARM_UART_ENABLED
    return sendLineSafe(ARM_UART, text);
#else
    (void)text;
    return false;
#endif
}
