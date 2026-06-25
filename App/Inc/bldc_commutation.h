/* ============================================================
 * App/Inc/bldc_commutation.h
 * Hall raw -> electrical sector mapping and 6-step drive table.
 * ============================================================ */
#ifndef BLDC_COMMUTATION_H
#define BLDC_COMMUTATION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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

/* ---- Hall map validation (safe-apply workflow) ---- */

/* Validate a Hall map: raw 0/7 must be 255, raw 1..6 must be 0..5,
 * sectors 0..5 each appear exactly once. Returns true if valid. */
bool Commutation_ValidateHallMap(const uint8_t map[8]);

/* Like Commutation_ValidateHallMap() but writes a human-readable
 * reason string on failure. reason may be NULL if not needed. */
bool Commutation_ValidateHallMapVerbose(const uint8_t map[8],
                                        char *reason, size_t reason_len);

/* True if all of map[1]..map[6] are valid sectors (0..5). */
bool Commutation_IsCompleteHallMap(const uint8_t map[8]);

/* True if any sector appears more than once in map[1]..map[6]. */
bool Commutation_HasDuplicateSectors(const uint8_t map[8]);

/* Copy src map to dst (8 bytes). */
void Commutation_CopyMap(uint8_t dst[8], const uint8_t src[8]);

/* Apply an already-validated map atomically to the active map.
 * MUST be called with the motor stopped. The caller must have
 * validated the map before calling this. */
void Commutation_ApplyMap(const uint8_t map[8]);

#ifdef __cplusplus
}
#endif

#endif /* BLDC_COMMUTATION_H */
