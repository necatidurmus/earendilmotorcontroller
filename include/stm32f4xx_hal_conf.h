/*
 * stm32f4xx_hal_conf.h — HAL module configuration for Earendil BLDC controller
 *
 * Only enables HAL modules used by this firmware.
 * Reduces compile time and binary size.
 */

#ifndef STM32F4XX_HAL_CONF_H
#define STM32F4XX_HAL_CONF_H

/* === Module Selection — only what we need === */
#define HAL_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED

/* === Oscillator Configuration === */
#if !defined(HSE_VALUE)
#define HSE_VALUE    ((uint32_t)25000000U)  /* 25 MHz HSE on Black Pill */
#endif

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT  ((uint32_t)100U)
#endif

#if !defined(HSI_VALUE)
#define HSI_VALUE    ((uint32_t)16000000U)
#endif

#if !defined(LSI_VALUE)
#define LSI_VALUE    ((uint32_t)32000U)
#endif

#if !defined(LSE_VALUE)
#define LSE_VALUE    ((uint32_t)32768U)
#endif

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT  ((uint32_t)5000U)
#endif

#if !defined(EXTERNAL_CLOCK_VALUE)
#define EXTERNAL_CLOCK_VALUE ((uint32_t)12288000U)
#endif

/* === System Configuration === */
#define VDD_VALUE               ((uint32_t)3300U)  /* 3.3V */
#define TICK_INT_PRIORITY       ((uint32_t)0U)
#define USE_RTOS                0U
#define PREFETCH_ENABLE         1U
#define PRFTBS_ENABLE           1U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE       1U

/* === Assert === */
/* #define USE_FULL_ASSERT    1U */
#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

/* === Include HAL module headers === */
#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32f4xx_hal_rcc.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32f4xx_hal_gpio.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32f4xx_hal_dma.h"
#endif

#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32f4xx_hal_cortex.h"
#endif

#ifdef HAL_ADC_MODULE_ENABLED
#include "stm32f4xx_hal_adc.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32f4xx_hal_flash.h"
#endif

#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32f4xx_hal_pwr.h"
#endif

#ifdef HAL_TIM_MODULE_ENABLED
#include "stm32f4xx_hal_tim.h"
#endif

#ifdef HAL_UART_MODULE_ENABLED
#include "stm32f4xx_hal_uart.h"
#endif

#ifdef HAL_EXTI_MODULE_ENABLED
#include "stm32f4xx_hal_exti.h"
#endif

#endif /* STM32F4XX_HAL_CONF_H */
