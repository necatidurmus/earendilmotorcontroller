/*
 * protection.h — Current sensing, overcurrent protection, fault management
 *
 * Responsibilities:
 *   - ADC sampling with decimation (reduces ISR overhead)
 *   - EMA low-pass filter on current ADC
 *   - Offset calibration (zero-current baseline)
 *   - Soft current limit (proportional duty backoff)
 *   - Hard overcurrent trip (latched fault)
 *   - Duty slew limiting
 *   - Fault latch with reason string
 *   - Bus voltage reading (raw, for telemetry)
 */

#ifndef PROTECTION_H
#define PROTECTION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection configuration — can be adjusted from CLI */
typedef struct {
    uint16_t softLimitAdc;      /* ADC delta counts for soft limit */
    uint16_t hardLimitAdc;      /* ADC delta counts for hard trip */
    uint8_t  hardStrikesToTrip; /* consecutive strikes before latch */
} ProtectionConfig;

/* Diagnostic snapshot for CLI */
typedef struct {
    uint16_t currentRaw;        /* latest raw ISENSE ADC reading */
    uint16_t currentFiltered;   /* EMA filtered value */
    uint16_t currentOffset;     /* calibrated zero-current baseline */
    uint16_t currentDelta;      /* filtered - offset (clamped >= 0) */
    uint16_t voltageRaw;        /* latest VSENSE ADC reading */
    float    estimatedAmps;     /* display-only current estimate */
    bool     softLimitActive;   /* true if soft limiter is reducing duty */
    uint8_t  hardStrikes;       /* current consecutive strike count */
} ProtectionSnapshot;

/* Fault state */
typedef struct {
    bool     latched;
    char     reason[48];
} FaultState;

/* Initialize protection module */
void Prot_Init(const ProtectionConfig *cfg);

/* Called from ISR every tick — samples ADC with decimation */
void Prot_SampleTick(void);

/*
 * Apply soft current limit to requested duty.
 * Returns the (possibly reduced) duty value.
 * Called from ISR.
 */
uint16_t Prot_ApplySoftLimit(uint16_t requestedDuty);

/*
 * Check hard overcurrent. Returns true if fault was latched.
 * Called from ISR.
 */
bool Prot_CheckHardLimit(void);

/*
 * Slew-limit duty from current to target.
 * Called from ISR.
 */
uint16_t Prot_SlewDuty(uint16_t current, uint16_t target);

/* Calibrate zero-current offset (blocking, outputs must be off) */
void Prot_CalibrateOffset(void);

/* Fault management */
void Prot_LatchFault(const char *reason);
void Prot_ClearFault(void);
bool Prot_IsFaulted(void);
const char* Prot_GetFaultReason(void);

/* Config updates from CLI */
void Prot_SetLimits(uint16_t soft, uint16_t hard);
void Prot_GetConfig(ProtectionConfig *cfg);

/* Get diagnostic snapshot */
void Prot_GetSnapshot(ProtectionSnapshot *snap);

/* INA gain setting for display conversion only */
void Prot_SetInaGain(float gain);
float Prot_GetEstimatedAmps(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTECTION_H */
