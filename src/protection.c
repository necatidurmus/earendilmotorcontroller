/*
 * protection.c — Akım ölçümü ve aşırı akım koruması
 *
 * Mimari:
 *   - ADC örnekleme seyreltilmiş hızda çalışır (her N. ISR tick'inde)
 *   - EMA filtresi ham ADC okumasını yumuşatır
 *   - Ofset başlangıçta çıkışlar kapalıyken kalibre edilir
 *   - Delta = filtreli - ofset (sıfır akım tabanından ne kadar uzakta)
 *   - Yumuşak limit: delta eşiği aşınca orantılı duty geri çekme
 *   - Sert limit: N ardışık eşik aşımı fault latch'ler
 *   - Fault latch: çıkışlar kapalı, mod durdu, açıkça temizlenmeli
 *   - Duty slew: tick başına duty değişim hızını sınırlar
 *
 * ÖNEMLİ: "estimated amps" SADECE GÖSTERİMDİR. Koruma ham ADC delta
 * sayıları üzerinden çalışır. INA181 kazancı belirsiz olduğu için
 * estimated amps güvenlik kararlarında kullanılmamalıdır.
 */

#include "protection.h"
#include "board_io.h"
#include "motor_config.h"

#include <string.h>

/* Konfigürasyon */
static ProtectionConfig protCfg = {
    .softLimitAdc = CURRENT_SOFT_LIMIT,
    .hardLimitAdc = CURRENT_HARD_LIMIT,
    .hardStrikesToTrip = HARD_LIMIT_STRIKES
};

static UndervoltageConfig uvCfg = {
    .enabled = (UNDERVOLTAGE_PROTECTION_ENABLE != 0U),
    .limitMv = UNDERVOLTAGE_LIMIT_MV,
    .hysteresisMv = UNDERVOLTAGE_HYSTERESIS_MV,
    .strikesToTrip = UNDERVOLTAGE_STRIKES
};

/* ADC durumu */
static volatile uint8_t  adcDecimator = 0;
static volatile uint16_t currentRaw = 0;
static volatile uint16_t voltageRaw = 0;
static volatile float    currentFiltered = 0.0f;
static volatile uint16_t currentOffset = 0;
static volatile uint16_t currentDelta = 0;
static volatile bool     filterInitialized = false;

/* Koruma durumu */
static volatile bool     softLimitActive = false;
static volatile uint8_t  hardStrikes = 0;
static volatile uint8_t  undervoltageStrikes = 0;
static volatile bool     faultLatched = false;
static char              faultReason[FAULT_REASON_MAX] = "none";

/* Gösterim ayarı */
static float    inaGain = INA_GAIN_DEFAULT;

/* ====================================================================
 * İç yardımcı fonksiyonlar
 * ==================================================================== */

static uint16_t clampU16(int32_t val) {
    if (val < 0) return 0;
    if (val > 4095) return 4095;
    return (uint16_t)val;
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void Prot_Init(const ProtectionConfig *cfg) {
    protCfg = *cfg;
    adcDecimator = 0;
    currentRaw = 0;
    voltageRaw = 0;
    currentFiltered = 0.0f;
    currentOffset = 0;
    currentDelta = 0;
    filterInitialized = false;
    softLimitActive = false;
    hardStrikes = 0;
    undervoltageStrikes = 0;
    faultLatched = false;
    strncpy(faultReason, "none", sizeof(faultReason) - 1);

    uvCfg.enabled = (UNDERVOLTAGE_PROTECTION_ENABLE != 0U);
    uvCfg.limitMv = UNDERVOLTAGE_LIMIT_MV;
    uvCfg.hysteresisMv = UNDERVOLTAGE_HYSTERESIS_MV;
    uvCfg.strikesToTrip = UNDERVOLTAGE_STRIKES;
}

void Prot_SampleTick(void) {
    ++adcDecimator;
    if (adcDecimator < ADC_DECIMATION) {
        return;
    }
    adcDecimator = 0;

    currentRaw = BoardIO_ReadADC(ISENSE_ADC_CHANNEL);
    voltageRaw = BoardIO_ReadADC(VSENSE_ADC_CHANNEL);

    /* EMA filter */
    if (!filterInitialized) {
        currentFiltered = (float)currentRaw;
        filterInitialized = true;
    } else {
        currentFiltered += ((float)currentRaw - currentFiltered) * CURRENT_FILTER_ALPHA;
    }

    /* Compute delta from zero-current offset */
    int32_t delta = (int32_t)currentFiltered - (int32_t)currentOffset;
    currentDelta = clampU16(delta);
}

uint16_t Prot_ApplySoftLimit(uint16_t requestedDuty) {
    softLimitActive = false;

    if (requestedDuty == 0 || faultLatched) {
        return 0;
    }

    if (currentDelta <= protCfg.softLimitAdc) {
        return requestedDuty;
    }

    softLimitActive = true;

    uint16_t over = currentDelta - protCfg.softLimitAdc;
    uint16_t backoff = SOFT_BACKOFF_MIN + (over / SOFT_BACKOFF_DIVISOR);
    if (backoff > SOFT_BACKOFF_MAX) {
        backoff = SOFT_BACKOFF_MAX;
    }

    if (requestedDuty > backoff) {
        return requestedDuty - backoff;
    }
    return 0;
}

bool Prot_CheckHardLimit(void) {
    if (faultLatched) {
        return true;
    }

#if UNDERVOLTAGE_PROTECTION_ENABLE
    /* Bus gerilimi limitin altındayken ISENSE zinciri geçersiz olabilir.
     * Bu durumda hard strike biriktirme.
     */
    const uint16_t uvLimitAdc = VSENSE_MV_TO_ADC(uvCfg.limitMv);
    if (voltageRaw <= uvLimitAdc) {
        hardStrikes = 0;
        return false;
    }
#endif

    if (currentDelta >= protCfg.hardLimitAdc) {
        ++hardStrikes;
        if (hardStrikes >= protCfg.hardStrikesToTrip) {
            Prot_LatchFault("hard overcurrent");
            return true;
        }
    } else {
        hardStrikes = 0;
    }

    return false;
}

bool Prot_CheckUndervoltage(void) {
    if (faultLatched) {
        return true;
    }

#if UNDERVOLTAGE_PROTECTION_ENABLE
    if (!uvCfg.enabled) {
        undervoltageStrikes = 0;
        return false;
    }

    const uint16_t uvLimitAdc = VSENSE_MV_TO_ADC(uvCfg.limitMv);
    const uint16_t uvReleaseAdc = VSENSE_MV_TO_ADC(uvCfg.limitMv + uvCfg.hysteresisMv);

    if (voltageRaw <= uvLimitAdc) {
        if (undervoltageStrikes < 255U) {
            ++undervoltageStrikes;
        }

        if (undervoltageStrikes >= uvCfg.strikesToTrip) {
            Prot_LatchFault("undervoltage");
            return true;
        }
    } else if (voltageRaw >= uvReleaseAdc) {
        undervoltageStrikes = 0;
    }
#else
    undervoltageStrikes = 0;
#endif

    return false;
}

uint16_t Prot_SlewDuty(uint16_t current, uint16_t target) {
    if (current < target) {
        uint32_t next = (uint32_t)current + DUTY_RAMP_UP_STEP;
        return (next > target) ? target : (uint16_t)next;
    }
    if (current > target) {
        if ((uint32_t)target + DUTY_RAMP_DOWN_STEP < current) {
            return current - DUTY_RAMP_DOWN_STEP;
        }
        return target;
    }
    return current;
}

void Prot_CalibrateOffset(void) {
    BoardIO_AllOff();
    HAL_Delay(5);  /* let signals settle */

    uint32_t sum = 0;
    for (uint16_t i = 0; i < ADC_CALIBRATION_SAMPLES; ++i) {
        sum += BoardIO_ReadADC(ISENSE_ADC_CHANNEL);
        BoardIO_DelayUs(150);
    }

    currentOffset = (uint16_t)(sum / ADC_CALIBRATION_SAMPLES);
    currentFiltered = (float)currentOffset;
    currentDelta = 0;
    filterInitialized = true;
}

void Prot_LatchFault(const char *reason) {
    faultLatched = true;
    BoardIO_AllOff();
    strncpy(faultReason, reason, sizeof(faultReason) - 1);
    faultReason[sizeof(faultReason) - 1] = '\0';
}

void Prot_ClearFault(void) {
    faultLatched = false;
    hardStrikes = 0;
    undervoltageStrikes = 0;
    strncpy(faultReason, "none", sizeof(faultReason) - 1);
    faultReason[sizeof(faultReason) - 1] = '\0';
}

bool Prot_IsFaulted(void) {
    return faultLatched;
}

const char* Prot_GetFaultReason(void) {
    return faultReason;
}

void Prot_SetLimits(uint16_t soft, uint16_t hard) {
    protCfg.softLimitAdc = soft;
    protCfg.hardLimitAdc = hard;
}

void Prot_GetConfig(ProtectionConfig *cfg) {
    *cfg = protCfg;
}

void Prot_SetUndervoltageConfig(const UndervoltageConfig *cfg) {
    if (!cfg) {
        return;
    }

    uvCfg.enabled = cfg->enabled;

    if (cfg->limitMv >= 1000U && cfg->limitMv <= 60000U) {
        uvCfg.limitMv = cfg->limitMv;
    }

    if (cfg->hysteresisMv <= 5000U) {
        uvCfg.hysteresisMv = cfg->hysteresisMv;
    }

    if (cfg->strikesToTrip >= 1U && cfg->strikesToTrip <= 50U) {
        uvCfg.strikesToTrip = cfg->strikesToTrip;
    }
}

void Prot_GetUndervoltageConfig(UndervoltageConfig *cfg) {
    if (!cfg) {
        return;
    }
    *cfg = uvCfg;
}

void Prot_GetSnapshot(ProtectionSnapshot *snap) {
    /* ISR tutarlılığı: kısa süreli interrupt disable ile atomik kopyalama */
    __disable_irq();
    snap->currentRaw = currentRaw;
    snap->currentFiltered = (uint16_t)currentFiltered;
    snap->currentOffset = currentOffset;
    snap->currentDelta = currentDelta;
    snap->voltageRaw = voltageRaw;
    snap->softLimitActive = softLimitActive;
    snap->hardStrikes = hardStrikes;
    snap->undervoltageStrikes = undervoltageStrikes;
    __enable_irq();
    /* Hesaplama fonksiyonları interrupt dışında çağrılır */
    snap->estimatedVolts = Prot_GetEstimatedVolts();
    snap->estimatedAmps = Prot_GetEstimatedAmps();
}

void Prot_SetInaGain(float gain) {
    if (gain >= 1.0f && gain <= 1000.0f) {
        inaGain = gain;
    }
}

float Prot_GetEstimatedAmps(void) {
    float senseVoltage = ((float)currentDelta * ADC_VREF) / ADC_MAX_COUNTS;
    return senseVoltage / (inaGain * SHUNT_OHMS);
}

float Prot_GetEstimatedVolts(void) {
    return VSENSE_ADC_TO_V(voltageRaw);
}
