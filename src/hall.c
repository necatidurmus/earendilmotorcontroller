/*
 * hall.c — TIM4 tabanli event-driven hall sensor isleme
 *
 * Mimari:
 *   - PB6/PB7/PB8 TIM4 Hall Sensor Interface (AF2) ile izlenir
 *   - Her hall gecisinde TIM4 capture IRQ tetiklenir (BOTHEDGE)
 *   - Counter her geciste sifirlanir (slave reset mode)
 *   - CCR1 onceki gecisten bu yana gecen sureyi verir (us, PSC=95)
 *   - hallProcessTransition() ISR'da state/timestamp gunceller
 *   - TIM3 kontrol ISR'i yalnizca son kabul edilen state'i tuketir
 *
 * GPIO IDR okuma:
 *   readHallRawDirect() AF modundayken bile GPIO IDR uzerinden
 *   pin seviyelerini okur. STM32'de IDR her zaman fiziksel pin
 *   durumunu yansitir — AF modundan bagimsiz. Bu, hall bit
 *   degerlerini almanin dogru ve tek yoludur.
 *
 * Hall_ReadRaw() / Hall_GetSnapshot():
 *   Debug ve CLI tani icin kullanilir, kontrol yolunda degil.
 */

#include "hall.h"
#include "motor_config.h"
#include "stm32f4xx_hal.h"

/* main.c'de tanimli kontrol tick sayaci (TIM3 ISR) */
extern volatile uint32_t g_isrTickCount;

/*
 * Hall -> komutasyon durumu donusum tablosu.
 * motor_config.h'de extern olarak bildirilir, burada tanimlanir.
 * Indeks = XOR-duzeltilmis ham hall (0..7), Deger = durum 0..5 veya 255=gecersiz
 */
const uint8_t HALL_TO_STATE_PROFILES[HALL_PROFILE_COUNT][8] = {
    {255, 0, 4, 5, 2, 1, 3, 255},  /* Profil 0: mevcut motor kablolamasi */
    {255, 0, 2, 1, 4, 5, 3, 255},  /* Profil 1: alternatif siralama */
    {255, 4, 0, 1, 2, 3, 5, 255},  /* Profil 2: alternatif siralama */
    {255, 2, 4, 3, 0, 1, 5, 255},  /* Profil 3: alternatif siralama */
};

/* Calisma zamani konfigurasyonu */
static volatile HallConfig hallCfg = {
    .profile = 0,
    .polarityMask = 0,
    .stateOffset = 0
};

/* Durum takibi */
static volatile uint8_t  lastAcceptedState = 255;
static volatile uint32_t lastAcceptedTransitionUs = 0;
static volatile uint32_t lastValidControlUs = 0;

static volatile uint8_t  lastRaw = 0;
static volatile uint8_t  lastCorrected = 0;
static volatile uint8_t  lastMapped = 255;

static volatile uint32_t lastTransitionUs = 0;
static volatile uint32_t lastSectorPeriodUs = 0;
static volatile uint8_t  hallStale = 1;

static volatile uint8_t  driveDirection = 0;  /* 0=ileri, 1=geri */
static volatile uint8_t  currentDriveState = 255;

/* TIM4 capture period'larini toplayarak olusturulan hall zaman tabani */
static volatile uint32_t hallEventTimeUs = 0;

/* ====================================================================
 * Ic yardimci fonksiyonlar
 * ==================================================================== */

static uint8_t wrapState(int16_t value) {
    int16_t v = value % 6;
    if (v < 0) {
        v += 6;
    }
    return (uint8_t)v;
}

/* Kontrol ISR zaman tabani: TIM3 12.5 kHz -> 80 us/tick */
static uint32_t hallControlNowUs(void) {
    uint32_t ticks = g_isrTickCount;
    return (uint32_t)((uint64_t)ticks * 80U);
}

/*
 * GPIO IDR uzerinden anlik hall pin durumlarini oku.
 * AF modundayken bile IDR fiziksel pin seviyesini yansitir.
 * TIM4 capture callback'inden ve debug/init'ten cagrilir.
 */
static uint8_t readHallRawDirect(void) {
    uint8_t hall = 0;

    if (HAL_GPIO_ReadPin(HALL_A_PORT, HALL_A_PIN) == GPIO_PIN_SET) {
        hall |= 0x01;
    }
    if (HAL_GPIO_ReadPin(HALL_B_PORT, HALL_B_PIN) == GPIO_PIN_SET) {
        hall |= 0x02;
    }
    if (HAL_GPIO_ReadPin(HALL_C_PORT, HALL_C_PIN) == GPIO_PIN_SET) {
        hall |= 0x04;
    }

    return hall & 0x07;
}

static uint8_t mapHallToState(uint8_t corrected) {
    uint8_t profile = hallCfg.profile;
    int8_t offset = hallCfg.stateOffset;
    const uint8_t *table;
    uint8_t base;

    if (profile >= HALL_PROFILE_COUNT) {
        profile = 0;
    }

    table = HALL_TO_STATE_PROFILES[profile];
    base = table[corrected & 0x07];

    if (base > 5) {
        return 255;
    }

    return wrapState((int16_t)base + (int16_t)offset);
}

static void hallProcessTransition(uint32_t transitionUs, uint32_t periodUs) {
    uint8_t raw = readHallRawDirect();
    uint8_t corrected = (uint8_t)((raw ^ hallCfg.polarityMask) & 0x07U);
    uint8_t mapped = mapHallToState(corrected);

    lastRaw = raw;
    lastCorrected = corrected;
    lastMapped = mapped;
    lastTransitionUs = transitionUs;
    lastSectorPeriodUs = periodUs;

    if (mapped <= 5) {
        uint32_t nowCtrlUs = hallControlNowUs();
        lastValidControlUs = nowCtrlUs;
        hallStale = 0;

        if (lastAcceptedState > 5) {
            /* Ilk gecerli state: hemen kabul et */
            lastAcceptedState = mapped;
            lastAcceptedTransitionUs = transitionUs;
            return;
        }

        /* Debounce: cok hizli gecisleri kabul etme */
        if (mapped != lastAcceptedState) {
            if ((uint32_t)(transitionUs - lastAcceptedTransitionUs) >= MIN_STATE_INTERVAL_US) {
                lastAcceptedState = mapped;
                lastAcceptedTransitionUs = transitionUs;
            }
        }
        return;
    }

    /* Gecersiz map event'i geldi, timeout penceresi icinde hold devam eder */
    hallStale = 1;
}

/* ====================================================================
 * TIM4 Hall callback'leri
 * ==================================================================== */

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != HALL_TIMER) {
        return;
    }

    /*
     * Hall sensor mode reset konfigurasyonunda CCR1, onceki gecisten bu yana
     * olculen sureyi verir (us, PSC=95).
     */
    uint32_t periodUs = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
    if (periodUs == 0U) {
        /* Ard arda cok yakin edge durumunda 0 gelebilir; zaman tabanini ilerlet */
        periodUs = 1U;
    }

    hallEventTimeUs += periodUs;
    hallProcessTransition(hallEventTimeUs, periodUs);
}

/* ====================================================================
 * Genel API
 * ==================================================================== */

void Hall_Init(const HallConfig *cfg) {
    uint8_t raw;
    uint8_t corrected;
    uint8_t mapped;

    __disable_irq();

    hallCfg.profile = (cfg->profile < HALL_PROFILE_COUNT) ? cfg->profile : 0;
    hallCfg.polarityMask = (uint8_t)(cfg->polarityMask & 0x07U);
    hallCfg.stateOffset = (cfg->stateOffset < -5) ? -5 : ((cfg->stateOffset > 5) ? 5 : cfg->stateOffset);

    lastAcceptedState = 255;
    lastAcceptedTransitionUs = 0;
    lastValidControlUs = 0;

    lastTransitionUs = 0;
    lastSectorPeriodUs = 0;
    hallEventTimeUs = 0;
    hallStale = 1;

    raw = readHallRawDirect();
    corrected = (uint8_t)((raw ^ hallCfg.polarityMask) & 0x07U);
    mapped = mapHallToState(corrected);

    lastRaw = raw;
    lastCorrected = corrected;
    lastMapped = mapped;
    currentDriveState = 255;

    __enable_irq();
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
    cfg->profile = hallCfg.profile;
    cfg->polarityMask = hallCfg.polarityMask;
    cfg->stateOffset = hallCfg.stateOffset;
}

/*
 * Yon ayari — yalnizca diagnostik goruntuleme icin.
 * Gercek komutasyon yonu Comm_ApplyStep()'e iletilen RunMode ile belirlenir.
 * Bu fonksiyon Hall_GetDriveState() / HallSnapshot.driveState degerini etkiler.
 */
void Hall_SetDirection(uint8_t forward) {
    driveDirection = forward ? 0 : 1;
}

/* Debug/CLI icin anlik ham hall degeri — kontrol yolunda kullanilmaz */
uint8_t Hall_ReadRaw(void) {
    return readHallRawDirect();
}

uint8_t Hall_ResolveState(uint32_t nowUs) {
    uint8_t accepted = lastAcceptedState;

    if (accepted <= 5) {
        uint32_t ageUs = (uint32_t)(nowUs - lastValidControlUs);

        if (ageUs <= INVALID_HALL_HOLD_US) {
            return accepted;
        }

        /* Timeout: son kabul edilen state'i gecersizle */
        lastAcceptedState = 255;
        hallStale = 1;
        return 255;
    }

    hallStale = 1;
    return 255;
}

uint8_t Hall_GetDriveState(void) {
    uint8_t accepted = lastAcceptedState;

    if (accepted > 5) {
        currentDriveState = 255;
    } else if (driveDirection == 0) {
        currentDriveState = accepted;
    } else {
        currentDriveState = wrapState((int16_t)accepted + 3);
    }

    return currentDriveState;
}

void Hall_GetSnapshot(HallSnapshot *snap) {
    __disable_irq();
    snap->raw = lastRaw;
    snap->corrected = lastCorrected;
    snap->mapped = lastMapped;
    snap->accepted = lastAcceptedState;
    snap->lastTransitionUs = lastTransitionUs;
    snap->sectorPeriodUs = lastSectorPeriodUs;
    snap->stale = hallStale;
    __enable_irq();

    Hall_GetDriveState();
    snap->driveState = currentDriveState;
}
