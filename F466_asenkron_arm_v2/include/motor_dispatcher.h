#ifndef MOTOR_DISPATCHER_H
#define MOTOR_DISPATCHER_H

#include <Arduino.h>
#include "types.h"
#include "config.h"

class MotorDispatcher {
public:
    void begin();
    bool isReady(uint8_t motorId) const;
    bool isArmReady() const;

    bool sendMotorCommand(const MotorCommand& cmd);
    bool sendTextCommand(uint8_t motorId, const char* text);
    bool stopAll();
    bool identifyAll();

    bool sendArmTextCommand(const char* text);

private:
    HardwareSerial* portFor(uint8_t motorId);
    bool sendLineSafe(HardwareSerial& port, const char* text);
};

#endif // MOTOR_DISPATCHER_H
