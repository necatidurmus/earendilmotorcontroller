#include "motion_controller.h"

uint8_t MotionController::compute(const Command& cmd, MotorCommand outMotors[MOTOR_COUNT]) {
    // First stop all motors
    setAll(outMotors, DIR_STOP, 0);

    switch (cmd.type) {
        case CMD_FORWARD:
            // All motors forward
            setAll(outMotors, DIR_FORWARD, (uint8_t)cmd.pwm);
            break;

        case CMD_BACKWARD:
            // All motors backward
            setAll(outMotors, DIR_BACKWARD, (uint8_t)cmd.pwm);
            break;

        case CMD_LEFT:
            // Left motors forward, right motors backward → left turn
            outMotors[MOTOR_FL] = { MOTOR_FL, DIR_FORWARD,  (uint8_t)cmd.pwm };
            outMotors[MOTOR_RL] = { MOTOR_RL, DIR_FORWARD,  (uint8_t)cmd.pwm };
            outMotors[MOTOR_FR] = { MOTOR_FR, DIR_BACKWARD, (uint8_t)cmd.pwm };
            outMotors[MOTOR_RR] = { MOTOR_RR, DIR_BACKWARD, (uint8_t)cmd.pwm };
            break;

        case CMD_RIGHT:
            // Left motors backward, right motors forward → right turn
            outMotors[MOTOR_FL] = { MOTOR_FL, DIR_BACKWARD, (uint8_t)cmd.pwm };
            outMotors[MOTOR_RL] = { MOTOR_RL, DIR_BACKWARD, (uint8_t)cmd.pwm };
            outMotors[MOTOR_FR] = { MOTOR_FR, DIR_FORWARD,  (uint8_t)cmd.pwm };
            outMotors[MOTOR_RR] = { MOTOR_RR, DIR_FORWARD,  (uint8_t)cmd.pwm };
            break;

        case CMD_STOP:
            // Already stopped all above
            break;

        default:
            return 0;
    }

    // Save last commands
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        _lastMotors[i] = outMotors[i];
    }

    return MOTOR_COUNT;
}

const MotorCommand* MotionController::getLastCommands() const {
    return _lastMotors;
}

void MotionController::setAll(MotorCommand motors[MOTOR_COUNT], MotorDirection dir, uint8_t pwm) {
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        motors[i].motorId   = (MotorId)i;
        motors[i].direction = dir;
        motors[i].pwm       = pwm;
    }
}
