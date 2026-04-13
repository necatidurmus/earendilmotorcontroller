/*
 * bldc_commutation.c — Sensörlü 6-adım senkron komplementer PWM komütasyonu
 *
 * Bu dosya asenkron yapıdan tamamen yeniden yazıldı.
 *
 * ÖNCEKİ YAPI (asenkron):
 *   - Yüksek taraf: TIM1_CH1/CH2/CH3 → PWM
 *   - Düşük taraf: PA7/PB0/PB1 → GPIO output, sabit HIGH/LOW
 *   - Deadtime: yazılımda, bir ISR periyodu all-off bekleme
 *   - Sorun: shoot-through riski, verimsiz geri dönüş akımı yönetimi
 *
 * YENİ YAPI (senkron komplementer):
 *   - Yüksek taraf: TIM1_CH1/CH2/CH3 → PWM sinyali
 *   - Düşük taraf: TIM1_CH1N/CH2N/CH3N → komplementer PWM (otomatik ters)
 *   - Deadtime: donanım (BDTR.DTG = 50 → 500 ns)
 *   - CCER ile hangi çiftin aktif olduğu kontrol edilir
 *   - Pasif fazda CHx ve CHxN her ikisi devre dışı → güvenli idle
 *
 * CCER Register Bit Haritası (TIM1):
 *   Bit  0: CC1E  — CH1  enable  (PA8  yüksek taraf A)
 *   Bit  2: CC1NE — CH1N enable  (PA7  düşük taraf A)
 *   Bit  4: CC2E  — CH2  enable  (PA9  yüksek taraf B)
 *   Bit  6: CC2NE — CH2N enable  (PB0  düşük taraf B)
 *   Bit  8: CC3E  — CH3  enable  (PA10 yüksek taraf C)
 *   Bit 10: CC3NE — CH3N enable  (PB1  düşük taraf C)
 *
 * Her adım için switch durumları (ileri yön):
 *   Adım 0: A↑ B↓ → CH1E | CH2NE
 *   Adım 1: A↑ C↓ → CH1E | CH3NE
 *   Adım 2: B↑ C↓ → CH2E | CH3NE
 *   Adım 3: B↑ A↓ → CH2E | CH1NE
 *   Adım 4: C↑ A↓ → CH3E | CH1NE
 *   Adım 5: C↑ B↓ → CH3E | CH2NE
 */

#include "bldc_commutation.h"
#include "motor_config.h"
#include "stm32f4xx.h"

/* ====================================================================
 * CCER kanal enable bit maskeleri
 * ==================================================================== */

#define CCER_CH1E   (1U << 0)
#define CCER_CH1NE  (1U << 2)
#define CCER_CH2E   (1U << 4)
#define CCER_CH2NE  (1U << 6)
#define CCER_CH3E   (1U << 8)
#define CCER_CH3NE  (1U << 10)

/*
 * Her komütasyon adımı için precomputed CCER değerleri.
 * ISR içinde lookup tablosu olarak kullanılır — dal yok, hesaplama yok.
 */
static const uint32_t CCER_FWD[6] = {
    CCER_CH1E | CCER_CH2NE,  /* Adım 0: A yük. B düş. */
    CCER_CH1E | CCER_CH3NE,  /* Adım 1: A yük. C düş. */
    CCER_CH2E | CCER_CH3NE,  /* Adım 2: B yük. C düş. */
    CCER_CH2E | CCER_CH1NE,  /* Adım 3: B yük. A düş. */
    CCER_CH3E | CCER_CH1NE,  /* Adım 4: C yük. A düş. */
    CCER_CH3E | CCER_CH2NE,  /* Adım 5: C yük. B düş. */
};

/* Geri yön: yüksek ve düşük taraf yer değiştirir */
static const uint32_t CCER_BWD[6] = {
    CCER_CH2E | CCER_CH1NE,  /* Adım 0 geri: B yük. A düş. */
    CCER_CH3E | CCER_CH1NE,  /* Adım 1 geri: C yük. A düş. */
    CCER_CH3E | CCER_CH2NE,  /* Adım 2 geri: C yük. B düş. */
    CCER_CH1E | CCER_CH2NE,  /* Adım 3 geri: A yük. B düş. */
    CCER_CH1E | CCER_CH3NE,  /* Adım 4 geri: A yük. C düş. */
    CCER_CH2E | CCER_CH3NE,  /* Adım 5 geri: B yük. C düş. */
};

/*
 * Her adım için hangi CCR'a duty yazılacağı.
 * Senkron komplementer'de yalnızca yüksek taraf CCR'ı duty belirler.
 * CHxN otomatik komplementer olarak düşük tarafı sürer.
 */
typedef volatile uint32_t* CCR_Ptr;

static CCR_Ptr const CCR_FWD_PTR[6] = {
    &TIM1->CCR1, &TIM1->CCR1,
    &TIM1->CCR2, &TIM1->CCR2,
    &TIM1->CCR3, &TIM1->CCR3,
};

static CCR_Ptr const CCR_BWD_PTR[6] = {
    &TIM1->CCR2, &TIM1->CCR3,
    &TIM1->CCR3, &TIM1->CCR1,
    &TIM1->CCR1, &TIM1->CCR2,
};

/* ====================================================================
 * İç durum
 * ==================================================================== */

static uint8_t  activeState = 0xFF;
static uint16_t activeDuty  = 0;

/* ====================================================================
 * API
 * ==================================================================== */

void Comm_Init(void) {
    Comm_AllOff();
}

/*
 * Comm_ApplyStep — ISR hot path
 *
 * İşlem sırası:
 *   1. Tüm CCR'ları sıfırla (önceki adımın duty kalıntısını temizle)
 *   2. Aktif yüksek tarafa duty yaz
 *   3. CCER'ı güncelle
 *
 * Donanım deadtime CCER geçişini korur — her CCR/CCER sırası güvenli.
 * Atomik değil ama deadtime (500ns) tüm glitch'leri maskeler.
 */
void Comm_ApplyStep(uint8_t state, uint16_t duty, RunMode dir) {
    if (state > 5) {
        Comm_AllOff();
        return;
    }

    /* Önceki adımın duty kalıntılarını temizle */
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR3 = 0;

    if (dir == RUN_FORWARD) {
        *CCR_FWD_PTR[state] = duty;
        TIM1->CCER = CCER_FWD[state];
    } else {
        *CCR_BWD_PTR[state] = duty;
        TIM1->CCER = CCER_BWD[state];
    }

    activeState = state;
    activeDuty  = duty;
}

void Comm_AllOff(void) {
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR3 = 0;
    TIM1->CCER = 0;
    activeState = 0xFF;
    activeDuty  = 0;
}

uint8_t  Comm_GetActiveState(void) { return activeState; }
uint16_t Comm_GetActiveDuty(void)  { return activeDuty;  }
