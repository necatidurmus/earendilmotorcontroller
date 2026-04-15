/*
 * bldc_commutation.c — Sensörlü 6-adım aktif faz çifti sürüş komütasyonu
 *
 * Bu dosya asenkron yapıdan tamamen yeniden yazıldı.
 *
 * ÖNCEKİ YAPI (asenkron):
 *   - Yüksek taraf: TIM1_CH1/CH2/CH3 → PWM
 *   - Düşük taraf: PA7/PB0/PB1 → GPIO output, sabit HIGH/LOW
 *   - Deadtime: yazılımda, bir ISR periyodu all-off bekleme
 *   - Sorun: shoot-through riski, verimsiz geri dönüş akımı yönetimi
 *
 * YENİ YAPI (gerçek senkron komplementer):
 *   - Aktif bacakta CHx ve CHxN birlikte açılır (OCxE + OCxNE)
 *   - Kaynak faz: CCR=duty  -> high-side PWM + low-side komplementer
 *   - Sink faz:   CCR=0     -> high-side kapalı, low-side açık
 *   - Float faz:  CHx/CHxN devre dışı
 *   - Deadtime: donanım (BDTR.DTG)
 *
 * CCER Register Bit Haritası (TIM1):
 *   Bit  0: CC1E  — CH1  enable  (PA8  yüksek taraf A)
 *   Bit  2: CC1NE — CH1N enable  (PA7  düşük taraf A)
 *   Bit  4: CC2E  — CH2  enable  (PA9  yüksek taraf B)
 *   Bit  6: CC2NE — CH2N enable  (PB0  düşük taraf B)
 *   Bit  8: CC3E  — CH3  enable  (PA10 yüksek taraf C)
 *   Bit 10: CC3NE — CH3N enable  (PB1  düşük taraf C)
 *
 * Her adım için faz rolleri (ileri yön):
 *   Adım 0: Source=A, Sink=B, Float=C
 *   Adım 1: Source=A, Sink=C, Float=B
 *   Adım 2: Source=B, Sink=C, Float=A
 *   Adım 3: Source=B, Sink=A, Float=C
 *   Adım 4: Source=C, Sink=A, Float=B
 *   Adım 5: Source=C, Sink=B, Float=A
 */

#include "bldc_commutation.h"
#include "board_io.h"
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

/* Mask of all commutation channel bits — safe read-modify-write */
#define CCER_COMM_MASK (CCER_CH1E | CCER_CH1NE | CCER_CH2E | CCER_CH2NE | CCER_CH3E | CCER_CH3NE)

typedef enum {
    PHASE_A = 0,
    PHASE_B = 1,
    PHASE_C = 2
} PhaseId;

typedef volatile uint32_t* CCR_Ptr;

static CCR_Ptr const CCR_PHASE_PTR[3] = {
    &TIM1->CCR1,
    &TIM1->CCR2,
    &TIM1->CCR3,
};

static const uint32_t CCER_PHASE_MASK[3] = {
    CCER_CH1E | CCER_CH1NE,
    CCER_CH2E | CCER_CH2NE,
    CCER_CH3E | CCER_CH3NE,
};

/* İleri yön: source/sink faz eşleşmeleri */
static const uint8_t SOURCE_FWD[6] = {
    PHASE_A, PHASE_A, PHASE_B,
    PHASE_B, PHASE_C, PHASE_C,
};

static const uint8_t SINK_FWD[6] = {
    PHASE_B, PHASE_C, PHASE_C,
    PHASE_A, PHASE_A, PHASE_B,
};

/* Geri yön: source/sink terslenir */
static const uint8_t SOURCE_BWD[6] = {
    PHASE_B, PHASE_C, PHASE_C,
    PHASE_A, PHASE_A, PHASE_B,
};

static const uint8_t SINK_BWD[6] = {
    PHASE_A, PHASE_A, PHASE_B,
    PHASE_B, PHASE_C, PHASE_C,
};

static inline void setPhaseDuty(uint8_t phase, uint16_t duty) {
    *CCR_PHASE_PTR[phase] = duty;
}

static inline uint32_t buildCcerMask(uint8_t sourcePhase, uint8_t sinkPhase) {
    return CCER_PHASE_MASK[sourcePhase] | CCER_PHASE_MASK[sinkPhase];
}

/* ====================================================================
 * İç durum
 * ==================================================================== */

static uint8_t  activeState = 0xFF;
static uint16_t activeDuty  = 0;
static RunMode  activeDir   = RUN_STOPPED;

/* ====================================================================
 * API
 * ==================================================================== */

void Comm_Init(void) {
    activeDir = RUN_STOPPED;
    Comm_AllOff();
}

/*
 * Comm_ApplyStep — ISR hot path
 *
 * İşlem sırası:
 *   1. Gerekirse MOE'yi tekrar aç (clear/re-arm sonrası)
 *   2. Sektör/yön değişiminde: önce CCR'leri yeni değerlerle yaz,
 *      sonra CCER bacak haritasını güncelle (glitch-free geçiş)
 *   3. Aynı sektörde: sadece duty güncelle
 *
 * Donanım deadtime CCER geçişini korur — shoot-through riski yok.
 * CCR'ler CCER'den önce yazılır → yeni bacak aktif olduğunda
 * doğru duty zaten shadow register'da bekler.
 */
void Comm_ApplyStep(uint8_t state, uint16_t duty, RunMode dir) {
    if (state > 5 || duty == 0U || dir == RUN_STOPPED) {
        Comm_AllOff();
        return;
    }

    if (duty > PWM_DUTY_MAX) {
        duty = PWM_DUTY_MAX;
    }

    uint8_t sourcePhase;
    uint8_t sinkPhase;
    if (dir == RUN_FORWARD) {
        sourcePhase = SOURCE_FWD[state];
        sinkPhase = SINK_FWD[state];
    } else {
        /* RUN_BACKWARD — BWD tablosu source/sink'i terslenir */
        sourcePhase = SOURCE_BWD[state];
        sinkPhase = SINK_BWD[state];
    }

    uint32_t ccerValue = buildCcerMask(sourcePhase, sinkPhase);

    if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U) {
        BoardIO_RearmPWMOutputs();
    }

    if (state != activeState || dir != activeDir) {
        /*
         * Sektör/yön değişimi — güvenli geçiş sırası:
         *   1. Önce CCER'den eski bacakları kapat (cross-conduction yok)
         *   2. Tüm CCR'leri sıfırla
         *   3. Yeni source ve sink CCR'lerini doğru değerlerle yaz
         *   4. CCER'i güncelle (yeni bacaklar aktif olduğunda CCR hazır)
         */
        TIM1->CCER &= ~CCER_COMM_MASK;
        TIM1->CCR1 = 0;
        TIM1->CCR2 = 0;
        TIM1->CCR3 = 0;
        setPhaseDuty(sourcePhase, duty);
        setPhaseDuty(sinkPhase, 0U);
        TIM1->CCER = (TIM1->CCER & ~CCER_COMM_MASK) | ccerValue;
    } else {
        /* Aynı sektörde — sadece duty güncelle */
        setPhaseDuty(sourcePhase, duty);
        setPhaseDuty(sinkPhase, 0U);
    }

    activeState = state;
    activeDuty  = duty;
    activeDir   = dir;
}

void Comm_AllOff(void) {
    BoardIO_AllOff();
    activeState = 0xFF;
    activeDuty  = 0;
    activeDir   = RUN_STOPPED;
}

uint8_t  Comm_GetActiveState(void) { return activeState; }
uint16_t Comm_GetActiveDuty(void)  { return activeDuty;  }
