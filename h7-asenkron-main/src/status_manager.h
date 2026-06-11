#ifndef STATUS_MANAGER_H
#define STATUS_MANAGER_H

#include "types.h"
#include "config.h"

class StatusManager {
public:
    // Resets system state
    void reset();

    // Updates motor states (after a command is sent)
    void updateMotor(uint8_t motorId, MotorDirection dir, uint8_t pwm);

    // Increments the command counter
    void incrementCommandCount();

    // Returns the system state
    const SystemState& getState() const;

    // Prints status to terminal (for the status command)
    void printStatus() const;

    // Returns motor status information
    const MotorStatus& getMotorStatus(uint8_t motorId) const;

    // Updates motor state from an actual F411 response
    void updateFromResponse(const MotorResponse& resp);

    // Updates all motors from an array of responses
    void updateFromResponses(const MotorResponse responses[MOTOR_COUNT]);

private:
    SystemState _state;
};

#endif // STATUS_MANAGER_H
