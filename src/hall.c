/*
 * hall.c — Hall sensor processing implementation
 *
 * Reads PB6/PB7/PB8 with oversampling and majority vote.
 * Applies polarity mask, profile lookup, state offset.
 * Debounces state changes (min interval).
 * Holds last valid state during invalid hall periods (timeout).
 */

#include "hall.h"
#include "motor_config.h"
#include "stm32f4xx_hal.h"

/*
 * Hall → komütasyon durumu dönüşüm tablosu.
 * motor_config.h'de extern olarak bildirilir, burada tanımlanır.
 * Index = XOR-düzeltilmiş ham hall (0..7), Değer = durum 0..5 veya 255=geçersiz
 */
const uint8_t HALL_TO_STATE_PROFILES[HALL_PROFILE_COUNT][8] = {
    {255, 0, 4, 5, 2, 1, 3, 255},  /* Profil 0: mevcut motor kablolaması */
    {255, 0, 2, 1, 4, 5, 3, 255},  /* Profil 1: alternatif sıralama */
    {255, 4, 0, 1, 2, 3, 5, 255},  /* Profil 2: alternatif sıralama */
    {255, 2, 4, 3, 0, 1, 5, 255},  /* Profil 3: alternatif sıralama */
};

/* Runtime configuration */
static HallConfig hallCfg = {
    .profile = 0,
    .polarityMask = 0,
    .stateOffset = 0
};

/* State tracking */
static uint8_t  lastAcceptedState = 255;
static uint32_t lastAcceptedTimeUs = 0;
static uint32_t lastValidTimeUs = 0;

static uint8_t  lastRaw = 0;
static uint8_t  lastCorrected = 0;
static uint8_t  lastMapped = 255;

static uint8_t  driveDirection = 0;  /* 0=forward, 1=backward */
static uint8_t  currentDriveState = 255;

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

static uint8_t wrapState(int16_t value) {
    int16_t v = value % 6;
    if (v < 0) v += 6;
    return (uint8_t)v;
}

static uint8_t readHallRawMajority(void) {
    uint8_t aCount = 0, bCount = 0, cCount = 0;

    for (uint8_t i = 0; i < HALL_OVERSAMPLE; ++i) {
        aCount += (uint8_t)HAL_GPIO_ReadPin(HALL_A_PORT, HALL_A_PIN);
        bCount += (uint8_t)HAL_GPIO_ReadPin(HALL_B_PORT, HALL_B_PIN);
        cCount += (uint8_t)HAL_GPIO_ReadPin(HALL_C_PORT, HALL_C_PIN);
    }

    uint8_t hall = 0;
    if (aCount > (HALL_OVERSAMPLE / 2)) hall |= 0x01;
    if (bCount > (HALL_OVERSAMPLE / 2)) hall |= 0x02;
    if (cCount > (HALL_OVERSAMPLE / 2)) hall |= 0x04;

    return hall & 0x07;
}

static uint8_t mapHallToState(uint8_t corrected) {
    const uint8_t *table = HALL_TO_STATE_PROFILES[hallCfg.profile];
    uint8_t base = table[corrected & 0x07];
    if (base > 5) return 255;
    return wrapState((int16_t)base + (int16_t)hallCfg.stateOffset);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void Hall_Init(const HallConfig *cfg) {
    hallCfg = *cfg;
    lastAcceptedState = 255;
    lastAcceptedTimeUs = 0;
    lastValidTimeUs = 0;
    lastRaw = 0;
    lastCorrected = 0;
    lastMapped = 255;
    currentDriveState = 255;
}

void Hall_SetProfile(uint8_t profile) {
    if (profile < HALL_PROFILE_COUNT) {
        hallCfg.profile = profile;
    }
}

void Hall_SetPolarityMask(uint8_t mask) {
    hallCfg.polarityMask = mask & 0x07;
}

void Hall_SetStateOffset(int8_t offset) {
    if (offset >= -5 && offset <= 5) {
        hallCfg.stateOffset = offset;
    }
}

void Hall_GetConfig(HallConfig *cfg) {
    *cfg = hallCfg;
}

void Hall_SetDirection(uint8_t forward) {
    driveDirection = forward ? 0 : 1;
}

uint8_t Hall_ReadRaw(void) {
    return readHallRawMajority();
}

uint8_t Hall_ResolveState(uint32_t nowUs) {
    uint8_t raw = readHallRawMajority();
    uint8_t corrected = (raw ^ hallCfg.polarityMask) & 0x07;
    uint8_t mapped = mapHallToState(corrected);

    lastRaw = raw;
    lastCorrected = corrected;
    lastMapped = mapped;

    if (mapped <= 5) {
        /* Valid hall reading */
        lastValidTimeUs = nowUs;

        if (lastAcceptedState > 5) {
            /* First valid reading — accept immediately */
            lastAcceptedState = mapped;
            lastAcceptedTimeUs = nowUs;
            return lastAcceptedState;
        }

        /* Debounce: only accept new state if enough time has passed */
        if (mapped != lastAcceptedState) {
            if ((uint32_t)(nowUs - lastAcceptedTimeUs) >= MIN_STATE_INTERVAL_US) {
                lastAcceptedState = mapped;
                lastAcceptedTimeUs = nowUs;
            }
        }

        return lastAcceptedState;
    }

    /* Invalid hall reading — hold last valid state for timeout period */
    if (lastAcceptedState <= 5 &&
        (uint32_t)(nowUs - lastValidTimeUs) <= INVALID_HALL_HOLD_US) {
        return lastAcceptedState;
    }

    /* Timeout exceeded — mark as invalid */
    lastAcceptedState = 255;
    return 255;
}

uint8_t Hall_GetDriveState(void) {
    if (lastAcceptedState > 5) {
        currentDriveState = 255;
    } else if (driveDirection == 0) {
        currentDriveState = lastAcceptedState;
    } else {
        currentDriveState = wrapState((int16_t)lastAcceptedState + 3);
    }
    return currentDriveState;
}

void Hall_GetSnapshot(HallSnapshot *snap) {
    Hall_GetDriveState();
    snap->raw = lastRaw;
    snap->corrected = lastCorrected;
    snap->mapped = lastMapped;
    snap->accepted = lastAcceptedState;
    snap->driveState = currentDriveState;
}
