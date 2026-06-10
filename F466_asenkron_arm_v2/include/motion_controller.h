#ifndef MOTION_CONTROLLER_H
#define MOTION_CONTROLLER_H

#include <Arduino.h>
#include "types.h"

class MotionController {
public:
    uint8_t compute(const Command& cmd, MotorCommand out[MOTOR_COUNT]);

private:
    static void setMotor(MotorCommand& mc, uint8_t id, Direction dir, uint8_t pwm);
};

#endif // MOTION_CONTROLLER_H
