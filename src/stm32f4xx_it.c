/*
 * stm32f4xx_it.c — Interrupt handlers for Earendil BLDC controller
 *
 * Contains:
 *   - Cortex-M4 system exception handlers
 *   - SysTick handler (HAL 1 ms timebase)
 *   - TIM3 update ISR (motor control hot path)
 */

#include "stm32f4xx_hal.h"
#include "stm32f4xx_it.h"
#include "motor_config.h"

/* External HAL handles */
extern TIM_HandleTypeDef htim3;

/* Motor control tick — implemented in main.c */
extern void MotorControl_Tick(void);

/* ====================================================================
 * Cortex-M4 system exception handlers
 * ==================================================================== */

void NMI_Handler(void) {
}

void HardFault_Handler(void) {
    /* Emergency: turn off all outputs, spin forever */
    __disable_irq();
    /* Force TIM1 outputs off — CCER=0, CCR=0 */
    TIM1->CCER = 0;
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR3 = 0;
    while (1) { }
}

void MemManage_Handler(void) {
    __disable_irq();
    while (1) { }
}

void BusFault_Handler(void) {
    __disable_irq();
    while (1) { }
}

void UsageFault_Handler(void) {
    __disable_irq();
    while (1) { }
}

void SVC_Handler(void) {
}

void DebugMon_Handler(void) {
}

void PendSV_Handler(void) {
}

/* ====================================================================
 * SysTick_Handler — HAL timebase (1 kHz tick)
 * ==================================================================== */

void SysTick_Handler(void) {
    HAL_IncTick();
}

/* ====================================================================
 * TIM3_IRQHandler — 12.5 kHz motor control ISR
 *
 * Delegates to MotorControl_Tick() in main.c which has access to
 * all motor state. This file just handles the hardware interrupt flag.
 * ==================================================================== */

void TIM3_IRQHandler(void) {
    if (__HAL_TIM_GET_FLAG(&htim3, TIM_FLAG_UPDATE) != RESET) {
        if (__HAL_TIM_GET_IT_SOURCE(&htim3, TIM_IT_UPDATE) != RESET) {
            __HAL_TIM_CLEAR_FLAG(&htim3, TIM_FLAG_UPDATE);
            MotorControl_Tick();
        }
    }
}
