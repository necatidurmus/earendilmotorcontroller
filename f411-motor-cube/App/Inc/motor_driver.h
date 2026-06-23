/* ============================================================
 * App/Inc/motor_driver.h
 * Low-level BLDC power-stage control.
 * No analogWrite / digitalWrite. All TIM1 CCR + channel enable
 * bits are written directly.
 * ============================================================ */
#ifndef MOTOR_DRIVER_H
#define MOTOR_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise TIM1 channels 1/2/3 (and N). Outputs start off. */
void MotorDriver_Init(void);

/* Disable every gate. Safe reset / default state. */
void MotorDriver_AllOff(void);

/* Alias of AllOff for the spec terminology. */
void MotorDriver_Coast(void);
void MotorDriver_FaultOff(void);

/* Set a global duty that will be used on the next ApplyStep. */
void MotorDriver_SetDuty(uint8_t duty);

/* Apply a 6-step sector.
 *   sector    = 0..5 (electrical step, raw Hall order)
 *   direction = +1 forward, -1 reverse, 0 coast (same as AllOff)
 *   duty      = 0..255, applied as TIM1 CCR.
 *
 * The function guarantees no cross-conduction in the same phase:
 * before driving phase X high it forces phase X low off (and vice versa).
 * It is safe to call from the main loop, NOT from an ISR hot path. */
void MotorDriver_ApplyStep(uint8_t sector, int8_t direction, uint8_t duty);

/* Returns the duty currently commanded (0..255). */
uint8_t  MotorDriver_GetDuty(void);

/* Returns the duty currently held in TIM1 CCR (0..PWM_PERIOD_TICKS). */
uint16_t MotorDriver_GetCurrentCcrTicks(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_DRIVER_H */
