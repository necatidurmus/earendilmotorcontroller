#include "status_manager.h"
#include "terminal_interface.h"

void StatusManager::reset() {
    _state.commandCount      = 0;
    _state.lastCommandTime   = 0;

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        _state.motors[i].driveMode   = DRIVE_DUTY;
        _state.motors[i].direction   = DIR_STOP;
        _state.motors[i].pwm         = 0;
        _state.motors[i].rpm         = 0;
        _state.motors[i].active      = false;
        _state.motors[i].lastUpdate  = 0;
        _state.motors[i].errorCount  = 0;
    }
}

void StatusManager::updateMotor(const MotorCommand& cmd) {
    if (cmd.motorId >= MOTOR_COUNT) return;

    bool active = (cmd.mode == DRIVE_SPEED) ? (cmd.rpm != 0)
                                            : (cmd.direction != DIR_STOP || cmd.pwm > 0);
    _state.motors[cmd.motorId].driveMode  = cmd.mode;
    _state.motors[cmd.motorId].direction  = cmd.direction;
    _state.motors[cmd.motorId].pwm        = cmd.pwm;
    _state.motors[cmd.motorId].rpm        = cmd.rpm;
    _state.motors[cmd.motorId].active      = active;
    _state.motors[cmd.motorId].lastUpdate  = millis();
}

void StatusManager::incrementCommandCount() {
    _state.commandCount++;
    _state.lastCommandTime = millis();
}

const SystemState& StatusManager::getState() const {
    return _state;
}

const MotorStatus& StatusManager::getMotorStatus(uint8_t motorId) const {
    if (motorId >= MOTOR_COUNT) motorId = 0;
    return _state.motors[motorId];
}

void StatusManager::updateFromResponse(const MotorResponse& resp) {
    if (!resp.valid) return;
    if (resp.motorId >= MOTOR_COUNT) return;

    _state.motors[resp.motorId].driveMode  = resp.driveMode;
    _state.motors[resp.motorId].direction  = resp.direction;
    _state.motors[resp.motorId].pwm        = resp.pwm;
    _state.motors[resp.motorId].rpm        = resp.rpm;
    _state.motors[resp.motorId].active      = (resp.direction != DIR_STOP || resp.pwm > 0 || resp.rpm != 0);
    _state.motors[resp.motorId].lastUpdate  = millis();

    // Reset error count on successful response
    _state.motors[resp.motorId].errorCount = 0;
}

void StatusManager::updateFromResponses(const MotorResponse responses[MOTOR_COUNT]) {
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        if (responses[i].valid) {
            updateFromResponse(responses[i]);
        } else {
            // Increment error count for motors that didn't respond
            if (_state.motors[i].errorCount < UINT16_MAX) _state.motors[i].errorCount++;
        }
    }
}

void StatusManager::printStatus() const {
    char line[80];
    const char* dirNames[] = { "BACK", "FWD ", "STOP" };

    terminal.println("========================================");
    terminal.println("  System Status");
    terminal.println("========================================");

    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        const char* dirStr = (_state.motors[i].direction < 3)
                             ? dirNames[_state.motors[i].direction]
                             : "????";
        const char* actStr = _state.motors[i].active ? "ACTIVE" : "IDLE  ";
        const char* modeStr = (_state.motors[i].driveMode == DRIVE_SPEED) ? "SPEED" : "DUTY ";

        if (_state.motors[i].driveMode == DRIVE_SPEED) {
            snprintf(line, sizeof(line), "  Motor %s: %s RPM=%d [%s] %s",
                     motorName(i), dirStr, _state.motors[i].rpm, modeStr, actStr);
        } else {
            snprintf(line, sizeof(line), "  Motor %s: %s PWM=%u [%s] %s",
                     motorName(i), dirStr, _state.motors[i].pwm, modeStr, actStr);
        }
        terminal.println(line);
    }

    terminal.println("----------------------------------------");

    snprintf(line, sizeof(line), "  Total commands: %lu", (unsigned long)_state.commandCount);
    terminal.println(line);

    if (_state.lastCommandTime > 0) {
        uint32_t elapsed = millis() - _state.lastCommandTime;
        snprintf(line, sizeof(line), "  Last command: %lu ms ago", (unsigned long)elapsed);
        terminal.println(line);
    } else {
        terminal.println("  Last command: none");
    }

    terminal.println("========================================");
}
