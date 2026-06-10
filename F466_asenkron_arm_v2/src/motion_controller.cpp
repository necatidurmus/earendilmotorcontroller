#include "motion_controller.h"

void MotionController::setMotor(MotorCommand& mc, uint8_t id, Direction dir, uint8_t pwm) {
    mc.motorId = id;
    mc.direction = dir;
    mc.pwm = pwm;
}

uint8_t MotionController::compute(const Command& cmd, MotorCommand out[MOTOR_COUNT]) {
    if (!cmd.valid || !out) return 0;

    const uint8_t pwm = static_cast<uint8_t>(cmd.pwm);

    switch (cmd.type) {
        case CMD_FORWARD:
            // Corrected after physical test:
            // FL forward + FR backward makes the rover go forward.
            setMotor(out[0], MOTOR_FL, DIR_FORWARD, pwm);
            setMotor(out[1], MOTOR_FR, DIR_BACKWARD, pwm);
            return MOTOR_COUNT;

        case CMD_BACKWARD:
            setMotor(out[0], MOTOR_FL, DIR_BACKWARD, pwm);
            setMotor(out[1], MOTOR_FR, DIR_FORWARD, pwm);
            return MOTOR_COUNT;

        case CMD_LEFT:
            setMotor(out[0], MOTOR_FL, DIR_BACKWARD, pwm);
            setMotor(out[1], MOTOR_FR, DIR_BACKWARD, pwm);
            return MOTOR_COUNT;

        case CMD_RIGHT:
            setMotor(out[0], MOTOR_FL, DIR_FORWARD, pwm);
            setMotor(out[1], MOTOR_FR, DIR_FORWARD, pwm);
            return MOTOR_COUNT;

        case CMD_STOP:
        case CMD_STOP_ALL:
            setMotor(out[0], MOTOR_FL, DIR_STOP, 0);
            setMotor(out[1], MOTOR_FR, DIR_STOP, 0);
            return MOTOR_COUNT;

        default:
            return 0;
    }
}
