/* ============================================================
 * App/Inc/telemetry.h
 * Periodic, rate-limited, non-blocking telemetry output.
 * ============================================================ */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELEMETRY_COMPACT = 0,  /* default */
    TELEMETRY_DEBUG   = 1
} TelemetryMode;

void Telemetry_Init(void);
void Telemetry_Tick(uint32_t nowMs);
void Telemetry_SetMode(TelemetryMode mode);
TelemetryMode Telemetry_GetMode(void);
void Telemetry_SetIntervalMs(uint32_t ms);
uint32_t Telemetry_GetIntervalMs(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
