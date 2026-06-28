/* ============================================================
 * App/Inc/hall_sensor.h
 * Hall sensing, debounce, RPM measurement, fault detection.
 * ============================================================ */
#ifndef HALL_SENSOR_H
#define HALL_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HALL_FRESH = 0,
    HALL_STALE = 1
} HallFreshness;

typedef enum {
    HALL_FAULT_NONE = 0,
    HALL_FAULT_INVALID_PERSIST = 1,   /* 0b000/0b111 held > INVALID_HALL_STOP_US */
    HALL_FAULT_ILLEGAL_TRANSITION = 2 /* invalidTransitionCount > threshold */
} HallFault;

void HallSensor_Init(void);

/* Run the debounce state machine.  Cheap, must be called every
 * control tick (App_Loop).  Not gated by an EXTI pending flag —
 * it reads the Hall pins directly so a missed EXTI does not lose a
 * stable transition. */
void HallSensor_Update(void);

/* Returns the latest debounced Hall code (3-bit, 0..7). */
uint8_t HallSensor_GetStableRaw(void);

/* Returns the mapped electrical sector (0..5) or 255 if invalid. */
uint8_t HallSensor_GetMappedState(void);

/* Returns the last electrical sector that the motor actually drove.
 * Useful when a transient Hall glitch leaves the raw code invalid. */
uint8_t HallSensor_GetLastDrivenState(void);

/* Returns the timestamp of the most recent stable Hall transition. */
uint32_t HallSensor_GetLastTransitionUs(void);

/* Returns 0 if no recent Hall transitions; otherwise mechanical RPM. */
uint32_t HallSensor_CalculateRpm(void);

/* Returns the filtered RPM (low-pass alpha filter).
 * Does NOT apply the filter — just returns the cached value.
 * The filter is applied once per loop in HallSensor_Update(). */
float HallSensor_GetFilteredRpm(void);

/* Apply the low-pass filter to the current RPM.
 * Called from HallSensor_Update() once per loop. */
void HallSensor_UpdateFilteredRpm(void);

/* Returns the raw RPM (one period) without filtering. */
float HallSensor_GetRawRpm(void);

/* TRUE if at least one Hall edge has been observed since init. */
bool HallSensor_HasValidEdge(void);

/* Returns true if the currently debounced raw hall state is not 0 or 7. */
bool HallSensor_IsCurrentRawValid(void);

/* FRESH if a Hall edge was seen in the last HALL_RPM_TIMEOUT_US. */
HallFreshness HallSensor_GetFreshness(void);

/* Monotonic hall edge counter — never resets. Used by SpeedPI. */
uint32_t HallSensor_GetEdgeCounter(void);

/* Monotonic EXTI IRQ counter — increments on every Hall EXTI edge.
 * Used for diagnostics; the debounce state machine runs in
 * HallSensor_Update() from the main loop. */
uint32_t HallSensor_GetIrqCount(void);

/* Diagnostic counters for fault tracking. */
uint32_t HallSensor_GetInvalidRawCount(void);
uint32_t HallSensor_GetInvalidTransitionCount(void);
uint32_t HallSensor_GetValidTransitionCount(void);

/* Returns the current candidate (unstable) raw Hall code and how
 * many consecutive samples it has held.  Useful for debug. */
uint8_t HallSensor_GetCandidateRaw(void);
uint8_t HallSensor_GetCandidateCount(void);

/* Inspect latched Hall-side faults.  These are cleared by
 * HallSensor_Init() and re-evaluated every HallSensor_Update(). */
HallFault HallSensor_GetFault(void);
void HallSensor_ClearFault(void);

/* Called after a Hall map change (identify, map apply, mapreset).
 * Resets cached Hall state so the new map is evaluated fresh.
 * Motor MUST be stopped before calling this. */
void HallSensor_OnMapChanged(void);

#ifdef __cplusplus
}
#endif

#endif /* HALL_SENSOR_H */
