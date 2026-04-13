/*
 * motor_config.h — Earendil BLDC motor controller konfigürasyonu
 *
 * STM32F411 Black Pill + L6388 + 6-NMOS sensörlü 6-adım BLDC sürücüsü
 * için tüm donanım bağımlı sabitler, ayar parametreleri ve tasarım varsayımları.
 *
 * Bölümler:
 *   0. CLI Transport seçimi  ← BU BÖLÜMÜ DEĞİŞTİR
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
 * 0. CLI Transport Seçimi
 *
 * CLI_TRANSPORT_UART : USART2 PA2(TX)/PA3(RX), 115200 baud
 *                      Herhangi bir USB-UART köprü veya ST-Link VCP ile çalışır.
 *                      PA9/PA10 TIM1 PWM ile çakışır, bu yüzden USART2 kullanılır.
 *
 * CLI_TRANSPORT_CDC  : USB Full-Speed CDC (PA11 D-, PA12 D+)
 *                      PC'de sanal seri port olarak görünür. Driver gerekmez
 *                      (Windows 10+, Linux, macOS).
 *                      UART kablosu gerekmez — doğrudan USB ile bağlan.
 *
 *                      ÖNEMLİ: CDC seçildiğinde SYSCLK 100→96 MHz olur.
 *                      USB için VCO/PLLQ = 48 MHz şartı var.
 *                      PLLN: 200→192, PLLQ=4 → 192/4 = 48 MHz USB ✓
 *                      Tüm timer hesapları bu saate göre güncellendi.
 *
 * Değiştirmek için sadece bu satırı düzenle:
 * ==================================================================== */
#define CLI_TRANSPORT_UART   0
#define CLI_TRANSPORT_CDC    1

#define CLI_TRANSPORT        CLI_TRANSPORT_UART   /* ← buraya CDC veya UART yaz */

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

/* Low-side complementary outputs — TIM1 CHxN (AF1) */
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
 *   HSE = 25 MHz -> PLL -> SYSCLK = 96 MHz
 *   APB1 = 48 MHz, APB1 timer saati = 96 MHz (x2, çünkü APB1 bölücü != 1)
 *   APB2 = 96 MHz, APB2 timer saati = 96 MHz (x1, çünkü APB2 prescaler=1)
 *
 * PWM timer: TIM1 (gelişmiş timer, APB2 timer saati = 96 MHz)
 *   Ön bölücü = 0  -> 96 MHz tick
 *   Periyot = 3199  -> 96 MHz / 3200 = 30 kHz PWM
 *   Çözünürlük: ~10 bit (0..3199 duty aralığı)
 *   Mod: Complementary (CH1/CH2/CH3 + CH1N/CH2N/CH3N)
 *
 * Kontrol timer: TIM3 (genel amaçlı, APB1 timer saati = 96 MHz)
 *   Ön bölücü = 95 -> 96 MHz / 96 = 1 MHz tick (1 us çözünürlük)
 *   Periyot = 79   -> 1 MHz / 80 = 12.5 kHz ISR
 *
 * TIM1 Deadtime:
 *   96 MHz tick, 1 tick = ~10.4 ns
 *   DEADTIME_COUNTS = 50 -> ~521 ns yazılımdan deadtime
 *   L6388 ayrıca ~300-400 ns dahili deadtime ekler
 *   Toplam efektif = ~820-920 ns — bench'te osiloskopla doğrulan
 */

/* PWM konfigürasyonu */
#define PWM_TIMER_HANDLE    htim1
#define PWM_FREQ_HZ         30000U

/*
 * Deadtime sayacı: TIM1 BDTR DTG alanına yazılır.
 * DTG[7:0] bit7=0 → deadtime = DTG × tdts
 *
 * SYSCLK=96 MHz ile TIM1 saati:
 *   APB2 prescaler=1 → TIM1 clock = APB2 = 96 MHz → tdts = 10.4 ns
 *   DEADTIME_COUNTS=50 → 50 × 10.4 ns ≈ 521 ns MCU-tarafı deadtime
 *   L6388 iç DT ~300-400 ns → toplam ~820-920 ns
 * [AYAR] — güvenli başlangıç. Osiloskopla doğrula.
 */
#define DEADTIME_COUNTS     50U

/*
 * PWM frekansı (SYSCLK=96 MHz):
 *   APB2 prescaler=1 → TIM1 clock = 96 MHz (x1)
 *   PSC=0 → sayım frekansı = 96 MHz
 *   ARR=3199 → PWM = 96 MHz / 3200 = 30 kHz ✓
 */
#define PWM_PERIOD_COUNTS   3199U       /* 96 MHz / 3200 = 30 kHz */
#define PWM_DUTY_MAX        3199U       /* duty aralığı 0..3199 */

/* Kontrol döngüsü timer — TIM3 */
#define CTRL_TIMER_HANDLE    htim3
#define CTRL_TIMER           TIM3
/*
 * TIM3 (SYSCLK=96 MHz):
 *   APB1 prescaler=2 → APB1=48 MHz → TIM3 clock = 48×2 = 96 MHz
 *   PSC=95 → tick = 96/(95+1) = 1 MHz (1 µs)
 *   ARR=79 → ISR = 1 MHz / 80 = 12.5 kHz ✓
 */
#define CTRL_TIMER_PRESCALER  95U       /* 96 MHz / 96 = 1 MHz */
#define CTRL_TIMER_PERIOD     79U       /* 1 MHz / 80 = 12.5 kHz */
#define CTRL_TICK_HZ          12500U

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
 *   I_motor -> R_shunt (0.5 mΩ) -> INA181A1QDBVRQ1 (gain=20 V/V) -> PA0 ADC
 *
 * ADC: 12-bit, Vref = 3.3 V
 *   LSB = 3.3 / 4095 = 0.806 mV
 *
 * Ekran dönüşümü ayarlanabilir kazanç üzerinden yapılır.
 * INA181 varyantları: A1=20, A2=50, A3=100, A4=200 V/V
 * [TASARIM] INA181A1QDBVRQ1 — A1 varyantı, kazanç=20 V/V. Bench'te doğrulanmalı.
 *
 * UYARI: Koruma kararları daima ham ADC delta üzerinden verilir.
 * estimatedAmps sadece gösterimdedir, koruma için kullanılmamalıdır.
 */
#define ADC_VREF             3.3f
#define ADC_MAX_COUNTS       4095.0f
#define SHUNT_OHMS           0.0005f    /* [TASARIM] 0.5 mΩ, 2512 paket */
#define INA_GAIN_DEFAULT     20.0f      /* INA181A1QDBVRQ1 — A1 varyantı, 20 V/V gain */

/* ADC örnekleme: ISR yükünü azaltmak için seyreltilmiş */
#define ADC_DECIMATION       4          /* her 4. ISR tick'inde örnekle = 3125 Hz */
#define CURRENT_FILTER_ALPHA 0.20f      /* EMA alçak geçiren katsayı [AYAR] */
#define ADC_CALIBRATION_SAMPLES 128     /* ofset kalibrasyonu örnek sayısı */

/* Voltaj ölçüm bölücü: devre şemasından [TASARIM] — bench'te doğrulanmalı */
#define VSENSE_DIVIDER_RATIO 0.04472f   /* R_top=47k, R_bot=2.2k -> 2.2/(47+2.2) */
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

/*
 * UART transport sabitleri (CLI_TRANSPORT == CLI_TRANSPORT_UART iken geçerli)
 * CDC seçildiğinde bu değerler devre dışıdır.
 */
#define CLI_UART_HANDLE      huart2  /* USART2 — PA2(TX)/PA3(RX) */

/*
 * USB CDC transport sabitleri (CLI_TRANSPORT == CLI_TRANSPORT_CDC iken geçerli)
 * CDC RX tampon boyutu: circular buffer, USB paket boyutunun katı olmalı
 */
#define USB_CDC_RX_BUF_SIZE  256U    /* circular RX tampon boyutu (2^n olsun) */

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

extern TIM_HandleTypeDef  htim1;    /* PWM timer */
extern TIM_HandleTypeDef  htim3;    /* control timer */
extern ADC_HandleTypeDef  hadc1;    /* current/voltage sense */
extern UART_HandleTypeDef huart2;   /* CLI UART — USART2 PA2/PA3 (sadece UART modda) */

#if CLI_TRANSPORT == CLI_TRANSPORT_CDC
/* USB CDC modda tanımlı — usb_device.c içinde */
#include "stm32f4xx_hal_pcd.h"
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
#endif

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_CONFIG_H */
