/*
 * board_io.h — Board-level GPIO, PWM, and ADC initialization
 *
 * Wraps STM32Cube HAL init calls for:
 *   - System clock (100 MHz from 25 MHz HSE)
 *   - GPIO pins (hall inputs, low-side outputs, LED)
 *   - TIM1 PWM outputs (high-side PA8/PA9/PA10)
 *   - TIM3 control timer (12.5 kHz ISR)
 *   - ADC1 (ISENSE PA0, VSENSE PA4)
 *   - USART1 CLI serial
 */

#ifndef BOARD_IO_H
#define BOARD_IO_H

#include "motor_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call all init functions in correct order */
void BoardIO_InitAll(void);

/* Individual subsystem init (called by InitAll, exposed for testing) */
void BoardIO_InitClock(void);
void BoardIO_InitGPIO(void);
void BoardIO_InitPWM(void);
void BoardIO_InitControlTimer(void);
void BoardIO_StartControlTimer(void);
void BoardIO_InitADC(void);
void BoardIO_InitUART(void);

/* PWM duty control — duty is in 0..PWM_PERIOD_COUNTS range */
void BoardIO_SetPWMA(uint16_t duty);
void BoardIO_SetPWMB(uint16_t duty);
void BoardIO_SetPWMC(uint16_t duty);
void BoardIO_SetAllPWM(uint16_t a, uint16_t b, uint16_t c);

/* Low-side GPIO control */
void BoardIO_SetLowSide(int al, int bl, int cl);

/* Shorthand: all outputs off */
void BoardIO_AllOff(void);

/* ADC blocking read */
uint16_t BoardIO_ReadADC(uint32_t channel);

/* LED toggle */
void BoardIO_LEDOn(void);
void BoardIO_LEDOff(void);
void BoardIO_LEDToggle(void);

/* Microsecond delay using DWT cycle counter */
void BoardIO_DelayUs(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_IO_H */
