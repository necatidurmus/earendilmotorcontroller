#ifndef MOTION_CONTROLLER_H
#define MOTION_CONTROLLER_H

#include "types.h"

class MotionController {
public:
    // Takes a command, produces a MotorCommand array for 4 motors.
    // Returns: number of generated commands (0-4)
    uint8_t compute(const Command& cmd, MotorCommand outMotors[MOTOR_COUNT]);

    // Returns the last motion state
    const MotorCommand* getLastCommands() const;

private:
    MotorCommand _lastMotors[MOTOR_COUNT];

    // Helper: assign the same direction and PWM to all motors
    void setAll(MotorCommand motors[MOTOR_COUNT], MotorDirection dir, uint8_t pwm);
};

#endif // MOTION_CONTROLLER_H
