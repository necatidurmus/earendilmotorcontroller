/* ============================================================
 * App/Src/bldc_commutation.c
 * Hall-raw -> electrical sector mapping. Pure logic, no I/O.
 * ============================================================ */

#include "bldc_commutation.h"
#include "app_config.h"

#include <string.h>

static uint8_t s_hall_to_state[8] = {
    DEFAULT_HALL_MAP_0, DEFAULT_HALL_MAP_1, DEFAULT_HALL_MAP_2, DEFAULT_HALL_MAP_3,
    DEFAULT_HALL_MAP_4, DEFAULT_HALL_MAP_5, DEFAULT_HALL_MAP_6, DEFAULT_HALL_MAP_7
};

void Commutation_LoadDefaultMap(void)
{
    s_hall_to_state[0] = DEFAULT_HALL_MAP_0;
    s_hall_to_state[1] = DEFAULT_HALL_MAP_1;
    s_hall_to_state[2] = DEFAULT_HALL_MAP_2;
    s_hall_to_state[3] = DEFAULT_HALL_MAP_3;
    s_hall_to_state[4] = DEFAULT_HALL_MAP_4;
    s_hall_to_state[5] = DEFAULT_HALL_MAP_5;
    s_hall_to_state[6] = DEFAULT_HALL_MAP_6;
    s_hall_to_state[7] = DEFAULT_HALL_MAP_7;
}

void Commutation_GetMap(uint8_t out[8])
{
    memcpy(out, s_hall_to_state, sizeof(s_hall_to_state));
}

bool Commutation_SetMapEntry(uint8_t hallCode, uint8_t state)
{
    if (hallCode > 7U) return false;
    /* State must be 0..5 (valid sector) or 255 (invalid) */
    if (state > 5U && state != 255U) return false;
    /* raw 0 (0b000) and raw 7 (0b111) are invalid Hall codes —
     * they must always map to 255 (invalid).  Allowing a valid
     * sector here would let the motor drive on an impossible Hall
     * state. */
    if ((hallCode == 0U || hallCode == 7U) && state != 255U) return false;
    s_hall_to_state[hallCode] = state;
    return true;
}

uint8_t Commutation_HallToState(uint8_t hallCode)
{
    if (hallCode > 7U) return HALL_STATE_INVALID;
    return s_hall_to_state[hallCode];
}

bool Commutation_IsValidState(uint8_t state)
{
    return state <= 5U;
}

bool Commutation_IsTransitionValid(uint8_t prev, uint8_t next)
{
    if (!Commutation_IsValidState(prev) || !Commutation_IsValidState(next)) {
        return true;   /* cannot judge without a baseline */
    }
    uint8_t delta = (uint8_t)((next + 6U - prev) % 6U);
    return (delta == 1U || delta == 5U);
}

void Commutation_GetDrivePhases(uint8_t sector, int8_t direction,
                                uint8_t *high_phase, uint8_t *low_phase)
{
    if (high_phase == NULL || low_phase == NULL) return;
    if (sector > 5U || direction == 0) {
        *high_phase = 0xFFU;
        *low_phase  = 0xFFU;
        return;
    }

    /* MUST match motor_driver.c s_drive_table init (fwd_high/fwd_low).
     * If either table changes, the other MUST change identically. */
    static const uint8_t fwd_high[6] = {1, 2, 2, 0, 0, 1};
    static const uint8_t fwd_low [6] = {0, 0, 1, 1, 2, 2};

    if (direction > 0) {
        *high_phase = fwd_high[sector];
        *low_phase  = fwd_low[sector];
    } else {
        uint8_t r = (uint8_t)((sector + 3U) % 6U);
        *high_phase = fwd_high[r];
        *low_phase  = fwd_low[r];
    }
}

/* ---- Hall map validation ---- */

bool Commutation_ValidateHallMapVerbose(const uint8_t map[8],
                                        char *reason, size_t reason_len)
{
    if (map == NULL) {
        if (reason && reason_len > 0U) {
            strncpy(reason, "null_map", reason_len);
            reason[reason_len - 1] = '\0';
        }
        return false;
    }

    /* raw 0 must be invalid (255) */
    if (map[0] != 255U) {
        if (reason && reason_len > 0U) {
            strncpy(reason, "raw0_not_invalid", reason_len);
            reason[reason_len - 1] = '\0';
        }
        return false;
    }

    /* raw 7 must be invalid (255) */
    if (map[7] != 255U) {
        if (reason && reason_len > 0U) {
            strncpy(reason, "raw7_not_invalid", reason_len);
            reason[reason_len - 1] = '\0';
        }
        return false;
    }

    /* Check raw 1..6: each must be 0..5 */
    bool seen[6] = {false, false, false, false, false, false};
    uint8_t valid_count = 0U;

    for (uint8_t i = 1U; i <= 6U; i++) {
        uint8_t s = map[i];
        if (s > 5U) {
            /* Out of range (not 0..5 and not 255) */
            if (reason && reason_len > 0U) {
                strncpy(reason, "sector_out_of_range", reason_len);
                reason[reason_len - 1] = '\0';
            }
            return false;
        }
        if (seen[s]) {
            if (reason && reason_len > 0U) {
                strncpy(reason, "duplicate_sector", reason_len);
                reason[reason_len - 1] = '\0';
            }
            return false;
        }
        seen[s] = true;
        valid_count++;
    }

    /* All 6 sectors must appear exactly once */
    if (valid_count != 6U) {
        if (reason && reason_len > 0U) {
            strncpy(reason, "missing_sector", reason_len);
            reason[reason_len - 1] = '\0';
        }
        return false;
    }

    return true;
}

bool Commutation_ValidateHallMap(const uint8_t map[8])
{
    return Commutation_ValidateHallMapVerbose(map, NULL, 0U);
}

bool Commutation_IsCompleteHallMap(const uint8_t map[8])
{
    if (map == NULL) return false;
    for (uint8_t i = 1U; i <= 6U; i++) {
        if (map[i] > 5U) return false;
    }
    return true;
}

bool Commutation_HasDuplicateSectors(const uint8_t map[8])
{
    if (map == NULL) return false;
    bool seen[6] = {false, false, false, false, false, false};
    for (uint8_t i = 1U; i <= 6U; i++) {
        if (map[i] > 5U) continue;
        if (seen[map[i]]) return true;
        seen[map[i]] = true;
    }
    return false;
}

void Commutation_CopyMap(uint8_t dst[8], const uint8_t src[8])
{
    if (dst == NULL || src == NULL) return;
    memcpy(dst, src, 8U);
}

void Commutation_ApplyMap(const uint8_t map[8])
{
    if (map == NULL) return;
    memcpy(s_hall_to_state, map, 8U);
}
