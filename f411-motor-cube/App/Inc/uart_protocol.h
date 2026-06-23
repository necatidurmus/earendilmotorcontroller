/* ============================================================
 * App/Inc/uart_protocol.h
 * Non-blocking UART command parser, ring buffer, line builder.
 * Public API used by the app layer.
 * ============================================================ */
#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UART_SRC_UART = 0,  /* main command port (PA2/PA3) */
    UART_SRC_USB  = 1   /* optional USB CDC; not used in Cube build */
} UartSource;

/* Initialise the ring buffer / parser. */
void UartProtocol_Init(void);

/* Drain any pending bytes from the UART (non-blocking) and feed them
 * into the ring buffer. Call from the main loop. */
void UartProtocol_Pump(void);

/* Pop at most one completed command line. Returns true if a line is
 * available in `out` (null-terminated). */
bool UartProtocol_PopLine(char *out, uint8_t maxLen, UartSource *srcOut);

/* Send a non-blocking reply. */
void UartProtocol_Print(const char *s);
void UartProtocol_Printf(const char *fmt, ...);
void UartProtocol_PrintNum(int32_t v);
void UartProtocol_PrintUnsigned(uint32_t v);
void UartProtocol_PrintFloat(float v, int decimals);
void UartProtocol_PrintNewline(void);

/* Set the source that subsequent replies are directed to. */
void UartProtocol_SetReplySource(UartSource src);

/* True if a character has been received in the last few ms.
 * Used by the host-disconnect watchdog. */
bool UartProtocol_HasRecentActivity(uint32_t nowMs, uint32_t windowMs);

/* ISSUE-041: TX ring drop counter.  UartProtocol_Print() drops a
 * whole message when the TX ring does not have enough free space.
 * This counter lets telemetry/debug report how many messages were
 * dropped so a TX bandwidth problem is visible.  Reset by clrerr. */
uint32_t UartProtocol_GetTxDropCount(void);
void UartProtocol_ResetTxDropCount(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */
