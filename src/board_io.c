/*
 * board_io.c — Earendil BLDC kontrol kartı için donanım başlatma
 *
 * STM32F411 Black Pill + L6388 + 6-NMOS köprü
 * Sadece STM32Cube HAL kullanır — Arduino bağımlılığı yok.
 *
 * Pin haritası:
 *   Hall:      PB6 (A), PB7 (B), PB8 (C) — TIM4_CH1/CH2/CH3 (AF2)
 *   Yük. taraf: PA8/PA9/PA10  — TIM1_CH1/CH2/CH3  (AF1) — L6388 INH
 *   Düş. taraf: PA7/PB0/PB1   — TIM1_CH1N/CH2N/CH3N (AF1) — L6388 INL
 *   ISENSE:    PA0 — ADC1_IN0
 *   VSENSE:    PA4 — ADC1_IN4
 *   CLI:       PA2 (TX) / PA3 (RX) — USART2 (AF7)
 *   LED:       PC13 — aktif düşük
 *
 * TIM1 komplementer PWM mimarisi:
 *   L6388 gate driver'ı ayrı INH (yüksek taraf) ve INL (düşük taraf) girişlerine
 *   sahiptir. TIM1_CHx → INH, TIM1_CHxN → INL. Hardware deadtime (BDTR.DTG)
 *   shoot-through'yu önler. Düşük taraf artık GPIO DEĞİL, timer çıkışıdır.
 */

#include "board_io.h"
#include <string.h>

/* ====================================================================
 * HAL handle tanımları — motor_config.h'de extern olarak bildirilmiş
 * ==================================================================== */

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;
ADC_HandleTypeDef hadc1;
UART_HandleTypeDef huart2;
static DMA_HandleTypeDef hdma_adc1;
static IWDG_HandleTypeDef hiwdg;
static volatile uint16_t adcDmaBuf[2] = {0U, 0U};

/* ====================================================================
 * DWT mikrosaniye zamanlayıcısı
 * ==================================================================== */

static int dwt_initialized = 0;

static void DWT_Init(void) {
    if (dwt_initialized) return;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    dwt_initialized = 1;
}

/* ====================================================================
 * Sistem Saati — 25 MHz HSE'den 96 MHz
 *
 * Neden 96 MHz (100 değil)?
 *   Saat ağacında VCO=192 MHz hedeflenir.
 *   SYSCLK = VCO/PLLP = 192/2 = 96 MHz.
 *
 *   PLLN=192 ile VCO=192 MHz seçilidir.
 *
 * HSE=25MHz, M=25, N=192, P=2 → SYSCLK=96 MHz
 * APB1 bölücü=2 → APB1=48 MHz, timer saati=96 MHz (x2)
 * APB2 bölücü=1 → APB2=96 MHz, timer saati=96 MHz (prescaler=1, x1)
 *
 * TIM1 (APB2): 96 MHz (APB2 prescaler=1 → x1 çarpanı)
 * TIM3 (APB1): 96 MHz (APB1 prescaler=2 → x2 çarpanı)
 * TIM4 (APB1): 96 MHz (APB1 prescaler=2 → x2 çarpanı)
 * ==================================================================== */

void BoardIO_InitClock(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* Black Pill HSE = 25 MHz */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 25;   /* VCO giriş = 1 MHz */
    RCC_OscInitStruct.PLL.PLLN       = 192;  /* VCO = 192 MHz */
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;  /* SYSCLK = 96 MHz */
    RCC_OscInitStruct.PLL.PLLQ       = 4;    /* PLLQ = 4 */

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        /* Saat hatası — LED yanıp söndür, takılı kal */
        while (1) { BoardIO_LEDToggle(); for (volatile int i = 0; i < 500000; i++); }
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;   /* HCLK = 96 MHz */
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;     /* APB1 = 48 MHz */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;     /* APB2 = 96 MHz */

    /* 96 MHz → FLASH_LATENCY_3 yeterli (80-100 MHz arası) */
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK) {
        while (1) { BoardIO_LEDToggle(); for (volatile int i = 0; i < 500000; i++); }
    }

    DWT_Init();
}

/* ====================================================================
 * GPIO Başlatma
 *
 * NOT: PA7/PB0/PB1 artık GPIO output DEĞİL. Bu pinler TIM1_CH1N/CH2N/CH3N
 * olarak BoardIO_InitPWM() içinde yapılandırılır.
 * ==================================================================== */

void BoardIO_InitGPIO(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* LED — PC13, push-pull, başlangıçta kapalı (aktif düşük) */
    gpio.Pin   = LED_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &gpio);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET); /* SET = LED kapalı */

    /* Hall pinleri başlangıçta girişe çekilir; TIM4 init içinde AF2 yapılır */
    gpio.Pin   = HALL_A_PIN | HALL_B_PIN | HALL_C_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(HALL_A_PORT, &gpio); /* Üçü de GPIOB'de */
}

/* ====================================================================
 * TIM1 Komplementer PWM — 6 kanal (CH1/CH2/CH3 + CH1N/CH2N/CH3N)
 *
 * Yüksek taraf:  PA8(CH1), PA9(CH2), PA10(CH3)  — AF1 — L6388 INH
 * Düşük taraf:   PA7(CH1N), PB0(CH2N), PB1(CH3N) — AF1 — L6388 INL
 *
 * TIM1 gelişmiş timer özellikleri:
 *   - Komplementer çıkışlar (CH ve CHN birbirine göre terslenmiş + deadtime)
 *   - Hardware deadtime insertion (BDTR.DTG ayarı)
 *   - OSSR=1: timer çalışırken çıkışlar idle state'i korur (güvenli)
 *   - MOE (Main Output Enable) aktif et
 *
 * APB2 timer saati = 96 MHz (APB2 prescaler=1 → x1, TIM clock = APB2)
 * Ön bölücü = 0 (PSC=0 → sayım = 96 MHz, tdts ≈ 10.4 ns)
 * Periyot = 3199 → 96 MHz / 3200 = 30 kHz PWM
 *
 * Deadtime hesabı:
 *   DTG[7:0] bit7=0: deadtime = DTG × tdts
 *   DEADTIME_COUNTS=50 → 50 × 10.4 ns ≈ 521 ns MCU tarafı deadtime
 *   L6388 ayrıca ~300-400 ns iç deadtime ekler
 *   Toplam efektif deadtime: ~820-920 ns (bench'te doğrula)
 * ==================================================================== */

void BoardIO_InitPWM(void) {
    __HAL_RCC_TIM1_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;

    /* Yüksek taraf pinleri: PA8(CH1), PA9(CH2), PA10(CH3) */
    gpio.Pin = AH_PIN | BH_PIN | CH_PIN;  /* GPIO_PIN_8 | 9 | 10 */
    HAL_GPIO_Init(GPIOA, &gpio);

    /* Düşük taraf pinleri: PA7(CH1N) */
    gpio.Pin = AL_PIN;  /* PA7 = GPIO_PIN_7 */
    HAL_GPIO_Init(AL_PORT, &gpio);

    /* Düşük taraf pinleri: PB0(CH2N), PB1(CH3N) */
    gpio.Pin = BL_PIN | CL_PIN;  /* PB0 | PB1 */
    HAL_GPIO_Init(BL_PORT, &gpio);

#if TIM1_BREAK_ENABLE
    /* TIM1 BKIN (örn. PB12) — donanımsal hızlı kapatma girişi */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = TIM1_BREAK_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = (TIM1_BREAK_ACTIVE_HIGH != 0U) ? GPIO_PULLDOWN : GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = TIM1_BREAK_AF;
    HAL_GPIO_Init(TIM1_BREAK_PORT, &gpio);
#endif

    /* TIM1 zaman tabanı */
    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0;                 /* PSC=0 → sayım=TIM1 clock=96 MHz */
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = PWM_PERIOD_COUNTS; /* 96 MHz / 3200 = 30 kHz */
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        while (1);
    }

    /*
     * PWM kanal yapılandırması.
     * OCPreload aktif: CCR değişimleri bir sonraki update event'inde geçerli olur.
     * Bu, olası CCR/CCER geçiş anlık tutarsızlıklarını önler.
     */
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode       = TIM_OCMODE_PWM1;      /* CNT < CCR → HIGH */
    oc.Pulse        = 0;                    /* başlangıçta 0% duty */
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH; /* CHN de aynı polarite (komplementer ters) */
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;  /* idle'da CH = 0 (yüksek taraf MOSFET kapalı) */
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET; /* idle'da CHN = 0 (düşük taraf MOSFET kapalı) */

    HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1);  /* PA8 — AH */
    HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2);  /* PA9 — BH */
    HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_3);  /* PA10 — CH */

    /*
     * Break ve Deadtime Register (BDTR) yapılandırması.
     *
     * OSSR=1 (Off-State Run Mode):
     *   Timer çalışırken devre dışı bırakılan bir çıkış idle state'ine gider
     *   (OCxIdleState=RESET → pin LOW → MOSFET kapalı). Motor sürülürken
     *   float fazın güvenli şekilde 0'da kalmasını sağlar.
     *
     * DeadTime=DEADTIME_COUNTS:
     *   Hardware deadtime. CH HIGH'a geçerken CHN'nin önce kapanmasını bekler
     *   ve tersinde de aynı. Shoot-through'ya karşı primer donanım koruması.
     *
     * BreakState: TIM1_BREAK_ENABLE ile koşullu açılır.
     *   BKIN aktifken donanım çıkışları hızlıca kapatır.
     *
     * AutomaticOutput=DISABLE: MOE biti yazılımdan kontrol edilir,
     *   break sonrası otomatik açılmaz.
     */
    TIM_BreakDeadTimeConfigTypeDef bdtr = {0};
    bdtr.OffStateRunMode  = TIM_OSSR_ENABLE;          /* çalışırken idle geçerli */
    bdtr.OffStateIDLEMode = TIM_OSSI_ENABLE;          /* dururken de idle geçerli */
    bdtr.LockLevel        = TIM_LOCKLEVEL_OFF;
    bdtr.DeadTime         = DEADTIME_COUNTS;          /* 20 → ~208 ns MCU-tarafı */
#if TIM1_BREAK_ENABLE
    bdtr.BreakState       = TIM_BREAK_ENABLE;
    bdtr.BreakPolarity    = (TIM1_BREAK_ACTIVE_HIGH != 0U) ? TIM_BREAKPOLARITY_HIGH : TIM_BREAKPOLARITY_LOW;
#else
    bdtr.BreakState       = TIM_BREAK_DISABLE;
    bdtr.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;
#endif
    bdtr.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;

    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bdtr) != HAL_OK) {
        while (1);
    }

    /*
     * Tüm 6 kanalı başlat.
     * HAL_TIM_PWM_Start → CH (yüksek taraf)
     * HAL_TIMEx_PWMN_Start → CHN (düşük taraf, komplementer)
     * İkisi de çalışır durumda olmalı; CCER ile enable/disable edilir.
     */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);       /* PA8 — AH */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);       /* PA9 — BH */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);       /* PA10 — CH */
    HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1);   /* PA7 — AL */
    HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);   /* PB0 — BL */
    HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_3);   /* PB1 — CL */

    /* Başlangıçta tüm kanalları devre dışı bırak — güvenli durum */
    /* CCR=0, CCER=0 → tüm pinler idle state = 0 */
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR3 = 0;
    TIM1->CCER = 0;

    /* Güvenli başlangıç: çıkışlar kapalı, re-arm ile açılır */
    TIM1->BDTR &= ~TIM_BDTR_MOE;
}

/* ====================================================================
 * TIM3 Kontrol Zamanlayıcısı — 12.5 kHz ISR
 *
 * APB1 timer saati = 96 MHz (APB1=48 MHz, prescaler≠1 → x2 = 96 MHz)
 * Ön bölücü (PSC) = 95 → 96 MHz / 96 = 1 MHz tick
 * Periyot (ARR) = 79 → 1 MHz / 80 = 12.5 kHz ISR
 * ==================================================================== */

void BoardIO_InitControlTimer(void) {
    __HAL_RCC_TIM3_CLK_ENABLE();

    htim3.Instance               = TIM3;
    htim3.Init.Prescaler         = CTRL_TIMER_PRESCALER;  /* 95 → 96 MHz/96 = 1 MHz */
    htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim3.Init.Period            = CTRL_TIMER_PERIOD;     /* 79 → 12.5 kHz */
    htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
        while (1);
    }

    /* Update interrupt — yüksek öncelik ama SysTick'e (priority 15) alan bırak */
    HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

/* Tüm init'ler tamamlandıktan sonra çağır */
void BoardIO_StartControlTimer(void) {
    HAL_TIM_Base_Start_IT(&htim3);
}

/* ====================================================================
 * TIM4 Hall Sensor Interface
 *
 * PB6/PB7/PB8 -> TIM4_CH1/CH2/CH3 (AF2)
 * Prescaler=95 ile sayaç 1 MHz (1 us çözünürlük)
 * Hall mode her geçişte CC1 capture üretir (event-driven hall acquisition)
 * ==================================================================== */

void BoardIO_InitHallTimer(void) {
    __HAL_RCC_TIM4_CLK_ENABLE();

    /* PB6/PB7/PB8 -> AF2 TIM4_CH1/CH2/CH3 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = HALL_A_PIN | HALL_B_PIN | HALL_C_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = HALL_TIMER_AF;
    HAL_GPIO_Init(GPIOB, &gpio);

    htim4.Instance               = TIM4;
    htim4.Init.Prescaler         = HALL_TIMER_PRESCALER;
    htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim4.Init.Period            = HALL_TIMER_PERIOD;
    htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    TIM_HallSensor_InitTypeDef hallCfg = {0};
    hallCfg.IC1Polarity       = TIM_ICPOLARITY_BOTHEDGE;
    hallCfg.IC1Prescaler      = TIM_ICPSC_DIV1;
    hallCfg.IC1Filter         = 4U;
    hallCfg.Commutation_Delay = 0U;

    if (HAL_TIMEx_HallSensor_Init(&htim4, &hallCfg) != HAL_OK) {
        while (1);
    }

    /* NVIC ayarla ama interrupt'i henuz baslatma.
     * Hall_Init() tamamlandiktan sonra BoardIO_StartHallTimer() ile baslatilir.
     */
    HAL_NVIC_SetPriority(TIM4_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);
}

/* Hall_Init() tamamlandiktan sonra cagrilir — capture interrupt baslatilir */
void BoardIO_StartHallTimer(void) {
    if (HAL_TIMEx_HallSensor_Start_IT(&htim4) != HAL_OK) {
        while (1);
    }
}

/* ====================================================================
 * ADC1 Başlatma — ISENSE (PA0/IN0) ve VSENSE (PA4/IN4)
 *
 * ADC saati: PCLK2/4 = 96/4 = 24 MHz (maks 36 MHz sınırının altında)
 * ADC sürekli scan + DMA circular ile arka planda akar.
 * ISR tarafı sadece son örnekleri DMA tamponundan okur (non-blocking).
 * ==================================================================== */

void BoardIO_InitADC(void) {
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* PA0 ve PA4 analog mod */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = ISENSE_ADC_PIN | VSENSE_ADC_PIN;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    hadc1.Instance                   = ADC1;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = ENABLE;          /* 2 kanal */
    hadc1.Init.ContinuousConvMode    = ENABLE;          /* sürekli */
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 2;
    hadc1.Init.DMAContinuousRequests = ENABLE;

    hdma_adc1.Instance                 = DMA2_Stream0;
    hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) {
        while (1);
    }

    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        while (1);
    }

    /* ADC ön bölücü: PCLK2/4 = 24 MHz (read-modify-write: diğer bitleri koru) */
    ADC1_COMMON->CCR = (ADC1_COMMON->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0;

    {
        ADC_ChannelConfTypeDef cfg = {0};
        cfg.Channel      = ISENSE_ADC_CHANNEL;
        cfg.Rank         = 1;
        cfg.SamplingTime = ADC_SAMPLETIME_84CYCLES;
        if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) {
            while (1);
        }

        cfg.Channel = VSENSE_ADC_CHANNEL;
        cfg.Rank    = 2;
        if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) {
            while (1);
        }
    }

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcDmaBuf, 2) != HAL_OK) {
        while (1);
    }
}

/* DMA-backed son örnek okuma — ISR-safe ve non-blocking */
uint16_t BoardIO_ReadADC(uint32_t channel) {
    if (channel == ISENSE_ADC_CHANNEL) {
        return adcDmaBuf[0];
    }
    if (channel == VSENSE_ADC_CHANNEL) {
        return adcDmaBuf[1];
    }
    return 0;
}

/* ====================================================================
 * USART2 Başlatma — CLI (PA2=TX, PA3=RX)
 *
 * PA9/PA10 TIM1_CH2/CH3 ile çakışır → USART2 PA2/PA3 kullanır.
 * USART2 APB1'de (48 MHz), 115200 baud sorunsuz çalışır.
 * ==================================================================== */

void BoardIO_InitUART(void) {
    __HAL_RCC_USART2_CLK_ENABLE();
    /* GPIOA clock already enabled in BoardIO_InitGPIO() */

    /* PA2 = USART2_TX, PA3 = USART2_RX — AF7 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = CLI_BAUD;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        while (1);
    }
}

/* ====================================================================
 * IWDG — Independent Watchdog
 * ==================================================================== */

void BoardIO_InitWatchdog(void) {
    __HAL_RCC_LSI_ENABLE();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == RESET) {
    }

    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;

    uint32_t reload = (IWDG_TIMEOUT_MS > 0U) ? (IWDG_TIMEOUT_MS - 1U) : 1U;
    if (reload > 0x0FFFU) {
        reload = 0x0FFFU;
    }
    hiwdg.Init.Reload = reload;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
        while (1) {
        }
    }
}

void BoardIO_KickWatchdog(void) {
    HAL_IWDG_Refresh(&hiwdg);
}

/* ====================================================================
 * PWM duty setter'ları — sadece CCR değerini günceller.
 * CCER enable/disable komütasyon katmanında yapılır.
 * ==================================================================== */

void BoardIO_SetPWMA(uint16_t duty) {
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, duty);
}

void BoardIO_SetPWMB(uint16_t duty) {
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, duty);
}

void BoardIO_SetPWMC(uint16_t duty) {
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, duty);
}

void BoardIO_SetAllPWM(uint16_t a, uint16_t b, uint16_t c) {
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, a);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, b);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, c);
}

/*
 * Tüm çıkışları kapat — güvenli durum.
 * CCR=0 (duty %0), CCER=0 (tüm kanallar devre dışı), MOE=0.
 * OSSR=1 ve idle state=0 olduğu için pinler LOW'da kalır
 * → L6388 INH=0, INL=0 → yüksek ve düşük taraf MOSFET'ler kapalı.
 */
void BoardIO_AllOff(void) {
    TIM1->CCR1 = 0;
    TIM1->CCR2 = 0;
    TIM1->CCR3 = 0;
    TIM1->CCER = 0;
    TIM1->BDTR &= ~TIM_BDTR_MOE;
}

void BoardIO_RearmPWMOutputs(void) {
    TIM1->BDTR |= TIM_BDTR_MOE;
}

/* ====================================================================
 * LED
 * ==================================================================== */

void BoardIO_LEDOn(void) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET); /* aktif düşük */
}

void BoardIO_LEDOff(void) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
}

void BoardIO_LEDToggle(void) {
    HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
}

/* ====================================================================
 * DWT döngü sayacıyla mikrosaniye gecikme
 * Sadece kalibrasyon sırasında kullanılır, ISR içinde ASLA çağrılmaz.
 * ==================================================================== */

void BoardIO_DelayUs(uint32_t us) {
    if (!dwt_initialized) return;
    uint32_t cycles = us * (SystemCoreClock / 1000000U);
    uint32_t start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) { }
}

/* ====================================================================
 * Ana başlatma — doğru sırada
 * ==================================================================== */

void BoardIO_InitAll(void) {
    HAL_Init();               /* HAL_Init SysTick'i 1 kHz'de kurar */
    BoardIO_InitClock();
    BoardIO_InitGPIO();
    BoardIO_InitHallTimer();
    BoardIO_InitADC();
    BoardIO_InitPWM();        /* TIM1 komplementer + deadtime */
    BoardIO_InitControlTimer();

    BoardIO_InitUART();

    BoardIO_AllOff();         /* Güvenli başlangıç durumu */
    BoardIO_InitWatchdog();
    BoardIO_KickWatchdog();
}
