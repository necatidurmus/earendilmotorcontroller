/* ============================================================
 * Core/Inc/tim.h
 * TIM peripheral handles — CubeMX generated
 * ============================================================ */

#ifndef __TIM_H
#define __TIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

extern TIM_HandleTypeDef htim1;   /* advanced-control motor PWM */
extern TIM_HandleTypeDef htim2;   /* 32-bit 1 MHz micros timer  */
extern TIM_HandleTypeDef htim4;   /* Hall sensor interface      */
/* The 1 kHz scheduler tick is driven by SysTick (HAL_IncTick) on
 * the STM32F411, which has no TIM6/TIM7 (basic timers). */

void MX_TIM1_Init(void);
void MX_TIM2_Init(void);
void MX_TIM4_Init(void);

/* 32-bit microsecond timestamp from TIM2 (1 MHz free-running). */
uint32_t App_GetMicros(void);

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

#ifdef __cplusplus
}
#endif

#endif /* __TIM_H */
