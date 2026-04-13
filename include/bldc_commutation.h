/*
 * bldc_commutation.h — Sensörlü 6-adım senkron komplementer PWM komütasyonu
 *
 * Mimari:
 *   - Her komütasyon adımında source + sink olmak üzere 2 faz aktif (6 adım)
 *   - Aktif bacakta CHx + CHxN birlikte aktif (OCxE + OCxNE)
 *   - Source fazda CCR=duty, sink fazda CCR=0, üçüncü faz devre dışı
 *   - Pasif fazlar: CCER'da devre dışı → idle state=0 → L6388 INH=INL=0
 *
 * Senkron komplementer:
 *   Aynı kanalın CHx/CHxN çifti timer tarafından komplementer sürülür.
 *   Donanım deadtime (BDTR.DTG) shoot-through'yu önler.
 *
 * Her adım için switch durumları (ileri yön, durum 0..5):
 *
 *   Adım | Hall(CBA) | Yük.Taraf | Düş.Taraf
 *   -----|-----------|-----------|----------
 *     0  |    001    |    A(CH1) |    B(CH2N)
 *     1  |    011    |    A(CH1) |    C(CH3N)
 *     2  |    010    |    B(CH2) |    C(CH3N)
 *     3  |    110    |    B(CH2) |    A(CH1N)
 *     4  |    100    |    C(CH3) |    A(CH1N)
 *     5  |    101    |    C(CH3) |    B(CH2N)
 *
 *   Geri yön: yüksek ve düşük taraf yer değiştirir (B→A yerine A→B)
 */

#ifndef BLDC_COMMUTATION_H
#define BLDC_COMMUTATION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Çalışma modu (main.c ve cli.c ile paylaşılır) */
typedef enum {
    RUN_STOPPED  = 0,
    RUN_FORWARD  = 1,
    RUN_BACKWARD = 2
} RunMode;

/* Komütasyon modülünü başlat (tüm çıkışlar kapalı) */
void Comm_Init(void);

/*
 * Bir komütasyon adımı uygula.
 * state: 0..5 (hall → state dönüşümünden gelir)
 * duty:  0..PWM_PERIOD_COUNTS (hesaplanmış efektif duty)
 * dir:   RUN_FORWARD veya RUN_BACKWARD
 *
 * ISR hot path — HAL çağrısı yok, doğrudan register erişimi.
 */
void Comm_ApplyStep(uint8_t state, uint16_t duty, RunMode dir);

/* Tüm çıkışları kapat — CCR=0, CCER=0 */
void Comm_AllOff(void);

/* Tanı için mevcut durum sorgu */
uint8_t  Comm_GetActiveState(void);
uint16_t Comm_GetActiveDuty(void);

#ifdef __cplusplus
}
#endif

#endif /* BLDC_COMMUTATION_H */
