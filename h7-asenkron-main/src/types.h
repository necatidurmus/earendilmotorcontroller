#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

constexpr size_t ARM_COMMAND_TEXT_MAX = 64;

// ============================================================
// Motor channel definitions
// ============================================================
enum MotorId : uint8_t {
    MOTOR_FL = 0,   // Front-Left
    MOTOR_RL = 1,   // Rear-Left
    MOTOR_FR = 2,   // Front-Right
    MOTOR_RR = 3,   // Rear-Right
    MOTOR_COUNT = 4
};

// ============================================================
// Motor direction
// ============================================================
enum MotorDirection : uint8_t {
    DIR_BACKWARD = 0,
    DIR_FORWARD  = 1,
    DIR_STOP     = 2
};

// ============================================================
// Command types (from terminal)
// ============================================================
enum CommandType : uint8_t {
    CMD_FORWARD,
    CMD_BACKWARD,
    CMD_LEFT,
    CMD_RIGHT,
    CMD_STOP,
    CMD_STOP_ALL,
    CMD_IDENTIFY,
    CMD_IDENTIFY_ONE,
    CMD_STATUS,
    CMD_HELP,
    CMD_PING,
    CMD_ESTOP_STATUS,
    CMD_ARM,
    CMD_ARMBRIDGE_ON,
    CMD_ARMBRIDGE_OFF,
    CMD_ARMBRIDGE_STATUS,
    CMD_RED_ON,
    CMD_RED_OFF,
    CMD_GREEN_ON,
    CMD_GREEN_OFF,
    CMD_YELLOW_ON,
    CMD_YELLOW_OFF,
    CMD_INVALID
};

// ============================================================
// Protocol command types (H7 → F411)
// ============================================================
enum ProtocolCmd : uint8_t {
    PROTO_SET_MOTOR  = 0x01,
    PROTO_STOP       = 0x02,
    PROTO_GET_STATUS = 0x03
};

// ============================================================
// Protocol response types (F411 → H7)
// ============================================================
enum ProtocolResponse : uint8_t {
    PROTO_ACK           = 0x81,
    PROTO_STATUS_RESP   = 0x82,
    PROTO_NACK          = 0x83
};

// ============================================================
// Response data received from F411 motor controller
// ============================================================
struct MotorResponse {
    uint8_t        motorId;
    uint8_t        cmd;
    MotorDirection direction;
    uint8_t        pwm;
    uint8_t        status;
    bool           valid;
};

// ============================================================
// Parsed terminal command
// ============================================================
struct Command {
    CommandType type;
    int         pwm;
    bool        valid;
    const char* error;
    uint8_t     motorId;
    char        armText[ARM_COMMAND_TEXT_MAX];
};

struct MotorCommand {
    MotorId        motorId;
    MotorDirection direction;
    uint8_t        pwm;
};

struct MotorStatus {
    MotorDirection direction;
    uint8_t        pwm;
    bool           active;
    uint32_t       lastUpdate;
    uint8_t        errorCount;
};

// ============================================================
// Entire system status information
// ============================================================
struct SystemState {
    MotorStatus motors[MOTOR_COUNT];
    uint32_t    commandCount;
    uint32_t    lastCommandTime;
};

#endif // TYPES_H