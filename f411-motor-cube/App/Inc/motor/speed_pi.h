/* ============================================================
 * App/Inc/speed_pi.h
 * Speed PI controller state machine:
 *   STOPPED -> START_BOOST -> SPEED_PI -> (FAULT)
 * ============================================================ */
#ifndef SPEED_PI_H
#define SPEED_PI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPD_IDLE = 0,
    SPD_START_BOOST = 1,
    SPD_SPEED_PI = 2
} SpeedPhase;

typedef enum {
    SPD_FAULT_NONE = 0,
    SPD_FAULT_NO_HALL = 1,
    SPD_FAULT_INVALID_HALL = 2
} SpeedFault;

void SpeedPI_Init(void);
void SpeedPI_Reset(void);
void SpeedPI_Enable(void);
void SpeedPI_Disable(void);
bool SpeedPI_IsEnabled(void);

/* Set a target. Signed: +forward, -reverse, 0 = stop/coast. */
void SpeedPI_SetTargetRpm(int32_t rpm);

/* Called from the 50–100 Hz scheduler. Non-blocking. */
void SpeedPI_Tick(uint32_t nowMs);

/* Current target (after ramp), in absolute RPM. */
float SpeedPI_GetRampedTargetRpm(void);
int32_t SpeedPI_GetRawTargetRpm(void);
uint16_t SpeedPI_GetComputedDuty(void);
SpeedPhase SpeedPI_GetPhase(void);
SpeedFault SpeedPI_GetFault(void);

float SpeedPI_GetK(void);
void SpeedPI_SetKp(float kp);
void SpeedPI_SetKi(float ki);
void SpeedPI_SetBasePwm(uint16_t low, uint16_t mid, uint16_t high);
void SpeedPI_SetBoostPwm(uint16_t low, uint16_t mid, uint16_t high, uint16_t ms);
void SpeedPI_SetRamp(float upPerSec, float downPerSec);

void SpeedPI_GetGains(float *kp, float *ki);
void SpeedPI_GetBasePwm(uint16_t *low, uint16_t *mid, uint16_t *high);
void SpeedPI_GetBoostPwm(uint16_t *low, uint16_t *mid, uint16_t *high, uint16_t *ms);
void SpeedPI_GetRampRates(float *up, float *down);

#ifdef __cplusplus
}
#endif

#endif /* SPEED_PI_H */
