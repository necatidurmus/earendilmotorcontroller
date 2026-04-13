/*
 * cli.h — Serial command-line interface for motor bring-up
 *
 * Uses USART2 (PA2=TX, PA3=RX) for all commands and telemetry.
 * USB CDC can be added later — UART is simpler to get running first.
 *
 * Commands:
 *   forward/f, backward/b, stop/s — motor direction
 *   pwm <0..255>                   — set duty
 *   status                         — full status dump
 *   hall                           — hall sensor snapshot
 *   current                        — current/voltage ADC snapshot
 *   hinv <0|1>                     — invert all hall bits
 *   hmask <0..7>                   — hall XOR mask
 *   offset <-5..5>                 — commutation state offset
 *   map <0..3>                     — hall mapping profile
 *   limits <soft> <hard>           — current limits (ADC counts)
 *   gain <20|50|100|200>           — INA gain for current display
 *   zeroi                          — recalibrate ISENSE offset
 *   clear                          — clear latched fault
 *   help/?                         — help text
 */

#ifndef CLI_H
#define CLI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize CLI (already handled by UART init, this sets up state) */
void CLI_Init(void);

/*
 * Service CLI — call from main loop (non-ISR).
 * Reads available characters, parses lines, dispatches commands.
 */
void CLI_Service(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
