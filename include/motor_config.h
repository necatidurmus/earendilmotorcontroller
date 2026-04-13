/*
 * motor_config.h — Earendil BLDC motor controller konfigürasyonu
 *
 * STM32F411 Black Pill + L6388 + 6-NMOS sensörlü 6-adım BLDC sürücüsü
 * için tüm donanım bağımlı sabitler, ayar parametreleri ve tasarım varsayımları.
 *
 * Bölümler:
 *   1. Kart pin haritası (çalışan firmware'den doğrulanmış)
 *   2. Timer ve PWM ayarları
 *   3. Hall sensör işleme
 *   4. ADC ve akım ölçümü
 *   5. Görev döngüsü ve rampa
 *   6. Koruma eşikleri
 *   7. Fault ve timeout
 *   8. CLI / seri iletişim
 *   9. Hall harita profilleri
 *
 * [KALIBRASYON]  = Gerçek donanımda ölçüldü
 * [TASARIM]      = Teorik / şematikten
 * [AYAR]         = Bench testinde kalibre edilmesi gereken başlangıç değeri
 * [BİLİNMİYOR]   = Donanım belirsizliği, bench'te doğrulanmalı
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
 * 2. Timer ve PWM ayarları — [TASARIM]
 * ==================================================================== */

/*
 * STM32F411 saat ağacı (board_io.c'de yapılandırılmış):
 *   HSE = 25 MHz -> PLL -> SYSCLK = 100 MHz
 *   APB1 = 50 MHz, APB1 timer saati = 100 MHz (x2, çünkü APB1 bölücü != 1)
 *   APB2 = 100 MHz, APB2 timer saati = 200 MHz (x2, çünkü APB2 bölücü != 1)
 *
 * PWM timer: TIM1 (gelişmiş timer, APB2 timer saati = 200 MHz)
 *   Ön bölücü = 1  -> 200 MHz / 2 = 100 MHz tick
 *   Periyot = 3332  -> 100 MHz / 3333 = ~30 kHz PWM
 *   Çözünürlük: ~10 bit (0..3332 duty aralığı)
 *   Mod: Complementary (CH1/CH2/CH3 + CH1N/CH2N/CH3N)
 *
 * Kontrol timer: TIM3 (genel amaçlı, APB1 timer saati = 100 MHz)
 *   Ön bölücü = 99 -> 100 MHz / 100 = 1 MHz tick (1 us çözünürlük)
 *   Periyot = 79   -> 1 MHz / 80 = 12.5 kHz ISR
 *
 * TIM1 Deadtime:
 *   100 MHz tick, 1 tick = 10 ns
 *   DEADTIME_COUNTS = 50 -> 500 ns yazılımdan deadtime
 *   L6388 ayrıca ~300-400 ns dahili deadtime ekler
 *   Toplam efektif = ~800-900 ns — bench'te osiloskopla doğrulan
 */

/* PWM konfigürasyonu */
#define PWM_TIMER_HANDLE    htim1
#define PWM_FREQ_HZ         30000U
#define PWM_PERIOD_COUNTS   3332U       /* 100 MHz / 2 / 3333 = ~30 kHz */
#define PWM_DUTY_MAX        3332U       /* tam açık = periyot */

/*
 * Deadtime sayacı: TIM1 BDTR DTG alanına yazılır.
 * DTG[7:0] bit7=0 → deadtime = DTG × tdts (tdts = 1/100MHz = 10ns)
 * 50 → 500 ns MCU-tarafı deadtime
 * [AYAR] — güvenli başlangıç noktası. L6388 iç DT ile ~900 ns toplam.
 */
#define DEADTIME_COUNTS     50U

/* Kontrol döngüsü timer — TIM3 */
#define CTRL_TIMER_HANDLE   htim3
#define CTRL_TIMER          TIM3
#define CTRL_TIMER_PRESCALER 99U        /* 100 MHz / 100 = 1 MHz */
#define CTRL_TIMER_PERIOD   79U         /* 1 MHz / 80 = 12.5 kHz */
#define CTRL_TICK_HZ        12500U      /* [AYAR] kontrol ISR frekansı */

/* ====================================================================
 * 3. Hall sensör işleme — [AYAR]
 * ==================================================================== */

#define HALL_OVERSAMPLE          7      /* örnekleme başına okuma, çoğunluk oyu */
#define MIN_STATE_INTERVAL_US   40U    /* debounce: durum değişimleri arası min süre */
#define INVALID_HALL_HOLD_US  1500U    /* hall geçersizse son geçerli durumu tut */

/* ====================================================================
 * 4. ADC ve akım ölçümü — [TASARIM] (INA181 kazancı bench'te doğrulanmalı)
 * ==================================================================== */

/*
 * Akım ölçüm zinciri:
 *   I_motor -> R_shunt (0.5 mΩ) -> INA181 (kazanç bilinmiyor) -> PA0 ADC
 *
 * ADC: 12-bit, Vref = 3.3 V
 *   LSB = 3.3 / 4095 = 0.806 mV
 *
 * Ekran dönüşümü ayarlanabilir kazanç üzerinden yapılır.
 * INA181 varyantları: A1=20, A2=50, A3=100, A4=200 V/V
 * [BİLİNMİYOR] — bench'te PCB üzerindeki suffix okunmalı.
 *
 * UYARI: Koruma kararları daima ham ADC delta üzerinden verilir.
 * estimatedAmps sadece gösterimdedir, koruma için kullanılmamalıdır.
 */
#define ADC_VREF             3.3f
#define ADC_MAX_COUNTS       4095.0f
#define SHUNT_OHMS           0.0005f    /* [TASARIM] 0.5 mΩ, 2512 paket */
#define INA_GAIN_DEFAULT     50.0f      /* [BİLİNMİYOR] varsayılan tahmin = A2 varyantı */

/* ADC örnekleme: ISR yükünü azaltmak için seyreltilmiş */
#define ADC_DECIMATION       4          /* her 4. ISR tick'inde örnekle = 3125 Hz */
#define CURRENT_FILTER_ALPHA 0.20f      /* EMA alçak geçiren katsayı [AYAR] */
#define ADC_CALIBRATION_SAMPLES 128     /* ofset kalibrasyonu örnek sayısı */

/* Voltaj ölçüm bölücü: devre şemasından [TASARIM] — bench'te doğrulanmalı */
#define VSENSE_DIVIDER_RATIO 0.04472f   /* R12=47k, R13=2.2k -> 2.2/(47+2.2) */
#define VSENSE_VREF          3.3f

/* ====================================================================
 * 5. Görev döngüsü ve rampa — [AYAR]
 * ==================================================================== */

#define DUTY_DEFAULT         70U        /* başlangıç komut değeri (0..255) */
#define DUTY_MIN_ACTIVE      8U         /* MOSFET anahtarlamasını sürdürmek için min */
#define DUTY_RAMP_UP_STEP    2U         /* tick başına artış adımı */
#define DUTY_RAMP_DOWN_STEP  4U         /* tick başına azalış adımı */

/* Duty'yi 0..255'ten 0..PWM_PERIOD_COUNTS'a çevir */
#define DUTY_TO_PWM(d)       ((uint16_t)((uint32_t)(d) * PWM_PERIOD_COUNTS / 255))

/* ====================================================================
 * 6. Koruma eşikleri — [AYAR]
 * ==================================================================== */

/*
 * Akım limitleri ADC delta count'ta (filtrelenmiş ADC - ofset).
 * 12-bit ham değerler, amper DEĞİL.
 * Gerçek zamanlı delta için 'current' CLI komutunu kullan.
 */
#define CURRENT_SOFT_LIMIT   450U       /* bu deltanın üstünde duty azalt */
#define CURRENT_HARD_LIMIT   700U       /* bu deltanın üstünde fault latched */
#define HARD_LIMIT_STRIKES   3          /* latch için gereken ardışık aşım sayısı */

/* Yumuşak limit geri çekim: backoff = 3 + (aşım / 16), max 80'de kesilir */
#define SOFT_BACKOFF_MIN     3U
#define SOFT_BACKOFF_DIVISOR 16U
#define SOFT_BACKOFF_MAX     80U

/* ====================================================================
 * 7. Fault ve timeout — [TASARIM]
 * ==================================================================== */

#define FAULT_REASON_MAX    48

/* TODO: VSENSE ölçeği doğrulandığında undervoltage eşiği ekle */
/* TODO: NTC eklenirse thermal limit ekle */

/* ====================================================================
 * 8. CLI / seri iletişim — [TASARIM]
 * ==================================================================== */

#define CLI_BAUD             115200U
#define CLI_LINE_BUF         96
#define CLI_IDLE_PARSE_MS    120U       /* newline gelmezse otomatik parse */

/* CLI USART2 PA2/PA3 kullanır (PA9/PA10 TIM1 PWM ile çakışır) */
#define CLI_UART_HANDLE      huart2

/* ====================================================================
 * 9. Hall harita profilleri — [TASARIM] (çalışan firmware'den)
 * ==================================================================== */

/*
 * Index = düzeltilmiş hall değeri (0..7)
 * Değer = komütasyon durumu 0..5, veya 255 geçersiz için
 *
 * Profil 0: mevcut motor kablolamasıyla test edildi
 * Profil 1-3: farklı hall/faz sıralamaları için alternatifler
 *
 * DİKKAT: Bu tablo hall.c'de tanımlıdır. Burada sadece extern bildirimi.
 * Birden fazla .c dosyası bu header'ı include ettiğinde multiple definition
 * hatası olmaması için static dizinin header'da bulunmaması gerekir.
 */
#define HALL_PROFILE_COUNT  4

/* Tablo hall.c'de tanımlı, burada extern bildirimi */
extern const uint8_t HALL_TO_STATE_PROFILES[HALL_PROFILE_COUNT][8];

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
