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

    if (cmd.mode == DRIVE_SPEED) {
        // Speed mode: rpm <signed> — pozitif=ileri, negatif=geri, 0=dur
        // stop komutu yerine rpm 0 gonderilir (F411 speed modda kalir)
        snprintf(textCmd, sizeof(textCmd), "rpm %d", cmd.rpm);
    } else {
        // Duty mode: f<pwm> / b<pwm> / stop
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
    // Guvenli stop: once "rpm 0" (speed modda durur, mod korur),
    // sonra "stop" (duty modda durur). Ters sira olsa "stop" disablePidMode
    // yapar ve "rpm 0" ERR verirdi.
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        sendLineSafe(_uartMap[i], "rpm 0");
        sendLineSafe(_uartMap[i], "stop");
    }
}

void MotorDispatcher::sendToAll(const char* text) {
    // Tum F411'lere ayni komut satirini gonder (forwarding / broadcast)
    if (!text) return;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        sendLineSafe(_uartMap[i], text);
        delay(F411_FWD_GAP_MS);
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
    // NOT IMPLEMENTED — intentionally stubbed.
    // F411 telemetry is received via the wheelbridge (updateWheelTelemetryBridge)
    // which reads F411 UART lines and prefixes them with motor name (FL|RL|FR|RR|).
    // This polling path is not used; wheelbridge is the active telemetry path.
    (void)motorId;
    (void)timeoutMs;
    resp.valid = false;
    return false;
}

uint8_t MotorDispatcher::pollAllResponses(MotorResponse responses[MOTOR_COUNT], uint32_t timeoutMs) {
    // NOT IMPLEMENTED — intentionally stubbed.
    // See readResponse() comment.  Wheelbridge telemetry is the active path.
    (void)timeoutMs;
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        responses[i].valid = false;
        responses[i].motorId = i;
    }
    return 0;
}
