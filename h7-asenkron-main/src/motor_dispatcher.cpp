#include "motor_dispatcher.h"
#include <string.h>

void MotorDispatcher::begin() {
    _uartMap[MOTOR_FL] = &MOTOR_UART_FL;
    _uartMap[MOTOR_RL] = &MOTOR_UART_RL;
    _uartMap[MOTOR_FR] = &MOTOR_UART_FR;
    _uartMap[MOTOR_RR] = &MOTOR_UART_RR;
    _armUart = &ARM_UART;

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        _uartMap[i]->begin(MOTOR_UART_BAUD);
        delayMicroseconds(50);
    }

    _armUart->begin(ARM_UART_BAUD);
    delayMicroseconds(50);
}

bool MotorDispatcher::isReady(uint8_t motorId) {
    if (motorId >= MOTOR_COUNT) return false;
    HardwareSerial* uart = _uartMap[motorId];
    if (uart == nullptr) return false;
    return (uart->availableForWrite() > 0);
}

bool MotorDispatcher::isArmReady() {
    if (_armUart == nullptr) return false;
    return (_armUart->availableForWrite() > 0);
}

uint8_t MotorDispatcher::send(uint8_t motorId, const uint8_t* buffer, uint8_t length) {
    if (motorId >= MOTOR_COUNT || buffer == nullptr || length == 0) {
        return 0;
    }
    HardwareSerial* uart = _uartMap[motorId];
    if (uart == nullptr) {
        return 0;
    }
    return uart->write(buffer, length);
}

bool MotorDispatcher::sendLineSafe(HardwareSerial* uart, const char* text) {
    if (uart == nullptr || text == nullptr) return false;

    const size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        uint32_t start = millis();
        while (uart->availableForWrite() <= 0) {
            if ((millis() - start) >= UART_TX_CHAR_TIMEOUT_MS) {
                return false;
            }
            yield();
        }
        if (uart->write(static_cast<uint8_t>(text[i])) != 1) {
            return false;
        }
    }

    // Send newline
    uint32_t start = millis();
    while (uart->availableForWrite() <= 0) {
        if ((millis() - start) >= UART_TX_CHAR_TIMEOUT_MS) {
            return false;
        }
        yield();
    }
    if (uart->write(static_cast<uint8_t>('\n')) != 1) {
        return false;
    }

    delay(UART_TX_GAP_MS);
    return true;
}

bool MotorDispatcher::sendMotorCommand(const MotorCommand& cmd) {
    if (cmd.motorId >= MOTOR_COUNT) {
        return false;
    }

    char textCmd[16];

    switch (cmd.direction) {
        case DIR_FORWARD:
            snprintf(textCmd, sizeof(textCmd), "f%u", cmd.pwm);
            break;

        case DIR_BACKWARD:
            snprintf(textCmd, sizeof(textCmd), "b%u", cmd.pwm);
            break;

        case DIR_STOP:
        default:
            snprintf(textCmd, sizeof(textCmd), "stop");
            break;
    }

    return sendLineSafe(_uartMap[cmd.motorId], textCmd);
}

bool MotorDispatcher::sendTextCommand(uint8_t motorId, const char* text) {
    if (motorId >= MOTOR_COUNT) {
        return false;
    }
    return sendLineSafe(_uartMap[motorId], text);
}

bool MotorDispatcher::sendArmTextCommand(const char* text) {
    return sendLineSafe(_armUart, text);
}

void MotorDispatcher::stopAll() {
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        sendLineSafe(_uartMap[i], "stop");
    }
}

bool MotorDispatcher::identifyAll() {
    bool ok = true;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        if (!sendLineSafe(_uartMap[i], "identify")) ok = false;
        delay(IDENTIFY_BETWEEN_MS);
    }
    return ok;
}

HardwareSerial* MotorDispatcher::getUART(uint8_t motorId) {
    if (motorId >= MOTOR_COUNT) return nullptr;
    return _uartMap[motorId];
}

HardwareSerial* MotorDispatcher::getArmUART() {
    return _armUart;
}

bool MotorDispatcher::readResponse(uint8_t motorId, MotorResponse& resp, uint32_t timeoutMs) {
    (void)motorId;
    (void)timeoutMs;
    resp.valid = false;
    return false;
}

uint8_t MotorDispatcher::pollAllResponses(MotorResponse responses[MOTOR_COUNT], uint32_t timeoutMs) {
    (void)timeoutMs;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        responses[i].valid = false;
        responses[i].motorId = i;
    }
    return 0;
}
