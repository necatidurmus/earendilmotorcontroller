/* ============================================================
 * App/Src/motor_driver.c
 *
 * Low-level asynchronous 6-step commutation driver for the F411
 * power stage.  Uses TIM1 complementary outputs (CH1/CH1N,
 * CH2/CH2N, CH3/CH3N) but NEVER in synchronous complementary PWM
 * mode.  For each electrical sector exactly one phase high-side is
 * PWM-modulated and one *different* phase low-side is held statically
 * ON; the third phase floats.
 *
 * CCxE / CCxNE model (see docs/TIM1_GATE_DRIVE.md):
 *
 *   Phase A:  AH = TIM1_CH1  -> CC1E    (high-side enable)
 *             AL = TIM1_CH1N -> CC1NE   (low-side  enable)
 *   Phase B:  BH = TIM1_CH2  -> CC2E
 *             BL = TIM1_CH2N -> CC2NE
 *   Phase C:  CH = TIM1_CH3  -> CC3E
 *             CL = TIM1_CH3N -> CC3NE
 *
 *   High PWM phase : OCxM = PWM1 (110), OCxPE = 1, CCxE = 1, CCxNE = 0,
 *                    CCRx = duty.  OCxREF follows PWM, OCxN is OFF.
 *   Low  ON  phase : OCxM = forced inactive (100) so OCxREF = 0 and the
 *                    complementary output OCxN is held HIGH (active).
 *                    CCxE = 0, CCxNE = 1.  The low-side gate is static ON.
 *   Floating phase : CCxE = 0, CCxNE = 0, OCxM = forced inactive.
 *
 * Invariant: CCxE and CCxNE are NEVER both set for the same phase in
 * the same step.  The 6-step table also guarantees high_phase !=
 * low_phase for every sector, so no same-phase shoot-through is
 * possible by construction.  ApplyStep additionally disables the
 * previous phase before enabling the new one.
 *
 * CCMR bit masks are channel-specific (TIM_CCMR1_OC1M / OC2M /
 * TIM_CCMR2_OC3M and OC1PE / OC2PE / OC3PE).  We never shift a
 * channel-1 mask by another channel's field offset — that was the
 * original CCMR corruption bug (ISSUE-003).
 *
 * HAL is used only for one-time init; the hot path writes TIM1
 * registers directly.  No analogWrite / digitalWrite anywhere.
 * ============================================================ */

#include "motor_driver.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"
#include "tim.h"          /* htim1 from Core/Src/tim.c */
#include "main.h"         /* Error_Handler */

#include <string.h>

/* Duty supplied by SpeedPI / manual command, in 0..250 range.
 * User duty is clamped to 250; CCR mapping uses 255 denominator
 * intentionally to keep headroom (duty 250 -> CCR < ARR+1). */
static uint8_t  s_duty           = 0U;
static uint16_t s_ccr_ticks      = 0U;   /* last CCR value written */
static uint8_t  s_last_high_idx  = 0xFF; /* 0..2, 0xFF = none */
static uint8_t  s_last_low_idx   = 0xFF;

/* 6-step table: which phase index is high (PWM) / low (ON) per sector.
 * 0=A, 1=B, 2=C.  Matches the legacy Arduino PH_HIGH/PH_LOW arrays:
 *   PH_HIGH = {B, C, C, A, A, B}
 *   PH_LOW  = {A, A, B, B, C, C}
 * (see docs/TIM1_GATE_DRIVE.md for the cross-check). */
typedef struct {
    uint8_t high;   /* 0=A, 1=B, 2=C */
    uint8_t low;    /* 0=A, 1=B, 2=C */
} DrivePhase;

static DrivePhase s_drive_table[12];  /* 6 sectors * 2 directions */

/* ----------------------------------------------------------------
 * Per-channel register descriptor.
 *
 * OCxM field values:  4 = forced inactive (OCxREF=0 -> OCxN HIGH),
 *                     6 = PWM mode 1.
 * ---------------------------------------------------------------- */
typedef struct {
    volatile uint32_t *ccmr;          /* &TIM1->CCMR1 or CCMR2      */
    volatile uint32_t *ccr;           /* &TIM1->CCR1/2/3            */
    uint32_t ocm_msk;                 /* OCxM field mask            */
    uint32_t ocpe;                    /* OCxPE bit for this channel */
    uint32_t cce;                     /* CCxE bit in CCER           */
    uint32_t ccne;                    /* CCxNE bit in CCER          */
} PhaseReg;

static const PhaseReg s_phase[3] = {
    /* Phase A = CH1:  OC1M[6:4], OC1PE bit3, CC1E/CC1NE */
    { &TIM1->CCMR1, &TIM1->CCR1, TIM_CCMR1_OC1M, TIM_CCMR1_OC1PE,
      TIM_CCER_CC1E, TIM_CCER_CC1NE },
    /* Phase B = CH2:  OC2M[14:12], OC2PE bit11, CC2E/CC2NE */
    { &TIM1->CCMR1, &TIM1->CCR2, TIM_CCMR1_OC2M, TIM_CCMR1_OC2PE,
      TIM_CCER_CC2E, TIM_CCER_CC2NE },
    /* Phase C = CH3:  OC3M[6:4] in CCMR2, OC3PE bit3, CC3E/CC3NE */
    { &TIM1->CCMR2, &TIM1->CCR3, TIM_CCMR2_OC3M, TIM_CCMR2_OC3PE,
      TIM_CCER_CC3E, TIM_CCER_CC3NE },
};

/* OCxM field shift: CH1/CH3 -> 4, CH2 -> 12.  Derived from the
 * mask position so it stays correct if the macro changes. */
#define OCM_SHIFT(p)   ((p) == 1 ? 12U : 4U)

/* ----------------------------------------------------------------
 * Output-compare mode helpers (CCMR only — no CCER changes here).
 * ---------------------------------------------------------------- */

static void set_oc_mode_pwm(uint8_t phase)
{
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    uint32_t shift = OCM_SHIFT(phase);
    uint32_t v = *r->ccmr;
    v &= ~r->ocm_msk;                 /* clear OCxM field         */
    v |=  (6U << shift);              /* OCxM = 110 = PWM mode 1  */
    v |=  r->ocpe;                    /* OCxPE = 1 (preload)      */
    *r->ccmr = v;
}

static void set_oc_mode_force_active_low_side(uint8_t phase)
{
    /* OCxM = forced inactive (100) -> OCxREF = 0 -> OCxN HIGH.
     * Used together with CCxNE=1 to hold the low-side statically ON. */
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    uint32_t shift = OCM_SHIFT(phase);
    uint32_t v = *r->ccmr;
    v &= ~r->ocm_msk;                 /* clear OCxM field         */
    v &= ~r->ocpe;                    /* OCxPE = 0 (no preload)   */
    v |=  (4U << shift);              /* OCxM = 100 = forced inactive */
    *r->ccmr = v;
}

static void set_oc_mode_force_inactive(uint8_t phase)
{
    /* OCxM = forced inactive (100).  Used for the floating phase and
     * by phase_disable().  OCxREF = 0 so neither OCx nor OCxN is
     * driven active by the compare logic (the output enable bits in
     * CCER decide whether the pin is actually driven). */
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    uint32_t shift = OCM_SHIFT(phase);
    uint32_t v = *r->ccmr;
    v &= ~r->ocm_msk;
    v &= ~r->ocpe;
    v |=  (4U << shift);
    *r->ccmr = v;
}

/* ----------------------------------------------------------------
 * Phase-level helpers (CCER + CCMR).
 *
 * Order matters: the channel is disabled in CCER BEFORE the OCxM
 * mode is changed, and re-enabled AFTER, to avoid a transient where
 * both CCxE and CCxNE could interpret a changing OCxREF.
 * ---------------------------------------------------------------- */

static void phase_disable(uint8_t phase)
{
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    uint32_t ccer = TIM1->CCER;
    ccer &= ~(r->cce | r->ccne);      /* CCxE = 0, CCxNE = 0 */
    TIM1->CCER = ccer;
    set_oc_mode_force_inactive(phase);
}

static void phase_high_pwm(uint8_t phase, uint16_t ccr)
{
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    /* Disable both enables first, then configure mode, then enable
     * ONLY the high-side (CCxE).  CCxNE stays clear. */
    uint32_t ccer = TIM1->CCER;
    ccer &= ~(r->cce | r->ccne);
    TIM1->CCER = ccer;
    set_oc_mode_pwm(phase);
    *r->ccr = ccr;
    ccer = TIM1->CCER;
    ccer |= r->cce;                   /* high-side ON only */
    TIM1->CCER = ccer;
}

static void phase_low_on(uint8_t phase)
{
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    uint32_t ccer = TIM1->CCER;
    ccer &= ~(r->cce | r->ccne);
    TIM1->CCER = ccer;
    set_oc_mode_force_active_low_side(phase);
    ccer = TIM1->CCER;
    ccer |= r->ccne;                  /* low-side ON only */
    TIM1->CCER = ccer;
}

static void phase_all_off(void)
{
    /* Clear every CCxE/CCxNE bit and force all OCxM inactive.
     * MOE is left enabled; with all channel enables clear the TIM1
     * outputs go to the OFF state.  With OSSI/OSSR disabled
     * (ISSUE-029/034) disabled channels are Hi-Z, not idle-low —
     * OCxIDLE=RESET only applies when OSSI/OSSR=1.  The actual gate
     * pin level therefore depends on the gate driver's input
     * (pull-down / floating); this must be scope-verified at
     * BRINGUP Stage 2.  This is conservative for first bring-up
     * and matches the active tim.c / .ioc. */
    phase_disable(0);
    phase_disable(1);
    phase_disable(2);
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void MotorDriver_Init(void)
{
    /* Forward sector->phase map (matches legacy Arduino PH_HIGH/PH_LOW). */
    static const uint8_t fwd_high[6] = {1, 2, 2, 0, 0, 1}; /* B,C,C,A,A,B */
    static const uint8_t fwd_low [6] = {0, 0, 1, 1, 2, 2}; /* A,A,B,B,C,C */

    for (uint8_t s = 0; s < 6; s++) {
        s_drive_table[s].high = fwd_high[s];
        s_drive_table[s].low  = fwd_low[s];
        /* Reverse: 180 electrical degrees (shift by 3 sectors). */
        uint8_t r = (uint8_t)((s + 3U) % 6U);
        s_drive_table[6 + s].high = fwd_high[r];
        s_drive_table[6 + s].low  = fwd_low[r];
    }

    s_duty          = 0U;
    s_ccr_ticks     = 0U;
    s_last_high_idx = 0xFFU;
    s_last_low_idx  = 0xFFU;

    /* Make sure all channels are disabled and in forced-inactive mode
     * BEFORE enabling MOE / starting the counter, so no gate can
     * glitch on at boot. */
    phase_all_off();

    /* Load shadow registers (ARR/CCR preload) with an update event. */
    if (HAL_TIM_GenerateEvent(&htim1, TIM_EVENTSOURCE_UPDATE) != HAL_OK) {
        Error_Handler();
    }

    /* Enable main output (MOE) and start the counter.  With every
     * CCxE/CCxNE = 0 the TIM1 outputs are in the OFF state (Hi-Z
     * with OSSI/OSSR disabled — see phase_all_off comment).  The
     * gate-driver input level (LOW vs floating) depends on the
     * board; verify on a scope at BRINGUP Stage 2. */
    TIM1->BDTR |= TIM_BDTR_MOE;
    __HAL_TIM_ENABLE(&htim1);

    MotorDriver_AllOff();
}

void MotorDriver_AllOff(void)
{
    phase_all_off();
    s_duty          = 0U;
    s_ccr_ticks     = 0U;
    s_last_high_idx = 0xFFU;
    s_last_low_idx  = 0xFFU;
}

void MotorDriver_Coast(void)
{
    MotorDriver_AllOff();
}

void MotorDriver_FaultOff(void)
{
    /* Hard stop: clear all channel enables AND clear MOE for maximum
     * cutoff.  With MOE=0 the TIM1 outputs are forced to their
     * idle/off state regardless of CCxE/CCxNE bits.  MOE is
     * re-enabled by MotorDriver_Init() on the next normal start
     * (via clrerr -> run_request -> MotorDriver_ApplyStep path).
     *
     * WARNING: With OSSI/OSSR disabled and no external gate-driver
     * pulldown resistors, the physical gate pin level after MOE=0
     * depends on the gate driver's input impedance.  This MUST be
     * verified with an oscilloscope at BRINGUP Stage 2. */
    phase_all_off();
    TIM1->BDTR &= ~TIM_BDTR_MOE;
    s_duty          = 0U;
    s_ccr_ticks     = 0U;
    s_last_high_idx = 0xFFU;
    s_last_low_idx  = 0xFFU;
}

void MotorDriver_SetDuty(uint8_t duty)
{
    if (duty > 250U) duty = 250U;     /* user duty clamped to 250 */
    s_duty = duty;
    /* Map 0..250 user duty to 0..PWM_PERIOD_TICKS.
     * Uses 255 denominator intentionally: duty 250 maps to
     * CCR = 250 * 4800 / 255 = 4705 (< ARR+1=4800), leaving
     * headroom so CCR never equals ARR+1 (100% duty). */
    uint32_t period = (uint32_t)TIM1->ARR + 1U;
    s_ccr_ticks = (uint16_t)(((uint32_t)s_duty * period) / 255U);
}

uint8_t  MotorDriver_GetDuty(void)            { return s_duty; }
uint16_t MotorDriver_GetCurrentCcrTicks(void) { return s_ccr_ticks; }

void MotorDriver_ApplyStep(uint8_t sector, int8_t direction, uint8_t duty)
{
    if (sector > 5U || direction == 0 || duty == 0U) {
        MotorDriver_AllOff();
        return;
    }

    /* Re-enable MOE if it was cleared by FaultOff.  Safe because
     * all CCxE/CCxNE bits are clear at this point; the per-phase
     * enables below are the real gate control. */
    if (!(TIM1->BDTR & TIM_BDTR_MOE)) {
        TIM1->BDTR |= TIM_BDTR_MOE;
    }

    MotorDriver_SetDuty(duty);

    uint8_t tbl_idx   = (direction > 0) ? sector : (uint8_t)(6U + sector);
    uint8_t new_high  = s_drive_table[tbl_idx].high;
    uint8_t new_low   = s_drive_table[tbl_idx].low;

    /* The table guarantees new_high != new_low.  If the same sector is
     * being driven again (same high/low phase pair), only update the
     * CCR register — do NOT touch CCER enable bits or CCMR mode.
     * Re-toggling CCxE via phase_high_pwm() causes a brief disable/
     * re-enable cycle that produces gate glitch/jitter on the
     * high-side output. */
    if (new_high == s_last_high_idx && new_low == s_last_low_idx) {
        *s_phase[new_high].ccr = s_ccr_ticks;
        return;
    }

    /* Disable the previous low-side first, then the previous
     * high-side, before enabling anything new.  This guarantees no
     * same-phase overlap during a transition. */
    if (s_last_low_idx  != 0xFFU && s_last_low_idx  != new_low)  phase_disable(s_last_low_idx);
    if (s_last_high_idx != 0xFFU && s_last_high_idx != new_high) phase_disable(s_last_high_idx);

    /* ISSUE-028: Only touch the phase that actually changes.  On a
     * sector transition exactly one of (high, low) changes; the
     * other must stay continuously driven to avoid a gate glitch
     * and torque ripple.  The legacy Arduino firmware guarded these
     * with if (oldH != newH) / if (oldL != newL); the cube firmware
     * previously called phase_low_on/phase_high_pwm unconditionally,
     * briefly disabling and re-enabling the unchanged phase. */
    if (new_low  != s_last_low_idx)  phase_low_on(new_low);
    if (new_high != s_last_high_idx) phase_high_pwm(new_high, s_ccr_ticks);
    else                          *s_phase[new_high].ccr = s_ccr_ticks;

    s_last_high_idx = new_high;
    s_last_low_idx  = new_low;
}
