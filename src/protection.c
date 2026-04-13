/*
 * protection.c — Current sensing and overcurrent protection
 *
 * Architecture:
 *   - ADC sampling runs at decimated rate (every Nth ISR tick)
 *   - EMA filter smooths the raw ADC reading
 *   - Offset is calibrated at startup with outputs off
 *   - Delta = filtered - offset (how far above zero-current baseline)
 *   - Soft limit: proportional duty backoff when delta exceeds threshold
 *   - Hard limit: N consecutive over-threshold readings latch a fault
 *   - Fault latch: outputs off, mode stopped, requires explicit clear
 *   - Duty slew: limits rate of duty change per tick
 *
 * IMPORTANT: "estimated amps" is DISPLAY ONLY. Protection operates on
 * raw ADC delta counts. Do not use estimated amps for safety decisions
 * because the INA181 gain is uncertain.
 */

#include "protection.h"
#include "board_io.h"
#include "motor_config.h"

#include <string.h>

/* Config */
static ProtectionConfig protCfg = {
    .softLimitAdc = CURRENT_SOFT_LIMIT,
    .hardLimitAdc = CURRENT_HARD_LIMIT,
    .hardStrikesToTrip = HARD_LIMIT_STRIKES
};

/* ADC state */
static uint8_t  adcDecimator = 0;
static uint16_t currentRaw = 0;
static uint16_t voltageRaw = 0;
static float    currentFiltered = 0.0f;
static uint16_t currentOffset = 0;
static uint16_t currentDelta = 0;

/* Protection state */
static bool     softLimitActive = false;
static uint8_t  hardStrikes = 0;
static bool     faultLatched = false;
static char     faultReason[FAULT_REASON_MAX] = "none";

/* Display setting */
static float    inaGain = INA_GAIN_DEFAULT;

/* ====================================================================
 * Internal helpers
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
    softLimitActive = false;
    hardStrikes = 0;
    faultLatched = false;
    strncpy(faultReason, "none", sizeof(faultReason) - 1);
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
    if (currentFiltered < 1.0f) {
        currentFiltered = (float)currentRaw;
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
    strncpy(faultReason, "none", sizeof(faultReason) - 1);
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

void Prot_GetSnapshot(ProtectionSnapshot *snap) {
    snap->currentRaw = currentRaw;
    snap->currentFiltered = (uint16_t)currentFiltered;
    snap->currentOffset = currentOffset;
    snap->currentDelta = currentDelta;
    snap->voltageRaw = voltageRaw;
    snap->estimatedAmps = Prot_GetEstimatedAmps();
    snap->softLimitActive = softLimitActive;
    snap->hardStrikes = hardStrikes;
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
