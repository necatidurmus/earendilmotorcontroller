#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

constexpr size_t ARM_COMMAND_TEXT_MAX = 64;
constexpr size_t F411_LINE_MAX = 96;   // F411'e forward edilen tam komut satiri

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
// Drive mode — H7'den F411'e sürüş modu
// ============================================================
enum DriveMode : uint8_t {
    DRIVE_DUTY  = 0,   // manuel PWM: f/b/stop
    DRIVE_SPEED = 1    // RPM kapali dongu: rpm <signed>
};

// ============================================================
// Command types (from terminal)
// ============================================================
enum CommandType : uint8_t {
    // PWM movement (eski, degismedi)
    CMD_FORWARD,
    CMD_BACKWARD,
    CMD_LEFT,
    CMD_RIGHT,
    CMD_STOP,
    CMD_STOP_ALL,
    CMD_IDENTIFY,
    CMD_IDENTIFY_ONE,
    CMD_STATUS,          // H7 sistem status (target yok)
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
    // RPM movement (speed PI mode)
    CMD_RPM_FORWARD,
    CMD_RPM_BACKWARD,
    CMD_RPM_LEFT,
    CMD_RPM_RIGHT,
    CMD_RPM_STOP,
    CMD_RPM_MOTOR,       // per-motor: rpm <motor> <signed>
    CMD_RPM_QUERY,       // rpm (sorgu)
    // Generic F411 forwarding (pi/base/boost/ramp/spstat/hall/map/save/
    // reload/mapreset/clrerr/status<motor>/raw/speed on/off/speedcfg)
    CMD_F411_FORWARD,
    // Wheel telemetry bridge
    CMD_WHEELBRIDGE_ON,
    CMD_WHEELBRIDGE_OFF,
    CMD_WHEELBRIDGE_STATUS,
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
    int16_t        rpm;        // olculen RPM (telemetri parse)
    DriveMode      driveMode;  // SP: 0=duty, 1=speed
    uint8_t        status;
    bool           valid;
};

// ============================================================
// Parsed terminal command
// ============================================================
struct Command {
    CommandType type;
    int         value;        // motion magnitude (pwm veya rpm, unsigned)
    int         signedValue;  // isaretli rpm (per-motor: rpm fl -23)
    bool        valid;
    const char* error;
    uint8_t     motorId;      // hedef motor (targetAll=false iken)
    bool        targetAll;    // tum motorlara uygula
    char        f411Line[F411_LINE_MAX];   // F411'e forward edilen tam komut
    char        armText[ARM_COMMAND_TEXT_MAX];
};

struct MotorCommand {
    MotorId        motorId;
    DriveMode      mode;       // bu komutun surus modu
    MotorDirection direction;  // duty mode yon
    uint8_t        pwm;        // duty mode PWM (0-255)
    int16_t        rpm;        // speed mode RPM (isaret=yon)
};

struct MotorStatus {
    DriveMode      driveMode;
    MotorDirection direction;
    uint8_t        pwm;
    int16_t        rpm;
    bool           active;
    uint32_t       lastUpdate;
    uint16_t       errorCount;
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