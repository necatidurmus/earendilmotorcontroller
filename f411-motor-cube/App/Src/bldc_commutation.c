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
    if (state > 5U && state != 255U) return false;
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
