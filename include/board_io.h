/*
 * board_io.h — Kart seviyesi GPIO, PWM ve ADC başlatma
 *
 * STM32Cube HAL init çağrılarını şunlar için sarar:
 *   - Sistem saati (25 MHz HSE'den 96 MHz)
 *   - GPIO pinleri (hall girişleri, LED)
 *   - TIM1 komplementer PWM çıkışları:
 *       Yüksek taraf: PA8/PA9/PA10  → TIM1_CH1/CH2/CH3  (AF1)
 *       Düşük taraf:  PA7/PB0/PB1   → TIM1_CH1N/CH2N/CH3N (AF1)
 *   - TIM3 kontrol zamanlayıcısı (12.5 kHz ISR)
 *   - ADC1 (ISENSE PA0, VSENSE PA4)
 *   - USART2 CLI seri (PA2/PA3)
 *
 * Önemli: Düşük taraf pinler artık GPIO değil, TIM1 çıkışıdır.
 * BoardIO_SetLowSide() API'si kaldırıldı. Komütasyon TIM1 CCER/CCR
 * ile doğrudan register seviyesinde yapılır.
 */

#ifndef BOARD_IO_H
#define BOARD_IO_H

#include "motor_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tüm başlatma fonksiyonlarını doğru sırada çağır */
void BoardIO_InitAll(void);

/* Alt sistem başlatma (InitAll tarafından çağrılır, test için açık) */
void BoardIO_InitClock(void);
void BoardIO_InitGPIO(void);
void BoardIO_InitPWM(void);           /* TIM1 komplementer + deadtime */
void BoardIO_InitControlTimer(void);
void BoardIO_StartControlTimer(void);
void BoardIO_InitADC(void);
void BoardIO_InitUART(void);
void BoardIO_InitWatchdog(void);
void BoardIO_KickWatchdog(void);

/* PWM duty kontrolü — duty 0..PWM_PERIOD_COUNTS aralığında */
void BoardIO_SetPWMA(uint16_t duty);
void BoardIO_SetPWMB(uint16_t duty);
void BoardIO_SetPWMC(uint16_t duty);
void BoardIO_SetAllPWM(uint16_t a, uint16_t b, uint16_t c);

/* Kısa yol: tüm çıkışlar kapalı (CCER sıfır + CCR sıfır + MOE kapalı) */
void BoardIO_AllOff(void);

/* TIM1 ana çıkış iznini (MOE) tekrar aç — clear/re-arm sonrası kullan */
void BoardIO_RearmPWMOutputs(void);

/* ADC DMA tamponundan son örneği oku — non-blocking, ISR-safe */
uint16_t BoardIO_ReadADC(uint32_t channel);

/* LED */
void BoardIO_LEDOn(void);
void BoardIO_LEDOff(void);
void BoardIO_LEDToggle(void);

/* DWT döngü sayacı ile mikrosaniye gecikme */
void BoardIO_DelayUs(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_IO_H */
