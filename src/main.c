/*
 * main.c — Earendil BLDC Motor Controller giriş noktası
 *
 * STM32F411 Black Pill + L6388 + 6-NMOS sensörlü 6-adım BLDC sürücüsü
 *
 * Mimari:
 *   - TIM3 ISR 12.5 kHz'de MotorControl_Tick() çağırır
 *   - Ana döngü yalnızca CLI ve LED yanıp sönmeyi işler
 *   - Arduino bağımlılığı yok — saf STM32Cube HAL
 *   - Komütasyon: senkron komplementer PWM (TIM1 CHx + CHxN)
 *
 * ISR ve ana döngü arasında paylaşılan volatile durum:
 *   - g_runMode: CLI tarafından yazılır, ISR tarafından okunur
 *   - g_commandDuty: CLI tarafından yazılır, ISR tarafından okunur
 *   - g_appliedDuty: ISR tarafından yazılır, CLI tarafından okunur
 *   - g_isrTickCount: ISR tarafından yazılır, CLI tarafından okunur
 *
 * Güvenlik:
 *   - Fault: çıkışlar hemen kapalı, duty sıfır, mod STOPPED
 *   - Geçersiz hall: çıkışlar kapalı
 *   - Hard overcurrent: latch'li fault, CLI 'clear' gerekir
 *   - Soft overcurrent: duty orantılı azaltılır
 */

#include "stm32f4xx_hal.h"
#include <string.h>

#include "motor_config.h"
#include "board_io.h"
#include "hall.h"
#include "bldc_commutation.h"
#include "protection.h"
#include "cli.h"

/* ====================================================================
 * ISR ve ana döngü arasında paylaşılan volatile durum
 * RunMode bldc_commutation.h'de tanımlı (tek kaynak)
 * ==================================================================== */

volatile RunMode   g_runMode     = RUN_STOPPED;
volatile uint16_t  g_commandDuty = DUTY_DEFAULT;
volatile uint16_t  g_appliedDuty = 0;
volatile uint32_t  g_isrTickCount = 0;


/* ====================================================================
 * MotorControl_Tick() — TIM3 ISR'dan 12.5 kHz'de çağrılır
 *
 * Motor kontrol hot path'in tamamı burada.
 * Kısa, deterministik, blocking yok, print yok.
 * ==================================================================== */

void MotorControl_Tick(void) {
    g_isrTickCount++;

    /* 1) ADC örnekleme (içeride decimation ile) */
    Prot_SampleTick();

    /* 2) Hard overcurrent kontrolü */
    if (Prot_CheckHardLimit()) {
        /* Fault latched oldu — çıkışlar zaten kapalı (Prot_LatchFault içinde) */
        g_runMode    = RUN_STOPPED;
        g_appliedDuty = 0;
        return;
    }

    /* 3) Durdurulmuş / fault / sıfır duty durumu */
    RunMode  mode    = (RunMode)g_runMode;
    uint16_t cmdDuty = g_commandDuty;

    if (mode == RUN_STOPPED || Prot_IsFaulted() || cmdDuty == 0) {
        g_appliedDuty = 0;
        Comm_AllOff();
        return;
    }

    /* 4) Hall → komütasyon durumuna çözümleme */
    uint32_t nowUs    = (uint32_t)((uint64_t)g_isrTickCount * 80U);  /* 64-bit multiply to avoid ~72 min overflow */
    uint8_t  baseState = Hall_ResolveState(nowUs);
    if (baseState > 5) {
        /* Geçersiz hall — güvenli çıkış */
        g_appliedDuty = 0;
        Comm_AllOff();
        return;
    }

    /* 5) Yumuşak akım limiti uygulaması */
    uint16_t targetDuty = Prot_ApplySoftLimit(cmdDuty);

    /* Minimum duty tabanı — çok küçük duty'de salınım olmasın */
    if (targetDuty > 0 && targetDuty < DUTY_MIN_ACTIVE) {
        targetDuty = DUTY_MIN_ACTIVE;
    }

    /* 6) Duty slew (rampa) limiti */
    g_appliedDuty = Prot_SlewDuty(g_appliedDuty, targetDuty);

    /* 7) Komütasyon adımını uygula
     *    Yön bilgisi Comm_ApplyStep'e iletilir.
     *    Geri yönde CCER tablosu yüksek/düşük tarafı ters çevirir.
     */
    uint16_t pwmDuty = DUTY_TO_PWM(g_appliedDuty);
    Comm_ApplyStep(baseState, pwmDuty, mode);
}

/* ====================================================================
 * Main entry point
 * ==================================================================== */

int main(void) {
    /* Initialize all hardware */
    BoardIO_InitAll();

    /* Initialize hall processing */
    HallConfig hallCfg = {
        .profile = 0,
        .polarityMask = 0,
        .stateOffset = 0
    };
    Hall_Init(&hallCfg);
    Hall_SetDirection(1);  /* forward by default */

    /* Initialize commutation */
    Comm_Init();

    /* Initialize protection */
    ProtectionConfig protCfg = {
        .softLimitAdc = CURRENT_SOFT_LIMIT,
        .hardLimitAdc = CURRENT_HARD_LIMIT,
        .hardStrikesToTrip = HARD_LIMIT_STRIKES
    };
    Prot_Init(&protCfg);

    /* Calibrate current sensor offset */
    {
        const char *msg = "Calibrating ISENSE...\r\n";
        HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), 200);
    }
    Prot_CalibrateOffset();

    /* Banner yazdır */
    {
        const char *banner =
            "\r\n"
            "========================================\r\n"
            " Earendil BLDC Motor Controller\r\n"
            " STM32F411 + L6388 + 6-NMOS\r\n"
            " Sensored 6-step synchronous comp. PWM\r\n"
            " Kontrol: 12.5 kHz TIM3 ISR\r\n"
            " CLI: UART2 @ 115200 (PA2/PA3)\r\n"
            "========================================\r\n"
            "\r\n";
        HAL_UART_Transmit(&huart2, (uint8_t *)banner, strlen(banner), 500);
    }

    /* Initialize CLI (prints help) */
    CLI_Init();

    /* Start control timer — motor control begins now */
    BoardIO_StartControlTimer();

    /* LED blink state */
    uint32_t lastBlinkMs = HAL_GetTick();

    /* ====================================================================
     * Main loop — only non-critical tasks
     * Motor control is entirely in TIM3 ISR.
     * ==================================================================== */
    while (1) {
        /* Service CLI */
        CLI_Service();

        /* LED blink every 400 ms */
        uint32_t nowMs = HAL_GetTick();
        if ((nowMs - lastBlinkMs) >= 400) {
            lastBlinkMs = nowMs;
            BoardIO_LEDToggle();
        }
    }
}
