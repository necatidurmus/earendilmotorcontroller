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
uint32_t UartProtocol_GetCmdDropCount(void);
void UartProtocol_ResetCmdDropCount(void);

/* RX ring overflow counter.  Incremented when the RX ring buffer is
 * full and an incoming byte is dropped.  Motion-active RX overflow
 * should trigger a safety fault. */
uint32_t UartProtocol_GetRxDropCount(void);
void UartProtocol_ResetRxDropCount(void);

/* RX byte counter.  Incremented for every byte drained from the DMA
 * RX buffer into the software ring.  Monotonic (reset by clrerr). */
uint32_t UartProtocol_GetRxByteCount(void);
void UartProtocol_ResetRxByteCount(void);

/* Emergency preempt counter.  Incremented when an emergency command
 * (stop/estop/safe) displaces the oldest queued command because the
 * queue was full. */
uint32_t UartProtocol_GetEmergencyPreemptCount(void);
void UartProtocol_ResetEmergencyPreemptCount(void);

/* UART error counter.  Incremented when a UART error (ORE/FE/NE/PE)
 * aborts the DMA RX stream.  The stream is restarted automatically,
 * but a rising count indicates electrical/protocol problems on the
 * command line. */
uint32_t UartProtocol_GetUartErrorCount(void);
void UartProtocol_ResetUartErrorCount(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */
