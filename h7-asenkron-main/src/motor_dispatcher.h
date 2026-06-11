#ifndef MOTOR_DISPATCHER_H
#define MOTOR_DISPATCHER_H

#include <Arduino.h>
#include "types.h"
#include "config.h"

class MotorDispatcher {
public:
    void begin();

    bool isReady(uint8_t motorId);
    bool isArmReady();

    uint8_t send(uint8_t motorId, const uint8_t* buffer, uint8_t length);

    bool sendMotorCommand(const MotorCommand& cmd);
    bool sendTextCommand(uint8_t motorId, const char* text);
    bool sendArmTextCommand(const char* text);

    void stopAll();
    bool identifyAll();

    HardwareSerial* getUART(uint8_t motorId);
    HardwareSerial* getArmUART();

    bool readResponse(uint8_t motorId, MotorResponse& resp, uint32_t timeoutMs = 100);
    uint8_t pollAllResponses(MotorResponse responses[MOTOR_COUNT], uint32_t timeoutMs = 100);

private:
    HardwareSerial* _uartMap[MOTOR_COUNT];
    HardwareSerial* _armUart;

    bool sendLineSafe(HardwareSerial* uart, const char* text);
};

#endif // MOTOR_DISPATCHER_H