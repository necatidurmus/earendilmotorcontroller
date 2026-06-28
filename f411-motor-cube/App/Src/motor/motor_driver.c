/* ============================================================
 * App/Src/motor_driver.c
 *
 * Low-level asynchronous 6-step commutation driver for the F411
 * power stage.  Uses TIM1 PWM for high-side and GPIO for low-side.
 *
 * HIGH-side: TIM1 CH1/CH2/CH3 (PA8/PA9/PA10) with CCxE enable.
 * LOW-side:  GPIO push-pull (PA7/PB0/PB1) — pins reconfigured
 *            from TIM1 AF1 to GPIO OUTPUT in Core/Src/main.c
 *            BEFORE MotorDriver_Init() runs.
 *
 * TIM1 complementary outputs (CCxNE + forced inactive mode) were
 * found to not reliably drive the low-side gates on this hardware.
 * The Arduino firmware uses GPIO for low-side control, so we
 * replicate that approach.
 * ============================================================ */

#include "motor_driver.h"
#include "bldc_commutation.h"  /* O4: canonical 6-step drive table */
#include "app_config.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"
#include "tim.h"          /* htim1 from Core/Src/tim.c */
#include "main.h"         /* Error_Handler */

#include <string.h>

/* ----------------------------------------------------------------
 * LOW-side GPIO mapping.
 *
 * TIM1 complementary outputs (CCxNE) do not reliably drive the
 * low-side gates on this hardware.  The proven Arduino firmware
 * uses GPIO for low-side control.  We replicate that: PA7/AL,
 * PB0/BL, PB1/CL are configured as GPIO push-pull outputs after
 * HAL_TIM_MspPostInit() sets them to AF1.
 * ---------------------------------------------------------------- */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} LowSideGpio;

static const LowSideGpio s_low_gpio[3] = {
    { GPIOA, GPIO_PIN_7  },   /* Phase A low-side = PA7 (AL) */
    { GPIOB, GPIO_PIN_0  },   /* Phase B low-side = PB0 (BL) */
    { GPIOB, GPIO_PIN_1  },   /* Phase C low-side = PB1 (CL) */
};

static void low_side_on(uint8_t phase)
{
    if (phase > 2U) return;
    HAL_GPIO_WritePin(s_low_gpio[phase].port, s_low_gpio[phase].pin, GPIO_PIN_SET);
}

static void low_side_off(uint8_t phase)
{
    if (phase > 2U) return;
    HAL_GPIO_WritePin(s_low_gpio[phase].port, s_low_gpio[phase].pin, GPIO_PIN_RESET);
}

/* Duty supplied by SpeedPI / manual command, in 0..PWM_MAX_DUTY range. */
static uint16_t s_duty           = 0U;
static uint16_t s_ccr_ticks      = 0U;   /* last CCR value written */
static uint8_t  s_last_high_idx  = 0xFF; /* 0..2, 0xFF = none */
static uint8_t  s_last_low_idx   = 0xFF;
static volatile bool s_safety_locked = false; /* safety lock flag — written by ISR (FaultOff) */

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
    /* Also drive LOW-side GPIO pin LOW */
    low_side_off(phase);
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
    /* LOW-side driven via GPIO (Arduino-style), not TIM1 CCxNE.
     * TIM1 complementary outputs don't reliably drive the gates on
     * this hardware.  The GPIO pin was reconfigured from AF1 to
     * OUTPUT_PP in MotorDriver_Init(). */
    if (phase > 2U) return;
    const PhaseReg *r = &s_phase[phase];
    /* Disable TIM1 channel entirely — we don't use CCxNE */
    uint32_t ccer = TIM1->CCER;
    ccer &= ~(r->cce | r->ccne);
    TIM1->CCER = ccer;
    set_oc_mode_force_inactive(phase);
    /* Drive the GPIO pin HIGH */
    low_side_on(phase);
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
    /* O4: Use the canonical 6-step drive table from bldc_commutation.c
     * instead of maintaining a duplicate copy.  This eliminates the
     * risk of the two copies getting out of sync and causing silent
     * commutation failures. */
    for (uint8_t s = 0; s < 6; s++) {
        Commutation_GetDrivePhases(s, +1,
            &s_drive_table[s].high, &s_drive_table[s].low);
        Commutation_GetDrivePhases(s, -1,
            &s_drive_table[6 + s].high, &s_drive_table[6 + s].low);
    }

    s_duty          = 0U;
    s_ccr_ticks     = 0U;
    s_last_high_idx = 0xFFU;
    s_last_low_idx  = 0xFFU;
    s_safety_locked = false;

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
    s_safety_locked = true;  /* prevent MOE re-enable until clrerr */
}

void MotorDriver_ActiveBrake(void)
{
    /* Safety lock: block active brake after a fault. */
    if (s_safety_locked) {
        MotorDriver_AllOff();
        return;
    }

    /* Active brake: all low-side ON via GPIO, all high-side OFF.
     * TIM1 complementary outputs (CCxNE) don't drive the low-side
     * gates on this hardware — use GPIO like the Arduino firmware. */

    /* All TIM1 outputs off */
    TIM1->CCER = 0U;

    /* All channels forced inactive */
    set_oc_mode_force_inactive(0);
    set_oc_mode_force_inactive(1);
    set_oc_mode_force_inactive(2);

    /* CCR = 0 for all channels */
    TIM1->CCR1 = 0U;
    TIM1->CCR2 = 0U;
    TIM1->CCR3 = 0U;

    /* All low-side GPIOs HIGH */
    low_side_on(0);
    low_side_on(1);
    low_side_on(2);

    /* Ensure MOE is set */
    TIM1->BDTR |= TIM_BDTR_MOE;

    s_duty          = 0U;
    s_ccr_ticks     = 0U;
    s_last_high_idx = 0xFFU;
    s_last_low_idx  = 0xFFU;
}

void MotorDriver_SetDuty(uint16_t duty)
{
    if (duty > PWM_MAX_DUTY) duty = PWM_MAX_DUTY;
    s_duty = duty;
    /* Map 0..PWM_MAX_DUTY user duty to 0..PWM_PERIOD_TICKS.
     * duty = PWM_MAX_DUTY maps to CCR = ARR (4799), i.e. ~100 %. */
    s_ccr_ticks = (uint16_t)(((uint32_t)s_duty * PWM_PERIOD_TICKS) / PWM_MAX_DUTY);
}

uint16_t MotorDriver_GetDuty(void)            { return s_duty; }
uint16_t MotorDriver_GetCurrentCcrTicks(void) { return s_ccr_ticks; }

void MotorDriver_ApplyStep(uint8_t sector, int8_t direction, uint16_t duty)
{
    if (sector > 5U || direction == 0 || duty == 0U) {
        MotorDriver_AllOff();
        return;
    }

    /* Safety lock: block gate re-enable after a fault. */
    if (s_safety_locked) {
        MotorDriver_AllOff();
        return;
    }

    /* Re-enable MOE if it was cleared by FaultOff. */
    if (!(TIM1->BDTR & TIM_BDTR_MOE)) {
        TIM1->BDTR |= TIM_BDTR_MOE;
    }

    MotorDriver_SetDuty(duty);

    uint8_t tbl_idx   = (direction > 0) ? sector : (uint8_t)(6U + sector);
    uint8_t new_high  = s_drive_table[tbl_idx].high;
    uint8_t new_low   = s_drive_table[tbl_idx].low;

    /* Same sector — just update CCR. */
    if (new_high == s_last_high_idx && new_low == s_last_low_idx) {
        *s_phase[new_high].ccr = s_ccr_ticks;
        return;
    }

    /* DIRECT REGISTER APPROACH (matches working gpiotest):
     * 1. Disable all TIM1 outputs (CCER = 0)
     * 2. Set all LOW-side GPIOs LOW
     * 3. Configure HIGH-side channel PWM (CCMR + CCR)
     * 4. Enable HIGH-side (CCER)
     * 5. Set LOW-side GPIO HIGH */

    /* Step 1: All TIM1 outputs off */
    TIM1->CCER = 0U;

    /* Step 2: All LOW-side GPIOs off */
    for (uint8_t i = 0; i < 3; i++) {
        low_side_off(i);
    }

    /* Step 3: Configure HIGH-side PWM.
     * OCxM = PWM mode 1 (110), OCxPE = 1 (preload). */
    {
        const PhaseReg *r = &s_phase[new_high];
        uint32_t shift = OCM_SHIFT(new_high);
        volatile uint32_t *ccmr = r->ccmr;
        uint32_t v = *ccmr;
        v &= ~r->ocm_msk;
        v |= (6U << shift);
        v |= r->ocpe;
        *ccmr = v;
        *r->ccr = s_ccr_ticks;
    }

    /* Step 4: Enable HIGH-side only */
    TIM1->CCER = s_phase[new_high].cce;

    /* Step 5: LOW-side GPIO HIGH */
    low_side_on(new_low);

    s_last_high_idx = new_high;
    s_last_low_idx  = new_low;
}

void MotorDriver_SetSafetyLock(bool locked) { s_safety_locked = locked; }
bool MotorDriver_IsSafetyLocked(void)       { return s_safety_locked; }
