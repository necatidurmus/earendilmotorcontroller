/* ============================================================
 * App/Src/uart_protocol.c
 *
 * Non-blocking UART command parser.
 *
 *   RX path: DMA1 Stream5 circular mode.  The ISR drains the DMA
 *   buffer into a software ring; UartProtocol_Pump() feeds bytes
 *   into a line-builder and commits complete lines to a command
 *   queue.
 *
 *   TX path: DMA1 Stream6 normal mode.  UartProtocol_Print()
 *   writes bytes into a TX ring buffer.  When the DMA is idle,
 *   the next chunk is kicked off.  A DMA-TC callback starts the
 *   following chunk if more data is pending.  This replaces the
 *   old polling-TX loop and eliminates the Hall-edge-miss risk
 *   at high telemetry rates (ISSUE-013).
 * ============================================================ */

#include "uart_protocol.h"
#include "usart.h"
#include "main.h"
#include "app_config.h"
#include "app_main.h"   /* App_Usart2RxIsr forward declaration */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* DMA RX buffer — halfword buffer that the DMA fills continuously.
 * ISSUE-027: was 32, which lost any burst >= 32 bytes (the NDTR
 * reload made s_dma_last_pos == pos == 0 so the drain loop copied
 * nothing).  128 + half/complete callbacks lets us drain mid-burst
 * even without an idle gap, covering UART_LINE_MAX (64) comfortably. */
#define DMA_RX_BUF_LEN    128U
static uint8_t  s_dma_rx[DMA_RX_BUF_LEN];
static volatile uint16_t s_dma_last_pos = 0U;   /* last seen DMA index */

static volatile char     s_ring[RX_RING_LEN];
static volatile uint16_t s_ring_head  = 0U;
static volatile uint16_t s_ring_tail  = 0U;

/* ---- TX ring buffer + DMA (ISSUE-013) ----------------------------
 * TX_RING_LEN must be a power of two for fast modular arithmetic.
 * The DMA transfers one contiguous chunk at a time from tx_ring to
 * USART2->DR.  s_tx_head is advanced by UartProtocol_Print(); the
 * DMA hardware advances s_tx_dma_pos as bytes go out.  When the DMA
 * TC interrupt fires, the callback starts the next chunk if more
 * data is pending.
 * ---------------------------------------------------------------- */
#define TX_RING_LEN      512U
static volatile uint8_t  s_tx_ring[TX_RING_LEN];
static volatile uint16_t s_tx_head      = 0U;   /* producer writes here */
static volatile uint16_t s_tx_dma_start = 0U;   /* start of active DMA chunk */
static volatile uint16_t s_tx_dma_end   = 0U;   /* end of active DMA chunk */
static volatile bool     s_tx_dma_busy  = false;

typedef struct {
    char     line[UART_LINE_MAX];
    uint8_t  index;
    uint32_t last_input_ms;
} LineBuilder;

static LineBuilder s_line_uart;

static char     s_queue[CMD_QUEUE_LEN][UART_LINE_MAX];
static uint8_t  s_q_head = 0U;
static uint8_t  s_q_tail = 0U;
static UartSource s_q_src[CMD_QUEUE_LEN];

static UartSource s_reply_src = UART_SRC_UART;
static uint32_t   s_last_byte_ms = 0U;
static uint32_t   s_tx_drop_count = 0U;   /* ISSUE-041 */

static bool ring_push(char c)
{
    uint16_t next = (uint16_t)((s_ring_head + 1U) % RX_RING_LEN);
    if (next == s_ring_tail) return false;
    s_ring[s_ring_head] = c;
    s_ring_head = next;
    return true;
}

static bool ring_pop(char *out)
{
    if (s_ring_head == s_ring_tail) return false;
    *out = s_ring[s_ring_tail];
    s_ring_tail = (uint16_t)((s_ring_tail + 1U) % RX_RING_LEN);
    return true;
}

/* ---- TX ring helpers -------------------------------------------- */

static uint16_t tx_ring_free(void)
{
    /* Distance between head and DMA start, minus 1 for the sentinel. */
    uint16_t used = (uint16_t)((s_tx_head - s_tx_dma_start) & (TX_RING_LEN - 1U));
    return (uint16_t)(TX_RING_LEN - 1U - used);
}

static void tx_ring_write(const char *s, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        s_tx_ring[s_tx_head] = (uint8_t)s[i];
        s_tx_head = (uint16_t)((s_tx_head + 1U) & (TX_RING_LEN - 1U));
    }
}

/* Start a DMA transfer of the next contiguous chunk in the ring.
 * Must be called with interrupts disabled or from the same context
 * that modifies s_tx_dma_start / s_tx_head. */
static void tx_start_dma(void)
{
    if (s_tx_dma_busy) return;
    if (s_tx_dma_start == s_tx_head) return;  /* nothing to send */

    /* Compute contiguous length (may wrap). */
    uint16_t end = s_tx_head;
    uint16_t len;
    if (end > s_tx_dma_start) {
        len = (uint16_t)(end - s_tx_dma_start);
    } else {
        len = (uint16_t)(TX_RING_LEN - s_tx_dma_start);
    }

    s_tx_dma_end  = (uint16_t)((s_tx_dma_start + len) & (TX_RING_LEN - 1U));
    s_tx_dma_busy = true;

    if (HAL_UART_Transmit_DMA(&huart2, (uint8_t *)&s_tx_ring[s_tx_dma_start], len) != HAL_OK) {
        /* DMA or UART busy — reset flag so the next Print() retries. */
        s_tx_dma_busy = false;
    }
}

/* Called from the DMA1_Stream6_IRQHandler via HAL_DMA_IRQHandler when
 * the DMA transfer completes.  This is the TC callback. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;

    s_tx_dma_start = s_tx_dma_end;
    s_tx_dma_busy  = false;

    /* Kick the next chunk if more data is pending. */
    tx_start_dma();
}

/* ISSUE-027: Half-transfer and transfer-complete callbacks for the
 * DMA RX stream.  The DMA fills s_dma_rx in circular mode; without
 * these callbacks the buffer was only drained on the USART IDLE
 * interrupt, so a burst longer than the DMA buffer (was 32) would
 * wrap and overwrite unread bytes.  Draining at the half and full
 * marks guarantees we never lose data for bursts up to
 * DMA_RX_BUF_LEN/2 between drain points. */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;
    App_Usart2RxIsr(0);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;
    App_Usart2RxIsr(0);
}

static void queue_push(const char *line, UartSource src)
{
    uint8_t next = (uint8_t)((s_q_head + 1U) % CMD_QUEUE_LEN);
    if (next == s_q_tail) return;  /* drop on overflow */
    strncpy(s_queue[s_q_head], line, UART_LINE_MAX - 1U);
    s_queue[s_q_head][UART_LINE_MAX - 1U] = '\0';
    s_q_src[s_q_head] = src;
    s_q_head = next;
}

static void line_builder_reset(LineBuilder *lb) { lb->index = 0U; lb->line[0] = '\0'; }

static void line_builder_push(LineBuilder *lb, char c, uint32_t nowMs)
{
    lb->last_input_ms = nowMs;
    if (c == '\r' || c == '\n') {
        if (lb->index > 0U) {
            lb->line[lb->index] = '\0';
            queue_push(lb->line, s_reply_src);
            line_builder_reset(lb);
        }
        return;
    }
    if (lb->index < (UART_LINE_MAX - 1U)) {
        lb->line[lb->index++] = c;
    } else {
        line_builder_reset(lb);
    }
}

void UartProtocol_Init(void)
{
    /* Start DMA1 Stream5 in circular mode and enable RXNE/IDLE
     * interrupts.  HAL_UART_Receive_DMA handles all the bookkeeping. */
    if (HAL_UART_Receive_DMA(&huart2, s_dma_rx, DMA_RX_BUF_LEN) != HAL_OK) {
        Error_Handler();
    }
    /* Enable IDLE line interrupt.  UE bit + IDLEIE.
     * Explicitly disable RXNEIE — DMA handles byte transfer; reading DR
     * in an RXNE ISR would steal bytes from the DMA stream. */
    CLEAR_BIT(huart2.Instance->CR1, USART_CR1_RXNEIE);
    SET_BIT(huart2.Instance->CR1, USART_CR1_IDLEIE);

    s_dma_last_pos = 0U;
    s_ring_head = s_ring_tail = 0U;
    s_q_head    = s_q_tail    = 0U;
    line_builder_reset(&s_line_uart);
    s_reply_src = UART_SRC_UART;
    s_last_byte_ms = 0U;

    /* TX ring buffer — DMA transfer started on first print. */
    s_tx_head      = 0U;
    s_tx_dma_start = 0U;
    s_tx_dma_end   = 0U;
    s_tx_dma_busy  = false;
}

/* Called from the USART2 ISR.  Drains the DMA buffer into our own
 * ring buffer.  Must be short and non-blocking. */
void App_Usart2RxIsr(uint16_t bytes)
{
    (void)bytes;
    /* Compute the current DMA write position.  When NDTR rolls over
     * the count goes from 0..N; the producer position is (N - NDTR). */
    uint16_t pos = DMA_RX_BUF_LEN - (uint16_t)__HAL_DMA_GET_COUNTER(huart2.hdmarx);
    while (s_dma_last_pos != pos) {
        char c = (char)s_dma_rx[s_dma_last_pos];
        if (!ring_push(c)) {
            /* overflow — drop */
        }
        s_last_byte_ms = HAL_GetTick();
        s_dma_last_pos = (uint16_t)((s_dma_last_pos + 1U) % DMA_RX_BUF_LEN);
    }
}

void UartProtocol_Pump(void)
{
    uint32_t now = HAL_GetTick();

    /* Drain ring -> line builder.  Budget so we never hog the CPU. */
    char c;
    uint8_t budget = 64U;
    while (budget-- && ring_pop(&c)) {
        line_builder_push(&s_line_uart, c, now);
    }

    /* Idle timeout — if a partial line has been sitting, commit it. */
    if (s_line_uart.index > 0U && (now - s_line_uart.last_input_ms) > 150U) {
        s_line_uart.line[s_line_uart.index] = '\0';
        queue_push(s_line_uart.line, s_reply_src);
        line_builder_reset(&s_line_uart);
    }
}

bool UartProtocol_PopLine(char *out, uint8_t maxLen, UartSource *srcOut)
{
    if (s_q_head == s_q_tail) return false;
    uint8_t len = (uint8_t)strlen(s_queue[s_q_tail]);
    if (len >= maxLen) len = (uint8_t)(maxLen - 1U);
    memcpy(out, s_queue[s_q_tail], len);
    out[len] = '\0';
    if (srcOut) *srcOut = s_q_src[s_q_tail];
    s_q_tail = (uint8_t)((s_q_tail + 1U) % CMD_QUEUE_LEN);
    return true;
}

void UartProtocol_Print(const char *s)
{
    if (s == NULL) return;

    uint16_t len = (uint16_t)strlen(s);
    if (len == 0U) return;

    /* Disable interrupts to protect the shared ring buffer state. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    /* Drop the entire message if the ring does not have enough room.
     * Partial writes would produce corrupted telemetry lines that
     * confuse the H7 and terminal.py parsers. */
    uint16_t free = tx_ring_free();
    if (len > free) {
        /* ISSUE-041: count the dropped message so telemetry can
         * report TX ring overflow instead of silently losing data. */
        s_tx_drop_count++;
        __set_PRIMASK(primask);
        return;
    }

    tx_ring_write(s, len);
    tx_start_dma();

    __set_PRIMASK(primask);
}

void UartProtocol_PrintNewline(void)
{
    UartProtocol_Print("\r\n");
}

void UartProtocol_PrintNum(int32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)v);
    UartProtocol_Print(buf);
}

void UartProtocol_PrintUnsigned(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    UartProtocol_Print(buf);
}

void UartProtocol_PrintFloat(float v, int decimals)
{
    /* ISSUE-H: newlib-nano does not support %f by default.
     * This function is retained for API compatibility but prints
     * a scaled integer instead.  Use scaled-integer printf directly
     * in application code (e.g. Kp_m=600 for Kp=0.600). */
    (void)decimals;
    char buf[24];
    int32_t scaled = (int32_t)(v * 1000.0f);
    snprintf(buf, sizeof(buf), "%ld", (long)scaled);
    UartProtocol_Print(buf);
}

void UartProtocol_Printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) UartProtocol_Print(buf);
}

void UartProtocol_SetReplySource(UartSource src) { s_reply_src = src; }

bool UartProtocol_HasRecentActivity(uint32_t nowMs, uint32_t windowMs)
{
    if (s_last_byte_ms == 0U) return false;
    return (nowMs - s_last_byte_ms) <= windowMs;
}

uint32_t UartProtocol_GetTxDropCount(void)        { return s_tx_drop_count; }
void UartProtocol_ResetTxDropCount(void)         { s_tx_drop_count = 0U; }
