/*
 * hall.h — Hall sensor reading, debouncing, and state mapping
 *
 * Responsibilities:
 *   - Oversampled majority-vote hall reading
 *   - Polarity mask / inversion
 *   - Profile-based hall-to-commutation-state lookup
 *   - State offset adjustment
 *   - Debounce (min interval between state changes)
 *   - Invalid hall hold (keep last valid state for timeout period)
 *   - Diagnostic snapshot for CLI
 */

#ifndef HALL_H
#define HALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hall processing configuration — set once at init */
typedef struct {
    uint8_t profile;            /* 0..HALL_PROFILE_COUNT-1 */
    uint8_t polarityMask;       /* XOR mask: 0x07 = invert all, 0 = normal */
    int8_t  stateOffset;        /* commutation state offset: -5..+5 */
} HallConfig;

/* Hall diagnostic snapshot — populated by Hall_GetSnapshot() */
typedef struct {
    uint8_t raw;                /* raw hall bits (0..7) */
    uint8_t corrected;          /* after polarity mask */
    uint8_t mapped;             /* after profile lookup (0..5 or 255) */
    uint8_t accepted;           /* debounced accepted state (0..5 or 255) */
    uint8_t driveState;         /* final drive state after direction (0..5 or 255) */
} HallSnapshot;

/* Initialize hall module with config */
void Hall_Init(const HallConfig *cfg);

/* Update config at runtime (called from CLI) */
void Hall_SetProfile(uint8_t profile);
void Hall_SetPolarityMask(uint8_t mask);
void Hall_SetStateOffset(int8_t offset);
void Hall_GetConfig(HallConfig *cfg);

/*
 * Core function: read hall sensors, resolve to commutation state.
 * Returns: 0..5 = valid state, 255 = invalid/hall error.
 * Call from ISR context. Uses current tick timestamp.
 */
uint8_t Hall_ResolveState(uint32_t nowUs);

/*
 * Get the resolved drive state (after applying direction).
 * Diagnostic only — not used by ISR commutation (Comm_ApplyStep handles direction).
 * Returns 0..5 or 255.
 */
uint8_t Hall_GetDriveState(void);

/* Set motor direction: forward=1 (driveDirection=0, state as-is), forward=0 (driveDirection=1, state+3) */
void Hall_SetDirection(uint8_t forward);

/* Take a diagnostic snapshot for CLI printing (non-ISR safe) */
void Hall_GetSnapshot(HallSnapshot *snap);

/* Read raw hall GPIO (for calibration / debug) */
uint8_t Hall_ReadRaw(void);

#ifdef __cplusplus
}
#endif

#endif /* HALL_H */
