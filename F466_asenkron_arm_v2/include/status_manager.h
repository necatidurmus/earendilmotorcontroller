#ifndef STATUS_MANAGER_H
#define STATUS_MANAGER_H

#include <Arduino.h>
#include "types.h"

class StatusManager {
public:
    void reset();
    void updateMotor(uint8_t motorId, Direction direction, uint8_t pwm);
    void incrementCommandCount();
    uint32_t commandCount() const;
    void printStatus();

private:
    Direction directions_[MOTOR_COUNT];
    uint8_t pwms_[MOTOR_COUNT];
    uint32_t commandCount_ = 0;
};

#endif // STATUS_MANAGER_H
