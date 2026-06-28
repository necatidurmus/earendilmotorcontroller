#ifndef PROTOCOL_HANDLER_H
#define PROTOCOL_HANDLER_H

#include "types.h"
#include "config.h"

class ProtocolHandler {
public:
    // Converts a MotorCommand to a byte packet.
    // Returns: packet size (bytes), written to buffer.
    // Buffer must be at least PROTOCOL_MAX_PACKET in size.
    static uint8_t encodeSetMotor(const MotorCommand& cmd, uint8_t* buffer);

    // Creates a STOP command packet
    static uint8_t encodeStop(uint8_t motorId, uint8_t* buffer);

    // Creates a GET_STATUS command packet
    static uint8_t encodeGetStatus(uint8_t motorId, uint8_t* buffer);

    // Calculate checksum (XOR based)
    static uint8_t calcChecksum(const uint8_t* data, uint8_t length);

    // Decode a response packet from F411 motor controller
    // Response format: [SYNC 0xAA] [LEN] [RESP_CMD] [DIR] [PWM] [STATUS] [CHECKSUM]
    // Returns true if valid response decoded into 'resp'
    static bool decodeResponse(const uint8_t* buffer, uint8_t length, uint8_t motorId, MotorResponse& resp);

    // Packet format:
    // [SYNC 0xAA] [LEN] [CMD] [DATA...] [CHECKSUM]
    // LEN = number of CMD + DATA bytes
    // CHECKSUM = LEN ^ CMD ^ DATA[0] ^ DATA[1] ^ ...
};

#endif // PROTOCOL_HANDLER_H
