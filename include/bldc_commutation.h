/*
 * bldc_commutation.h — 6-step trapezoidal commutation and output stage
 *
 * Maps commutation state (0..5) to phase output patterns.
 * Handles high-side PWM + low-side static ON/OFF + third phase floating.
 * Applies optional dead-time on state transitions (non-blocking).
 *
 * Drive style: asynchronous (high-side PWM, low-side digital).
 * This matches the current PCB routing (L6388 gate drivers, no complementary PWM).
 */

#ifndef BLDC_COMMUTATION_H
#define BLDC_COMMUTATION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Commutation output pattern for one state */
typedef struct {
    uint16_t ah_pwm;    /* high-side A duty (0..PWM_PERIOD_COUNTS) */
    uint16_t bh_pwm;    /* high-side B duty */
    uint16_t ch_pwm;    /* high-side C duty */
    bool     al_on;     /* low-side A static ON */
    bool     bl_on;     /* low-side B static ON */
    bool     cl_on;     /* low-side C static ON */
} PhasePattern;

/* Initialize output stage (all off) */
void Comm_Init(void);

/*
 * Apply a commutation state with given duty.
 *   state: 0..5 (from hall mapping)
 *   duty_raw: 0..255 (will be scaled to PWM period)
 *
 * If stateChanged is true, inserts a brief all-off dead-time before
 * applying the new pattern. Dead-time is non-blocking (uses flag).
 */
void Comm_Apply(uint8_t state, uint16_t duty_pwm, bool stateChanged);

/* Force all outputs off (safe stop) */
void Comm_AllOff(void);

/* Get currently active drive state (for diagnostics) */
uint8_t Comm_GetActiveState(void);
uint16_t Comm_GetActiveDuty(void);

#ifdef __cplusplus
}
#endif

#endif /* BLDC_COMMUTATION_H */
