#ifndef TELEMETRY_BRIDGE_H
#define TELEMETRY_BRIDGE_H

#include <stdbool.h>

/* ── Telemetry forwarding state ──────────────────────────────────────────────
 *  Controls whether motor UART telemetry lines are forwarded to the
 *  terminal (Logger_Log).  When disabled ("bridge off"), the RX
 *  processing chain (SafetyManager_NotifyRx, ACK parsing, link-loss
 *  detection) remains fully active — only the log output is suppressed.
 *
 *  Default state: enabled (ON). */
void  TelemetryBridge_Init(void);
void  TelemetryBridge_SetEnabled(bool enabled);
bool  TelemetryBridge_IsEnabled(void);

#endif /* TELEMETRY_BRIDGE_H */
