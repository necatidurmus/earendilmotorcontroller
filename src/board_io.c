/*
 * board_io.c — Board-level HAL initialization for Earendil BLDC controller
 *
 * Configures: system clock, GPIO, TIM1 PWM, TIM3 control timer, ADC1, USART1.
 * Uses STM32Cube HAL exclusively — no Arduino dependencies.
 */

#include "board_io.h"

#include <string.h>

/* HAL handles — defined here, declared extern in motor_config.h */
TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim3;
ADC_HandleTypeDef  hadc1;
UART_HandleTypeDef huart2;

/* DWT cycle counter for microsecond delay */
static int dwt_initialized = 0;

static void DWT_Init(void) {
    if (dwt_initialized) return;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    dwt_initialized = 1;
}

/* ====================================================================
 * System Clock — 100 MHz from 25 MHz HSE
 *
 * HSE=25MHz, PLL_M=25, PLL_N=200, PLL_P=2 -> SYSCLK=100MHz
 * APB1 prescaler=2 -> APB1 clock=50MHz, timer clock=100MHz (x2)
 * APB2 prescaler=1 -> APB2 clock=100MHz, timer clock=200MHz (x2)
 *
 * NOTE: The STM32F411 Black Pill has HSE=25MHz. PLL config:
 *   VCO = HSE/M * N = 25/25 * 200 = 200 MHz
 *   SYSCLK = VCO/P = 200/2 = 100 MHz
 *   APB1 = SYSCLK/2 = 50 MHz (timer = 100 MHz)
 *   APB2 = SYSCLK/1 = 100 MHz (timer = 200 MHz)
 * ==================================================================== */

void BoardIO_InitClock(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* HSE = 25 MHz on Black Pill */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 25;
    RCC_OscInitStruct.PLL.PLLN = 200;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 4;    /* USB clock = 200/4 = 50 MHz */

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        while (1) { BoardIO_LEDToggle(); for (volatile int i = 0; i < 500000; i++); }
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;     /* HCLK = 100 MHz */
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;   /* APB1 = 50 MHz */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;   /* APB2 = 100 MHz */

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK) {
        while (1) { BoardIO_LEDToggle(); for (volatile int i = 0; i < 500000; i++); }
    }

    DWT_Init();
}

/* ====================================================================
 * GPIO Init
 * ==================================================================== */

void BoardIO_InitGPIO(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* LED — PC13, push-pull output */
    gpio.Pin = LED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &gpio);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET); /* LED off (active low on Black Pill) */

    /* Hall inputs — PB6/PB7/PB8, pull-up */
    gpio.Pin = HALL_A_PIN | HALL_B_PIN | HALL_C_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(HALL_A_PORT, &gpio); /* All on GPIOB */

    /* Low-side outputs — PA7, PB0, PB1 — push-pull, initially LOW */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    gpio.Pin = AL_PIN;
    HAL_GPIO_Init(AL_PORT, &gpio);
    HAL_GPIO_WritePin(AL_PORT, AL_PIN, GPIO_PIN_RESET);

    gpio.Pin = BL_PIN;
    HAL_GPIO_Init(BL_PORT, &gpio);
    HAL_GPIO_WritePin(BL_PORT, BL_PIN, GPIO_PIN_RESET);

    gpio.Pin = CL_PIN;
    HAL_GPIO_Init(CL_PORT, &gpio);
    HAL_GPIO_WritePin(CL_PORT, CL_PIN, GPIO_PIN_RESET);
}

/* ====================================================================
 * TIM1 PWM — High-side outputs PA8/PA9/PA10 (TIM1_CH1/CH2/CH3)
 *
 * APB2 timer clock = 200 MHz
 * Prescaler = 0  -> 200 MHz tick
 * Period = PWM_PERIOD_COUNTS = 3332 -> 200 MHz / 3333 = ~60 kHz...
 *
 * WAIT — let me recalculate:
 *   If APB2 prescaler = 1, APB2 clock = 100 MHz, timer clock = 200 MHz
 *   But we want 30 kHz PWM.
 *   Period = 200 MHz / 30 kHz - 1 = 6666 - 1 = 6665
 *   That gives ~10.5 bits resolution.
 *
 *   Alternatively, use prescaler=1:
 *   200 MHz / 2 = 100 MHz tick
 *   Period = 100 MHz / 30 kHz - 1 = 3333 - 1 = 3332
 *   That gives ~10 bits resolution, matching Arduino config.
 *
 * We use prescaler=1, period=3332.
 * ==================================================================== */

void BoardIO_InitPWM(void) {
    __HAL_RCC_TIM1_CLK_ENABLE();

    /* Configure TIM1 GPIO: PA8/PA9/PA10 as AF1 (TIM1_CH1/CH2/CH3) */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = AH_PIN | BH_PIN | CH_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* TIM1 time base */
    htim1.Instance = TIM1;
    htim1.Init.Prescaler = 1;                          /* 200 MHz / 2 = 100 MHz */
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = PWM_PERIOD_COUNTS;              /* 100 MHz / 3333 = 30 kHz */
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        while (1);
    }

    /* PWM channel configuration */
    TIM_OC_InitTypeDef sConfigOC = {0};
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 0;                               /* start with 0% duty */
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;

    HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1);  /* PA8 — AH */
    HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2);  /* PA9 — BH */
    HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_3);  /* PA10 — CH */

    /* TIM1 is an advanced timer — need to enable main output */
    TIM_BreakDeadTimeConfigTypeDef sBreakDTConfig = {0};
    sBreakDTConfig.OffStateRunMode = TIM_OSSR_DISABLE;
    sBreakDTConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
    sBreakDTConfig.LockLevel = TIM_LOCKLEVEL_OFF;
    sBreakDTConfig.DeadTime = 0;                        /* no hardware dead-time */
    sBreakDTConfig.BreakState = TIM_BREAK_DISABLE;
    sBreakDTConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
    sBreakDTConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
    HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDTConfig);

    /* Start PWM on all 3 channels */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
}

/* ====================================================================
 * TIM3 Control Timer — 12.5 kHz ISR
 *
 * APB1 timer clock = 100 MHz
 * Prescaler = 83 -> 100 MHz / 84 = 1.190476 MHz...
 *
 * Wait, recalculate from clock tree:
 *   APB1 = 50 MHz, timer clock = 100 MHz (x2 because APB1 prescaler != 1)
 *   Actually APB1 prescaler = 2, so timer clock = 50 * 2 = 100 MHz
 *
 *   Prescaler = 99 -> 100 MHz / 100 = 1 MHz
 *   Period = 79 -> 1 MHz / 80 = 12.5 kHz
 *
 * Hmm, but the old code had APB1=84MHz, timer=84MHz.
 * With our 100 MHz PLL: APB1=50MHz, timer=100MHz.
 * So prescaler = 99 (100MHz/100 = 1MHz), period = 79 (1MHz/80 = 12.5kHz).
 * ==================================================================== */

void BoardIO_InitControlTimer(void) {
    __HAL_RCC_TIM3_CLK_ENABLE();

    htim3.Instance = TIM3;
    htim3.Init.Prescaler = 99;             /* 100 MHz / 100 = 1 MHz */
    htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3.Init.Period = CTRL_TIMER_PERIOD;  /* 79 -> 1 MHz / 80 = 12.5 kHz */
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
        while (1);
    }

    /* Configure update interrupt */
    HAL_NVIC_SetPriority(TIM3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

/* Start control timer — call after all other inits */
void BoardIO_StartControlTimer(void) {
    HAL_TIM_Base_Start_IT(&htim3);
}

/* ====================================================================
 * ADC1 Init — ISENSE (PA0/IN0) and VSENSE (PA4/IN4)
 * ==================================================================== */

void BoardIO_InitADC(void) {
    __HAL_RCC_ADC1_CLK_ENABLE();

    /* ADC GPIO: PA0 and PA4 as analog */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = ISENSE_ADC_PIN | VSENSE_ADC_PIN;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    hadc1.Instance = ADC1;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = DISABLE;              /* single channel mode */
    hadc1.Init.ContinuousConvMode = DISABLE;        /* single conversion */
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;

    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        while (1);
    }

    /* ADC prescaler: PCLK2/4 = 100/4 = 25 MHz (must be < 36 MHz) */
    ADC1_COMMON->CCR = ADC_CCR_ADCPRE_0;  /* 01 = /4 */
}

/* Blocking single-channel ADC read */
uint16_t BoardIO_ReadADC(uint32_t channel) {
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = channel;
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_84CYCLES; /* enough settling time */

    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start(&hadc1);

    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK) {
        return (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
    return 0;
}

/* ====================================================================
 * USART1 Init — CLI serial (PA9=TX, PA10=RX)
 *
 * NOTE: PA9/PA10 are also TIM1_CH2/CH3 (PWM outputs).
 * USART1 TX/RX needs remap or alternate pin.
 *
 * Actually: STM32F411 PA9 = TIM1_CH2 (AF1) or USART1_TX (AF7)
 *           PA10 = TIM1_CH3 (AF1) or USART1_RX (AF7)
 *
 * CONFLICT: PA9 and PA10 are used for both TIM1 PWM AND USART1.
 * This is a hardware conflict. USART1 must use different pins.
 *
 * Available USART1 pins:
 *   TX: PB6 (conflict with Hall_A), PA9 (conflict with TIM1_CH2)
 *   RX: PB7 (conflict with Hall_B), PA10 (conflict with TIM1_CH3)
 *
 * USART2: PA2(TX)/PA3(RX) — available!
 *   But USART2 is on APB1 (50 MHz).
 *
 * Decision: Use USART2 on PA2/PA3 for CLI.
 * The original Arduino firmware used USB CDC which doesn't conflict.
 * For now we use UART2 as the simplest non-conflicting option.
 * ==================================================================== */

/* Re-definition: CLI UART is actually USART2 */
#undef CLI_UART_HANDLE
#define CLI_UART_HANDLE huart2

UART_HandleTypeDef huart2;

void BoardIO_InitUART(void) {
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA2 = USART2_TX, PA3 = USART2_RX — AF7 */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart2.Instance = USART2;
    huart2.Init.BaudRate = CLI_BAUD;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        while (1);
    }
}

/* ====================================================================
 * PWM duty setters
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

/* ====================================================================
 * Low-side GPIO
 * ==================================================================== */

void BoardIO_SetLowSide(int al, int bl, int cl) {
    HAL_GPIO_WritePin(AL_PORT, AL_PIN, al ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BL_PORT, BL_PIN, bl ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CL_PORT, CL_PIN, cl ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void BoardIO_AllOff(void) {
    BoardIO_SetAllPWM(0, 0, 0);
    BoardIO_SetLowSide(0, 0, 0);
}

/* ====================================================================
 * LED
 * ==================================================================== */

void BoardIO_LEDOn(void) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET); /* active low */
}

void BoardIO_LEDOff(void) {
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
}

void BoardIO_LEDToggle(void) {
    HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
}

/* ====================================================================
 * Microsecond delay via DWT cycle counter
 * ==================================================================== */

void BoardIO_DelayUs(uint32_t us) {
    if (!dwt_initialized) return;
    uint32_t cycles = us * (SystemCoreClock / 1000000);
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) { }
}

/* ====================================================================
 * Master init
 * ==================================================================== */

void BoardIO_InitAll(void) {
    HAL_Init();             /* HAL_Init sets up SysTick at 1 kHz */
    BoardIO_InitClock();
    BoardIO_InitGPIO();
    BoardIO_InitADC();
    BoardIO_InitPWM();
    BoardIO_InitControlTimer();
    BoardIO_InitUART();
    BoardIO_AllOff();
}
