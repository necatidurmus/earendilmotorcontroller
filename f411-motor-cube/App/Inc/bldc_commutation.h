/* ============================================================
 * App/Inc/bldc_commutation.h
 * Hall raw -> electrical sector mapping and 6-step drive table.
 * ============================================================ */
#ifndef BLDC_COMMUTATION_H
#define BLDC_COMMUTATION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the default Hall map into RAM. */
void Commutation_LoadDefaultMap(void);

/* Read the current Hall map from RAM into the supplied buffer. */
void Commutation_GetMap(uint8_t out[8]);

/* Override a single Hall->state mapping (runtime only).
 * Returns false if hallCode > 7 or state is not 0..5 / 255. */
bool Commutation_SetMapEntry(uint8_t hallCode, uint8_t state);

/* Return mapped sector (0..5) for a 3-bit Hall code, or 255 if invalid. */
uint8_t Commutation_HallToState(uint8_t hallCode);

/* Check whether the 3-bit Hall code is a valid mapped state. */
bool Commutation_IsValidState(uint8_t state);

/* True if the (prev -> next) step is a single-step change (+/-1 mod 6). */
bool Commutation_IsTransitionValid(uint8_t prev, uint8_t next);

/* 6-step high/low phase lookup.
 *   sector     = 0..5 electrical state
 *   direction  = +1 forward, -1 reverse
 *   high_phase = 0..2 -> A,B,C index (PWM phase)
 *   low_phase  = 0..2 -> A,B,C index (forced-on low-side) */
void Commutation_GetDrivePhases(uint8_t sector, int8_t direction,
                                uint8_t *high_phase, uint8_t *low_phase);

#ifdef __cplusplus
}
#endif

#endif /* BLDC_COMMUTATION_H */
