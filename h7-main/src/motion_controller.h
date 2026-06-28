#ifndef MOTION_CONTROLLER_H
#define MOTION_CONTROLLER_H

#include "types.h"

class MotionController {
public:
    // Komutu 4 motor icin MotorCommand dizisine cevir.
    // cmd.type'tan surus modunu cikar (PWM_* -> DRIVE_DUTY, RPM_* -> DRIVE_SPEED).
    // Yon mapping PWM ve RPM icin ortak (paylasimli).
    // Returns: uretilen komut sayisi (0 veya MOTOR_COUNT)
    uint8_t compute(const Command& cmd, MotorCommand outMotors[MOTOR_COUNT]);

    // Returns the last motion state
    const MotorCommand* getLastCommands() const;

private:
    MotorCommand _lastMotors[MOTOR_COUNT];

    // Helper: tum motorlara ayni yon/deger ata
    void setAll(MotorCommand motors[MOTOR_COUNT], DriveMode mode,
                MotorDirection dir, uint8_t pwm, int16_t rpm);
};

#endif // MOTION_CONTROLLER_H
