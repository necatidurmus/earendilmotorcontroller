/* ============================================================
 * Core/Src/tim.c
 * TIM1 (motor PWM), TIM2 (32-bit 1 MHz micros timestamp),
 * TIM4 (Hall interface placeholder) init.
 *
 * CubeMX-generated style.  The 1 kHz scheduler tick is driven by
 * SysTick on the STM32F411 (no TIM6/TIM7 basic timers exist).
 * ============================================================ */

#include "tim.h"
#include "main.h"

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim4;

/* ============================================================
 * TIM1 — advanced-control motor PWM
 *
 * PWM frequency (edge-aligned, upcounting):
 *   f_pwm = TIM_CLK / (ARR + 1) = 96 MHz / 4800 = 20.0 kHz
 *
 * Edge-aligned mode is used for first bring-up because:
 *   - the frequency formula is unambiguous and matches ARR=4799,
 *   - gate waveforms are simpler to verify on a scope.
 * Center-aligned mode would give f = TIM_CLK / (2*(ARR+1)) = 10 kHz
 * for the same ARR; see docs/TIM1_GATE_DRIVE.md.
 *
 * Break input is DISABLED for first bring-up.  No BKIN pin is wired
 * on the board, so an enabled break with a floating BKIN line could
 * permanently assert break and keep MOE off.  See docs/KNOWN_RISKS.md.
 * ============================================================ */

void MX_TIM1_Init(void)
{
    TIM_ClockConfigTypeDef clk = {0};
    TIM_OC_InitTypeDef oc = {0};
    TIM_BreakDeadTimeConfigTypeDef bd = {0};

    htim1.Instance = TIM1;
    htim1.Init.Prescaler         = 0;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;   /* edge-aligned */
    htim1.Init.Period            = 4799;          /* 96 MHz / 4800 = 20 kHz */
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim1, &clk) != HAL_OK) {
        Error_Handler();
    }

    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = 0;
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;   /* idle = low (off) — only effective when OSSI/OSSR=1 */
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;  /* idle = low (off) — only effective when OSSI/OSSR=1 */

    if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2) != HAL_OK) Error_Handler();
    if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_3) != HAL_OK) Error_Handler();

    /* Dead-time ~0.66 us at 96 MHz (DTG=63, DTG<=127 so DT = DTG*t_DTS
     * = 63/96 MHz = 656 ns).  Not bench-verified yet.  The TIM1 clock
     * is unchanged (96 MHz) so DTG=63 gives the same dead-time as
     * before the 20 kHz PWM change.  Break DISABLED — no BKIN pin
     * wired.  OSSI/OSSR disabled (ISSUE-029): disabled channels go
     * OFF (Hi-Z), not idle-low; see docs/TIM1_GATE_DRIVE.md. */
    bd.DeadTime        = 63;
    bd.LockLevel       = TIM_LOCKLEVEL_OFF;
    bd.OffStateIDLEMode = TIM_OSSI_DISABLE;
    bd.OffStateRunMode  = TIM_OSSR_DISABLE;
    bd.BreakState      = TIM_BREAK_DISABLE;     /* ISSUE-005: no BKIN wired */
    bd.BreakPolarity   = TIM_BREAKPOLARITY_HIGH;
    bd.BreakFilter     = 0;
    bd.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;

    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bd) != HAL_OK) {
        Error_Handler();
    }

    /* Configure TIM1 GPIO alternate function (PA7/8/9/10, PB0/1).
     * This MUST be called here — without it the gate pins stay as
     * plain GPIO inputs and no TIM1 output reaches the gate driver. */
    HAL_TIM_MspPostInit(&htim1);

    /* ISSUE-031: Enable the TIM1 break and update NVIC lines so the
     * break fault path (App_Tim1BrkIsr -> FaultManager_Raise) is
     * reachable if break is ever wired (ISSUE-005).  Priority 5
     * matches the .ioc.  No behavioural change while break stays
     * disabled — no break events are generated. */
    HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);

    /* Outputs are NOT enabled here.  MOE is set and the counter is
     * started by MotorDriver_Init(); per-step CCxE/CCxNE enabling is
     * done by the motor driver so all gate outputs default to OFF. */
}

/* ============================================================
 * TIM2 — 32-bit free-running 1 MHz microsecond timer.
 *
 * TIM2 is on APB1; with APB1 prescaler != 1 the timer clock is
 * 96 MHz.  Prescaler 95 -> 1 MHz tick (1 us).  Period 0xFFFFFFFF
 * gives a 32-bit free-running counter that wraps every ~71.6 min;
 * unsigned subtraction handles wrap correctly.
 *
 * Used by hall_sensor.c for Hall edge timestamps and RPM period.
 * NEVER use the TIM1 PWM counter for timestamps — it wraps every
 * ~50 us (20 kHz) and is not a monotonic timebase.
 * ============================================================ */

void MX_TIM2_Init(void)
{
    htim2.Instance = TIM2;
    htim2.Init.Prescaler         = 95;            /* 96 MHz / 96 = 1 MHz */
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 0xFFFFFFFFUL;  /* 32-bit free-run */
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
        Error_Handler();
    }

    /* Start free-running; no interrupts needed. */
    if (HAL_TIM_Base_Start(&htim2) != HAL_OK) {
        Error_Handler();
    }
}

/* ============================================================
 * TIM4 — Hall sensor interface (DISABLED).
 *
 * Hall sensing uses GPIO EXTI on PB6/PB7/PB8 with TIM2 timestamps
 * (see hall_sensor.c, ISSUE-007).  TIM4 Hall mode was previously
 * initialised here, which risked CubeMX regeneration reassigning
 * PB6/PB7/PB8 to TIM4 alternate function and breaking the EXTI
 * Hall path.  TIM4 init is now a no-op.  The htim4 handle and
 * HAL_TIM_Base_MspInit callback are kept link-clean.
 * ============================================================ */

void MX_TIM4_Init(void)
{
    /* No-op: TIM4 is not used.  Hall sensing uses EXTI + TIM2. */
    (void)htim4;
}

void MX_TIM6_Init(void)
{
    /* Not used on STM32F411 — no TIM6/TIM7 basic timers.  Kept as a
     * no-op so the CubeMX .ioc reference does not need editing.  The
     * 1 kHz scheduler tick is provided by SysTick (HAL_IncTick). */
}

/* ----------------------------------------------------------------
 * HAL MSP callbacks — clock + GPIO bring-up.
 *
 * TIM1 is initialised with HAL_TIM_PWM_Init(), so the callback HAL
 * calls is HAL_TIM_PWM_MspInit() (NOT HAL_TIM_Base_MspInit).  The
 * previous code only implemented HAL_TIM_Base_MspInit, which meant
 * the TIM1 clock was never enabled — ISSUE-001.
 * ---------------------------------------------------------------- */

void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) {
        __HAL_RCC_TIM1_CLK_ENABLE();
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) {
        __HAL_RCC_TIM1_CLK_ENABLE();
    } else if (htim->Instance == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    } else if (htim->Instance == TIM4) {
        __HAL_RCC_TIM4_CLK_ENABLE();
    }
}

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef gpio = {0};
    if (htim->Instance != TIM1) return;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PA8  = TIM1_CH1    (AH)
     * PA9  = TIM1_CH2    (BH)
     * PA10 = TIM1_CH3    (CH)
     * PA7  = TIM1_CH1N   (AL)
     * PB0  = TIM1_CH2N   (BL)
     * PB1  = TIM1_CH3N   (CL) */
    /* P0-04: GPIO_PULLDOWN ensures gate pins are held LOW by the
     * internal pulldown until TIM1 explicitly drives them.  Without
     * this, floating pins can glitch the gate driver and cause
     * shoot-through (partial MOSFET turn-on → hardware damage). */
    gpio.Pin       = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLDOWN;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOB, &gpio);
}

/* 32-bit microsecond timestamp from the free-running TIM2 (1 MHz).
 * Safe to call from the main loop; not used from ISRs. */
uint32_t App_GetMicros(void)
{
    return __HAL_TIM_GET_COUNTER(&htim2);
}
