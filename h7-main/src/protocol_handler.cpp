#include "protocol_handler.h"

uint8_t ProtocolHandler::calcChecksum(const uint8_t* data, uint8_t length) {
    uint8_t cs = 0;
    for (uint8_t i = 0; i < length; i++) {
        cs ^= data[i];
    }
    return cs;
}

uint8_t ProtocolHandler::encodeSetMotor(const MotorCommand& cmd, uint8_t* buffer) {
    // Packet: [SYNC] [LEN] [CMD] [DIR] [PWM] [CHECKSUM]
    // LEN = 3 (CMD + DIR + PWM)
    uint8_t idx = 0;
    buffer[idx++] = PROTOCOL_SYNC_BYTE;           // [0] SYNC
    buffer[idx++] = 0x03;                          // [1] LEN
    buffer[idx++] = PROTO_SET_MOTOR;               // [2] CMD
    buffer[idx++] = cmd.direction;                 // [3] DIR
    buffer[idx++] = cmd.pwm;                       // [4] PWM

    // Calculate checksum: LEN ^ CMD ^ DIR ^ PWM
    uint8_t cs = calcChecksum(&buffer[1], 4);      // 4 bytes including LEN
    buffer[idx++] = cs;                            // [5] CHECKSUM

    return idx; // Total 6 bytes
}

uint8_t ProtocolHandler::encodeStop(uint8_t motorId, uint8_t* buffer) {
    // Packet: [SYNC] [LEN] [CMD] [MOTOR_ID] [CHECKSUM]
    // LEN = 2 (CMD + MOTOR_ID)
    uint8_t idx = 0;
    buffer[idx++] = PROTOCOL_SYNC_BYTE;           // [0] SYNC
    buffer[idx++] = 0x02;                          // [1] LEN
    buffer[idx++] = PROTO_STOP;                    // [2] CMD
    buffer[idx++] = motorId;                       // [3] MOTOR_ID

    uint8_t cs = calcChecksum(&buffer[1], 3);
    buffer[idx++] = cs;                            // [4] CHECKSUM

    return idx; // Total 5 bytes
}

uint8_t ProtocolHandler::encodeGetStatus(uint8_t motorId, uint8_t* buffer) {
    // Packet: [SYNC] [LEN] [CMD] [MOTOR_ID] [CHECKSUM]
    // LEN = 2 (CMD + MOTOR_ID)
    uint8_t idx = 0;
    buffer[idx++] = PROTOCOL_SYNC_BYTE;           // [0] SYNC
    buffer[idx++] = 0x02;                          // [1] LEN
    buffer[idx++] = PROTO_GET_STATUS;              // [2] CMD
    buffer[idx++] = motorId;                       // [3] MOTOR_ID

    uint8_t cs = calcChecksum(&buffer[1], 3);
    buffer[idx++] = cs;                            // [4] CHECKSUM

    return idx; // Total 5 bytes
}

bool ProtocolHandler::decodeResponse(const uint8_t* buffer, uint8_t length, uint8_t motorId, MotorResponse& resp) {
    resp.valid = false;
    resp.motorId = motorId;

    // Minimum packet: [SYNC] [LEN] [RESP_CMD] [CHECKSUM] = 4 bytes
    if (length < 4) return false;

    // Check SYNC byte
    if (buffer[0] != PROTOCOL_SYNC_BYTE) return false;

    // Check LEN field
    uint8_t len = buffer[1];
    // Total packet size = SYNC(1) + LEN(1) + payload(LEN) + checksum(1)
    if (length < (2 + len + 1)) return false;

    // Verify checksum: XOR of bytes from LEN through last data byte
    uint8_t cs = calcChecksum(&buffer[1], len + 1); // len+1 includes LEN byte itself
    if (cs != buffer[2 + len]) return false;

    // Parse response command
    uint8_t respCmd = buffer[2];
    resp.cmd = respCmd;

    if (respCmd == PROTO_ACK) {
        // ACK: [SYNC] [LEN=1] [ACK] [CHECKSUM]
        resp.direction = DIR_STOP;
        resp.pwm = 0;
        resp.status = 0;
        resp.valid = true;
        return true;
    }

    if (respCmd == PROTO_NACK) {
        // NACK: [SYNC] [LEN=2] [NACK] [ERROR_CODE] [CHECKSUM]
        resp.direction = DIR_STOP;
        resp.pwm = 0;
        resp.status = (len >= 2) ? buffer[3] : 0xFF;
        resp.valid = true;
        return true;
    }

    if (respCmd == PROTO_STATUS_RESP) {
        // STATUS: [SYNC] [LEN=4] [STATUS_RESP] [DIR] [PWM] [STATUS_FLAGS] [CHECKSUM]
        if (len < 4) return false;
        resp.direction = static_cast<MotorDirection>(buffer[3]);
        resp.pwm = buffer[4];
        resp.status = buffer[5];
        resp.valid = true;
        return true;
    }

    // Unknown response type
    return false;
}
