#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

// ============================================================
// Active wheel count for this build
// ============================================================
// FL and FR are active. RL and RR are deliberately absent.
// Mechanical arm is handled separately through the `arm <payload>` bridge.
#define MOTOR_COUNT 2

enum MotorId : uint8_t {
    MOTOR_FL = 0,
    MOTOR_FR = 1
};

enum Direction : uint8_t {
    DIR_STOP = 0,
    DIR_FORWARD = 1,
    DIR_BACKWARD = 2
};

enum CommandType : uint8_t {
    CMD_INVALID = 0,
    CMD_FORWARD,
    CMD_BACKWARD,
    CMD_LEFT,
    CMD_RIGHT,
    CMD_STOP,
    CMD_STOP_ALL,
    CMD_IDENTIFY_ALL,
    CMD_IDENTIFY_ONE,
    CMD_STATUS,
    CMD_HELP,
    CMD_PING,
    CMD_ESTOP_STATUS,
    CMD_ARM,
    CMD_ARMBRIDGE_ON,
    CMD_ARMBRIDGE_OFF,
    CMD_ARMBRIDGE_STATUS
};

struct Command {
    bool valid = false;
    CommandType type = CMD_INVALID;
    int pwm = 0;
    uint8_t motorId = 0;
    char armText[120] = {0};  // payload after the `arm` prefix, preserving case
    const char* error = nullptr;
};

struct MotorCommand {
    uint8_t motorId = 0;
    Direction direction = DIR_STOP;
    uint8_t pwm = 0;
};

#endif // TYPES_H
