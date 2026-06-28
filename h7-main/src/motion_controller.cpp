#include "motion_controller.h"
#include "config.h"

// Motor komutunu elle doldur — mode'a gore pwm/rpm alanlarini set et
static inline MotorCommand makeCmd(MotorId id, DriveMode mode,
                                   MotorDirection dir, uint8_t pwm, int16_t rpm) {
    MotorCommand m;
    m.motorId   = id;
    m.mode      = mode;
    m.direction = dir;
    m.pwm       = (mode == DRIVE_DUTY)  ? pwm : 0;
    m.rpm       = (mode == DRIVE_SPEED) ? rpm : 0;
    return m;
}

uint8_t MotionController::compute(const Command& cmd, MotorCommand outMotors[MOTOR_COUNT]) {
    // cmd.type'tan surus modu ve yon tipini cikar (paylasimli mapping)
    DriveMode mode;
    CommandType motionType;

    switch (cmd.type) {
        // PWM (duty) movement
        case CMD_FORWARD:     mode = DRIVE_DUTY;  motionType = CMD_FORWARD;  break;
        case CMD_BACKWARD:    mode = DRIVE_DUTY;  motionType = CMD_BACKWARD; break;
        case CMD_LEFT:        mode = DRIVE_DUTY;  motionType = CMD_LEFT;     break;
        case CMD_RIGHT:       mode = DRIVE_DUTY;  motionType = CMD_RIGHT;    break;
        case CMD_STOP:        mode = DRIVE_DUTY;  motionType = CMD_STOP;     break;
        // RPM (speed) movement — ayni yon mapping
        case CMD_RPM_FORWARD:  mode = DRIVE_SPEED; motionType = CMD_FORWARD;  break;
        case CMD_RPM_BACKWARD: mode = DRIVE_SPEED; motionType = CMD_BACKWARD; break;
        case CMD_RPM_LEFT:     mode = DRIVE_SPEED; motionType = CMD_LEFT;     break;
        case CMD_RPM_RIGHT:    mode = DRIVE_SPEED; motionType = CMD_RIGHT;    break;
        case CMD_RPM_STOP:     mode = DRIVE_SPEED; motionType = CMD_STOP;     break;
        default:
            return 0;   // motion controller'in ilgilendirmedigi komut
    }

    // Deger cozumle — mode'a gore clamp
    int value = cmd.value;
    if (value < 0) value = 0;
    if (mode == DRIVE_DUTY) {
        if (value > PWM_MAX) value = PWM_MAX;
    } else {
        if (value > RPM_MAX) value = RPM_MAX;
    }

    // Once tum motorlari durdur (mode uygun: duty->stop, speed->rpm 0)
    setAll(outMotors, mode, DIR_STOP, 0, 0);

    switch (motionType) {
        case CMD_FORWARD:
            // Tum motorlar ileri — speed: +rpm, duty: f<pwm>
            setAll(outMotors, mode, DIR_FORWARD, (uint8_t)value, (int16_t)+value);
            break;

        case CMD_BACKWARD:
            // Tum motorlar geri — speed: -rpm, duty: b<pwm>
            setAll(outMotors, mode, DIR_BACKWARD, (uint8_t)value, (int16_t)(-value));
            break;

        case CMD_LEFT:
            // Sol motorlar ileri, sag motorlar geri -> sola donus
            outMotors[MOTOR_FL] = makeCmd(MOTOR_FL, mode, DIR_FORWARD,  (uint8_t)value, (int16_t)+value);
            outMotors[MOTOR_RL] = makeCmd(MOTOR_RL, mode, DIR_FORWARD,  (uint8_t)value, (int16_t)+value);
            outMotors[MOTOR_FR] = makeCmd(MOTOR_FR, mode, DIR_BACKWARD, (uint8_t)value, (int16_t)(-value));
            outMotors[MOTOR_RR] = makeCmd(MOTOR_RR, mode, DIR_BACKWARD, (uint8_t)value, (int16_t)(-value));
            break;

        case CMD_RIGHT:
            // Sol motorlar geri, sag motorlar ileri -> saga donus
            outMotors[MOTOR_FL] = makeCmd(MOTOR_FL, mode, DIR_BACKWARD, (uint8_t)value, (int16_t)(-value));
            outMotors[MOTOR_RL] = makeCmd(MOTOR_RL, mode, DIR_BACKWARD, (uint8_t)value, (int16_t)(-value));
            outMotors[MOTOR_FR] = makeCmd(MOTOR_FR, mode, DIR_FORWARD,  (uint8_t)value, (int16_t)+value);
            outMotors[MOTOR_RR] = makeCmd(MOTOR_RR, mode, DIR_FORWARD,  (uint8_t)value, (int16_t)+value);
            break;

        case CMD_STOP:
            // Yukarida setAll(STOP) yapildi
            break;

        default:
            return 0;
    }

    // Son komutlari kaydet
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        _lastMotors[i] = outMotors[i];
    }

    return MOTOR_COUNT;
}

const MotorCommand* MotionController::getLastCommands() const {
    return _lastMotors;
}

void MotionController::setAll(MotorCommand motors[MOTOR_COUNT], DriveMode mode,
                              MotorDirection dir, uint8_t pwm, int16_t rpm) {
    for (uint8_t i = 0; i < MOTOR_COUNT; i++) {
        motors[i] = makeCmd((MotorId)i, mode, dir, pwm, rpm);
    }
}
