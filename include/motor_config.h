/*
 * motor_config.h — Earendil BLDC motor controller configuration
 *
 * All hardware-dependent constants, tuning parameters, and design assumptions
 * for the STM32F411 Black Pill + L6388 + 6-NMOS sensored 6-step BLDC driver.
 *
 * Sections:
 *   1. Board pin mapping (confirmed from working firmware)
 *   2. Timer and PWM settings
 *   3. Hall sensor processing
 *   4. ADC and current sensing
 *   5. Duty cycle and ramp
 *   6. Protection thresholds
 *   7. Fault and timeout
 *   8. CLI / serial
 *   9. Hall map profiles
 *
 * Values marked [CALIBRATED] were measured on real hardware.
 * Values marked [DESIGN] are theoretical / from schematic.
 * Values marked [TUNING] are starting points that need bench adjustment.
 */

#ifndef MOTOR_CONFIG_H
#define MOTOR_CONFIG_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* ====================================================================
 * 1. Board pin mapping — CONFIRMED from working Arduino firmware
 * ==================================================================== */

/* Hall sensor inputs — GPIOB */
#define HALL_A_PORT         GPIOB
#define HALL_A_PIN          GPIO_PIN_6
#define HALL_B_PORT         GPIOB
#define HALL_B_PIN          GPIO_PIN_7
#define HALL_C_PORT         GPIOB
#define HALL_C_PIN          GPIO_PIN_8

/* High-side PWM outputs — TIM1 CH1/CH2/CH3 on PA8/PA9/PA10 */
#define AH_PORT             GPIOA
#define AH_PIN              GPIO_PIN_8
#define BH_PORT             GPIOA
#define BH_PIN              GPIO_PIN_9
#define CH_PORT             GPIOA
#define CH_PIN              GPIO_PIN_10

/* Low-side ON/OFF outputs — GPIO */
#define AL_PORT             GPIOA
#define AL_PIN              GPIO_PIN_7
#define BL_PORT             GPIOB
#define BL_PIN              GPIO_PIN_0
#define CL_PORT             GPIOB
#define CL_PIN              GPIO_PIN_1

/* Analog inputs — ADC1 */
#define ISENSE_ADC_PIN      GPIO_PIN_0   /* PA0 — ADC1_IN0  */
#define ISENSE_ADC_PORT     GPIOA
#define ISENSE_ADC_CHANNEL  ADC_CHANNEL_0

#define VSENSE_ADC_PIN      GPIO_PIN_4   /* PA4 — ADC1_IN4  */
#define VSENSE_ADC_PORT     GPIOA
#define VSENSE_ADC_CHANNEL  ADC_CHANNEL_4

/* Status LED */
#define LED_PORT            GPIOC
#define LED_PIN             GPIO_PIN_13

/* ====================================================================
 * 2. Timer and PWM settings — DESIGN
 * ==================================================================== */

/*
 * STM32F411 clock tree (configured in board_io.c):
 *   HSE = 25 MHz -> PLL -> SYSCLK = 100 MHz
 *   APB1 = 50 MHz, APB1 timer clock = 100 MHz (x2)
 *   APB2 = 100 MHz, APB2 timer clock = 200 MHz (x2)
 *
 * PWM timer: TIM1 (advanced timer, APB2 timer clock = 200 MHz)
 *   Prescaler = 1  -> 200 MHz / 2 = 100 MHz tick
 *   Period = 3332  -> 100 MHz / 3333 = ~30 kHz PWM
 *   Resolution: ~10 bits (0..3332 duty range)
 *
 * Control timer: TIM3 (general-purpose, APB1 timer clock = 100 MHz)
 *   Prescaler = 99 -> 100 MHz / 100 = 1 MHz tick (1 us resolution)
 *   Period = 79    -> 1 MHz / 80 = 12.5 kHz ISR
 */

/* PWM configuration */
#define PWM_TIMER_HANDLE    htim1
#define PWM_FREQ_HZ         30000U
#define PWM_PERIOD_COUNTS   3332U       /* 100 MHz / 2 / 3333 = ~30 kHz */
#define PWM_DUTY_MAX        3332U       /* full ON = period */

/* Control loop timer — TIM3 */
#define CTRL_TIMER_HANDLE   htim3
#define CTRL_TIMER          TIM3
#define CTRL_TIMER_PRESCALER 99U        /* 100 MHz / 100 = 1 MHz */
#define CTRL_TIMER_PERIOD   79U         /* 1 MHz / 80 = 12.5 kHz */
#define CTRL_TICK_HZ        12500U      /* [TUNING] control ISR frequency */

/* ====================================================================
 * 3. Hall sensor processing — TUNING
 * ==================================================================== */

#define HALL_OVERSAMPLE     7           /* reads per sample, majority vote */
#define MIN_STATE_INTERVAL_US  40U      /* debounce: min time between state changes */
#define INVALID_HALL_HOLD_US   1500U    /* hold last valid state if hall invalid */

/* ====================================================================
 * 4. ADC and current sensing — DESIGN (verify INA181 gain on bench)
 * ==================================================================== */

/*
 * Current sense chain:
 *   I_motor -> R_shunt (0.5 mΩ) -> INA181 (gain unknown) -> PA0 ADC
 *
 * ADC: 12-bit, Vref = 3.3 V
 *   LSB = 3.3 / 4095 = 0.806 mV
 *
 * Display conversion assumes a configurable gain.
 * INA181 variants: A1=20, A2=50, A3=100, A4=200 V/V
 * [UNKNOWN] — must be confirmed on bench.
 */
#define ADC_VREF            3.3f
#define ADC_MAX_COUNTS      4095.0f
#define SHUNT_OHMS          0.0005f     /* [DESIGN] 0.5 mΩ, 2512 package */
#define INA_GAIN_DEFAULT    50.0f       /* [UNKNOWN] default guess = A2 variant */

/* ADC sampling: decimated to reduce ISR load */
#define ADC_DECIMATION      4           /* sample every 4th ISR tick = 3125 Hz */
#define CURRENT_FILTER_ALPHA 0.20f      /* EMA low-pass coefficient [TUNING] */
#define ADC_CALIBRATION_SAMPLES 128     /* offset calibration sample count */

/* Voltage sense divider: R12=47k, R13=2.2k -> ratio = 2.2/(47+2.2) = 0.04472 */
#define VSENSE_DIVIDER_RATIO 0.04472f   /* [DESIGN] verify on bench */
#define VSENSE_VREF         3.3f

/* ====================================================================
 * 5. Duty cycle and ramp — TUNING
 * ==================================================================== */

#define DUTY_DEFAULT        70U         /* initial command duty (0..255) */
#define DUTY_MIN_ACTIVE     8U          /* minimum active duty to keep MOSFETs switching */
#define DUTY_RAMP_UP_STEP   2U          /* duty counts per tick, ramp up */
#define DUTY_RAMP_DOWN_STEP 4U          /* duty counts per tick, ramp down */

/* Convert duty from 0..255 range to 0..PWM_PERIOD_COUNTS */
#define DUTY_TO_PWM(d)      ((uint16_t)((uint32_t)(d) * PWM_PERIOD_COUNTS / 255))

/* ====================================================================
 * 6. Protection thresholds — TUNING
 * ==================================================================== */

/*
 * Current limits are in ADC delta counts (filtered ADC - offset).
 * These are raw 12-bit values, NOT converted to amps.
 * Use 'current' CLI command to see real-time delta and calibrate.
 */
#define CURRENT_SOFT_LIMIT  450U        /* back off duty above this delta */
#define CURRENT_HARD_LIMIT  700U        /* latch fault above this delta */
#define HARD_LIMIT_STRIKES  3           /* consecutive strikes to trip */

/* Soft limit backoff: backoff = 3 + (over / 16), capped at 80 */
#define SOFT_BACKOFF_MIN    3U
#define SOFT_BACKOFF_DIVISOR 16U
#define SOFT_BACKOFF_MAX    80U

/* ====================================================================
 * 7. Fault and timeout — DESIGN
 * ==================================================================== */

#define FAULT_REASON_MAX    48

/* TODO: add undervoltage threshold when VSENSE scaling is validated */
/* TODO: add thermal limit if NTC is added to hardware */

/* ====================================================================
 * 8. CLI / serial — DESIGN
 * ==================================================================== */

#define CLI_BAUD            115200U
#define CLI_LINE_BUF        96
#define CLI_IDLE_PARSE_MS   120U        /* auto-parse if no newline received */

/* CLI uses USART2 on PA2/PA3 (PA9/PA10 conflict with TIM1 PWM) */
#define CLI_UART_HANDLE     huart2

/* ====================================================================
 * 9. Hall map profiles — DESIGN (from working firmware)
 * ==================================================================== */

/*
 * Index = corrected hall value (0..7)
 * Value = commutation state 0..5, or 255 for invalid
 *
 * Profile 0: tested with the current motor wiring
 * Profiles 1-3: alternatives for different hall/phase orderings
 */
#define HALL_PROFILE_COUNT  4

static const uint8_t HALL_TO_STATE_PROFILES[HALL_PROFILE_COUNT][8] = {
    {255, 0, 4, 5, 2, 1, 3, 255},
    {255, 0, 2, 1, 4, 5, 3, 255},
    {255, 4, 0, 1, 2, 3, 5, 255},
    {255, 2, 4, 3, 0, 1, 5, 255}
};

/* ====================================================================
 * External HAL handles — defined in board_io.c
 * ==================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

extern TIM_HandleTypeDef htim1;     /* PWM timer */
extern TIM_HandleTypeDef htim3;     /* control timer */
extern ADC_HandleTypeDef  hadc1;    /* current/voltage sense */
extern UART_HandleTypeDef huart2;   /* CLI serial — USART2 on PA2/PA3 */

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONFIG_H */
