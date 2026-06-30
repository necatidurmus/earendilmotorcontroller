#include "telemetry_bridge.h"

/* ── Private state ─────────────────────────────────────────────────────────── */
static bool s_enabled = true;

/* ── Public functions ──────────────────────────────────────────────────────── */

void TelemetryBridge_Init(void)
{
    s_enabled = true;
}

void TelemetryBridge_SetEnabled(bool enabled)
{
    s_enabled = enabled;
}

bool TelemetryBridge_IsEnabled(void)
{
    return s_enabled;
}
