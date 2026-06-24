/* ============================================================
 * App/Src/hall_sensor.c
 * Hall sensing with debounce, transition tracking, RPM
 * calculation, and invalid-Hall / illegal-transition fault
 * detection.
 *
 * Architecture (ISSUE-007, ISSUE-019, ISSUE-035):
 *   - EXTI on PB6/PB7/PB8 fires on every rising/falling edge.  The
 *     ISR captures a raw snapshot (GPIOB->IDR), a TIM2 timestamp,
 *     and increments a sequence counter.  It does NOT run the
 *     debounce state machine — that stays single-writer in the
 *     main loop.
 *   - HallSensor_Update() is called every App_Loop iteration.  It
 *     reads the Hall pins directly and runs the debounce state
 *     machine regardless of whether an EXTI fired.  This fixes the
 *     debounce bug (ISSUE-035) where a stable transition was
 *     missed because the second sample waited for a new EXTI that
 *     never came.
 *   - All ISR-shared variables are volatile.
 *
 * No current sense / current limiting is used here.  Faults are
 * based only on Hall pattern, transition validity, and timing.
 * ============================================================ */

#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "app_config.h"
#include "tim.h"          /* App_GetMicros() (TIM2 1 MHz free-running) */
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <string.h>

/* ---- ISR-shared snapshot (volatile, written only from EXTI ISR) ----
 * s_irq_seq  — bumped on every Hall EXTI edge (monotonic)
 * s_irq_count — same, kept as a separate diagnostic counter
 * s_irq_raw  — raw Hall code sampled at the ISR edge
 * s_irq_us   — TIM2->CNT captured at the ISR edge (1 MHz)
 *
 * The main loop reads s_irq_seq to detect that a new edge happened
 * and s_irq_us for the RPM period.  The debounce state machine
 * itself reads the live GPIO so it is not gated by the ISR. */
static volatile uint32_t s_irq_seq   = 0U;
static volatile uint32_t s_irq_count = 0U;
static volatile uint8_t  s_irq_raw   = 0U;
static volatile uint32_t s_irq_us    = 0U;

/* Atomic snapshot of the ISR-shared Hall edge data.  Reads seq, raw,
 * and us in a single critical section so they are always consistent
 * with each other.  Without this, a Hall edge arriving between
 * reading s_irq_seq and reading s_irq_us could give a seq from one
 * edge and a us from the next (or vice versa).  The critical section
 * is ~6 instructions (< 100 ns at 96 MHz) so it does not interfere
 * with the 20 kHz PWM ISR timing. */
static void irq_snapshot(uint32_t *seq, uint8_t *raw, uint32_t *us)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    *seq = s_irq_seq;
    *raw = s_irq_raw;
    *us  = s_irq_us;
    __set_PRIMASK(primask);
}

/* ---- EXTI callback (ISSUE-007) -----------------------------------
 * Called from EXTI9_5_IRQHandler via HAL_GPIO_EXTI_IRQHandler.
 * Fires on every rising/falling edge of PB6/PB7/PB8.
 *
 * Minimal ISR work: sample the Hall pins and TIM2 directly from
 * registers (no HAL call), bump the sequence counter.  Debounce
 * runs in HallSensor_Update() from the main loop.
 * ---------------------------------------------------------------- */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == HALL_A_NUMBER || GPIO_Pin == HALL_B_NUMBER ||
        GPIO_Pin == HALL_C_NUMBER) {
        /* Direct GPIOB->IDR read — avoids HAL overhead in the ISR.
         * PB6=bit6, PB7=bit7, PB8=bit8.  Map to Hall bits
         *   HALL_A (PB6) -> bit2, HALL_B (PB7) -> bit1, HALL_C (PB8) -> bit0 */
        uint32_t idr = GPIOB->IDR;
        uint8_t raw = 0U;
        if (idr & HALL_A_NUMBER) raw |= 0x4U;
        if (idr & HALL_B_NUMBER) raw |= 0x2U;
        if (idr & HALL_C_NUMBER) raw |= 0x1U;
        s_irq_raw = raw;
        s_irq_us  = TIM2->CNT;          /* direct register, 1 MHz tick */
        s_irq_seq++;
        s_irq_count++;
    }
}

/* Monotonic microsecond timestamp from TIM2 (32-bit, 1 MHz).
 * NEVER use the TIM1 PWM counter for timestamps — it wraps at 20 kHz
 * (~50 us) and is not a monotonic timebase (ISSUE-006). */

typedef struct {
    uint8_t  rawCandidate;
    uint8_t  rawCandidateCount;
    uint32_t candidateStartUs;
    uint8_t  stableRaw;
    uint8_t  lastValidState;
    uint8_t  lastDrivenState;

    uint32_t lastValidUs;
    uint32_t lastTransitionUs;
    uint32_t prevTransitionUs;
    uint32_t hallPeriodUs;
    uint32_t invalidSinceUs;

    uint32_t invalidRawCount;
    uint32_t invalidTransitionCount;
    uint32_t validTransitionCount;

    HallFault fault;
} HallRt;

static HallRt   s_hall;
static float    s_raw_rpm       = 0.0f;
static float    s_filtered_rpm  = 0.0f;
static uint32_t s_edge_counter  = 0U;    /* monotonic, never resets */
/* Last s_irq_seq value consumed by HallSensor_Update().  The RPM
 * period calculation only uses the ISR timestamp when a NEW edge
 * arrived since the last update (s_irq_seq != s_last_irq_seq) AND
 * the ISR raw snapshot matches the current debounced raw — this
 * avoids reusing a stale timestamp from an earlier edge, and avoids
 * using an ISR timestamp whose raw value does not correspond to the
 * stable transition we just accepted. */
static uint32_t s_last_irq_seq   = 0U;

static uint8_t read_raw_hall(void)
{
    /* PB6 = bit2, PB7 = bit1, PB8 = bit0.  Use HAL for the main-loop
     * path (clear, readable); the ISR uses a direct IDR read. */
    GPIO_PinState a = HAL_GPIO_ReadPin(HALL_A_GPIOPORT, HALL_A_NUMBER);
    GPIO_PinState b = HAL_GPIO_ReadPin(HALL_B_GPIOPORT, HALL_B_NUMBER);
    GPIO_PinState c = HAL_GPIO_ReadPin(HALL_C_GPIOPORT, HALL_C_NUMBER);

    uint8_t v = 0U;
    if (a == GPIO_PIN_SET) v |= 0x4U;
    if (b == GPIO_PIN_SET) v |= 0x2U;
    if (c == GPIO_PIN_SET) v |= 0x1U;
    return v;
}

void HallSensor_Init(void)
{
    memset(&s_hall, 0, sizeof(s_hall));

    uint32_t now = App_GetMicros();
    uint8_t  raw = read_raw_hall();

    s_hall.rawCandidate      = raw;
    s_hall.rawCandidateCount = 1U;
    s_hall.candidateStartUs  = now;
    s_hall.stableRaw         = raw;

    uint8_t mapped = Commutation_HallToState(raw);
    s_hall.lastValidUs = now;
    if (Commutation_IsValidState(mapped)) {
        s_hall.lastValidState = mapped;
    } else {
        s_hall.lastValidState = HALL_STATE_INVALID;
        s_hall.invalidSinceUs = now;
    }
    s_hall.lastDrivenState = s_hall.lastValidState;
    s_hall.fault           = HALL_FAULT_NONE;
    s_last_irq_seq         = s_irq_seq;
}

void HallSensor_Update(void)
{
    /* ISSUE-035: the previous version only ran the debounce state
     * machine when s_hall_irq_pending was set by the EXTI ISR.  With
     * HALL_STABLE_SAMPLES=2 the first sample set candidateCount=1 and
     * the function returned; the second sample required a NEW EXTI
     * event, which may never come if the motor is coasting or the
     * edge was missed.  The stable transition was therefore never
     * recognised.
     *
     * Fix: run the debounce every call.  App_Loop calls this every
     * iteration.  The interval between consecutive samples depends on
     * the App_Loop iteration rate (typically sub-millisecond when the
     * CPU is not busy with UART/telemetry).  With HALL_STABLE_SAMPLES=2
     * the debounce window is approximately two loop iterations — fast
     * enough for bring-up RPM, and not
     * gated by EXTI delivery.  The ISR snapshot (seq/raw/us) is still
     * captured for diagnostics and for a future ISR-driven RPM path.
     *
     * No current sense / current limit is used here.  Faults are
     * based only on Hall pattern, transition validity, and timing. */

    uint32_t now = App_GetMicros();
    uint8_t  raw = read_raw_hall();

    /* ---- Time-based debounce: require the raw value to be stable
     * for at least HALL_DEBOUNCE_US microseconds before accepting.
     * This replaces the old sample-count approach which depended on
     * the main-loop iteration rate. ---- */
    if (raw == s_hall.rawCandidate) {
        if (s_hall.rawCandidateCount < 255U) s_hall.rawCandidateCount++;
    } else {
        s_hall.rawCandidate      = raw;
        s_hall.rawCandidateCount = 1U;
        s_hall.candidateStartUs  = now;
    }

    if (s_hall.rawCandidateCount < HALL_STABLE_SAMPLES) return;
    if ((now - s_hall.candidateStartUs) < HALL_DEBOUNCE_US) return;

    if (raw == s_hall.stableRaw) {
        /* Same stable value — but if it is an invalid code, the
         * persistence timer keeps running so a long-held 0b000/0b111
         * still faults.  Handle below. */
    } else {
        uint8_t prevState = Commutation_HallToState(s_hall.stableRaw);
        uint8_t newState  = Commutation_HallToState(raw);

        s_hall.stableRaw        = raw;
        s_hall.lastTransitionUs = now;

        if (!Commutation_IsValidState(newState)) {
            if (s_hall.invalidSinceUs == 0U) s_hall.invalidSinceUs = now;
            s_hall.invalidRawCount++;
        } else {
            /* Valid state — keep lastValidUs fresh so the "no Hall"
             * timeout doesn't fire while the motor is being driven. */
            s_hall.lastValidUs  = now;

            if (!Commutation_IsTransitionValid(prevState, newState)) {
                if (s_hall.invalidSinceUs == 0U) s_hall.invalidSinceUs = now;
                s_hall.invalidTransitionCount++;
            } else {
                s_hall.lastValidState    = newState;
                s_hall.invalidSinceUs    = 0U;
                s_hall.validTransitionCount++;
                s_edge_counter++;                    /* monotonic counter */

                /* Update RPM from the period between two valid
                 * transitions.  Prefer the ISR timestamp (captured at
                 * the edge) when a NEW ISR event arrived since the last
                 * update AND its raw snapshot matches the stable raw we
                 * just accepted.  Otherwise fall back to the main-loop
                 * TIM2 read — less precise (up to one loop period of
                 * latency) but still monotonic.  Matching the raw
                 * value avoids reusing a stale ISR timestamp from an
                 * earlier, different Hall code. */
                /* ISSUE-045: read the ISR snapshot atomically so seq,
                 * raw, and us are always consistent with each other.
                 * Without the critical section a Hall edge arriving
                 * between reading seq and reading us could give a seq
                 * from one edge and a us from the next. */
                uint32_t irq_seq_snap;
                uint8_t  irq_raw_snap;
                uint32_t irq_us_snap;
                irq_snapshot(&irq_seq_snap, &irq_raw_snap, &irq_us_snap);

                uint32_t ts = now;
                if (irq_seq_snap != s_last_irq_seq && irq_raw_snap == raw) {
                    ts = irq_us_snap;
                }
                s_last_irq_seq = irq_seq_snap;
                if (s_hall.prevTransitionUs != 0U) {
                    uint32_t period = ts - s_hall.prevTransitionUs;
                    if (period > 0U && period < 1000000U) {
                        s_hall.hallPeriodUs = period;
                    }
                }
                s_hall.prevTransitionUs = ts;

                /* The motor now drives this state. */
                s_hall.lastDrivenState = newState;

                /* Periodic reset of the illegal-transition counter so
                 * a long-running motor does not trip the threshold
                 * from accumulated noise. */
                if ((s_hall.validTransitionCount % 100U) == 0U) {
                    s_hall.invalidTransitionCount = 0U;
                }
            }
        }
    }

    /* ---- Fault evaluation (no current sense; Hall-only) ---- */

    /* Invalid-Hall persistence: 0b000 or 0b111 held longer than
     * INVALID_HALL_STOP_US while the motor is expected to be moving.
     * The caller (app_main) decides whether the motor is running; we
     * just report the persistence so the app can raise the fault at
     * the right time. */
    uint8_t mappedNow = Commutation_HallToState(s_hall.stableRaw);
    if (!Commutation_IsValidState(mappedNow)) {
        if (s_hall.invalidSinceUs != 0U &&
            (now - s_hall.invalidSinceUs) > INVALID_HALL_STOP_US) {
            s_hall.fault = HALL_FAULT_INVALID_PERSIST;
        }
    } else {
        /* Valid state present — clear the persistence fault flag if
         * the Hall recovered.  The latched FaultManager fault (raised
         * by the app) still needs clrerr. */
        if (s_hall.fault == HALL_FAULT_INVALID_PERSIST) {
            s_hall.fault = HALL_FAULT_NONE;
        }
    }

    /* Illegal-transition spam: more than INVALID_TRANSITION_THRESHOLD
     * invalid transitions since the last periodic reset. */
    if (s_hall.invalidTransitionCount > INVALID_TRANSITION_THRESHOLD) {
        s_hall.fault = HALL_FAULT_ILLEGAL_TRANSITION;
    }

    HallSensor_UpdateFilteredRpm();
}

uint8_t HallSensor_GetStableRaw(void)        { return s_hall.stableRaw; }
uint8_t HallSensor_GetMappedState(void)      { return s_hall.lastValidState; }
uint8_t HallSensor_GetLastDrivenState(void)  { return s_hall.lastDrivenState; }
uint32_t HallSensor_GetLastTransitionUs(void){ return s_hall.lastTransitionUs; }

bool HallSensor_HasValidEdge(void)           { return s_hall.prevTransitionUs != 0U; }

uint32_t HallSensor_CalculateRpm(void)
{
    if (s_hall.hallPeriodUs == 0U) return 0U;

    uint32_t now = App_GetMicros();
    uint32_t since_last = (s_hall.prevTransitionUs == 0U) ? 0U
                         : (now - s_hall.prevTransitionUs);
    if (since_last > HALL_RPM_TIMEOUT_US) return 0U;

    /* 60,000,000 us/min / (period_us * 6 edges/electrical rev * pole pairs) */
    uint32_t denom = s_hall.hallPeriodUs * 6U * MOTOR_POLE_PAIRS;
    if (denom == 0U) return 0U;
    return 60000000UL / denom;
}

float HallSensor_GetRawRpm(void)
{
    return (float)HallSensor_CalculateRpm();
}

float HallSensor_GetFilteredRpm(void)
{
    return s_filtered_rpm;
}

void HallSensor_UpdateFilteredRpm(void)
{
    float raw = HallSensor_GetRawRpm();
    s_raw_rpm      = raw;
    s_filtered_rpm = HALL_FILTER_ALPHA * raw
                   + (1.0f - HALL_FILTER_ALPHA) * s_filtered_rpm;
}

HallFreshness HallSensor_GetFreshness(void)
{
    if (s_hall.prevTransitionUs == 0U) return HALL_STALE;
    uint32_t now = App_GetMicros();
    uint32_t since = now - s_hall.prevTransitionUs;
    return (since > HALL_RPM_TIMEOUT_US) ? HALL_STALE : HALL_FRESH;
}

uint32_t HallSensor_GetEdgeCounter(void)             { return s_edge_counter; }
uint32_t HallSensor_GetIrqCount(void)                { return s_irq_count; }
uint32_t HallSensor_GetInvalidRawCount(void)         { return s_hall.invalidRawCount; }
uint32_t HallSensor_GetInvalidTransitionCount(void)  { return s_hall.invalidTransitionCount; }
uint32_t HallSensor_GetValidTransitionCount(void)    { return s_hall.validTransitionCount; }
uint8_t  HallSensor_GetCandidateRaw(void)            { return s_hall.rawCandidate; }
uint8_t  HallSensor_GetCandidateCount(void)          { return s_hall.rawCandidateCount; }

HallFault HallSensor_GetFault(void)                  { return s_hall.fault; }
void HallSensor_ClearFault(void)
{
    s_hall.fault = HALL_FAULT_NONE;
    s_hall.invalidSinceUs = 0U;
    s_hall.invalidTransitionCount = 0U;
}
