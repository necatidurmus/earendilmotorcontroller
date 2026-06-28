/* ============================================================
 * Core/Src/main.c
 * CubeMX-generated main — minimal, only calls App_Init / App_Loop.
 * Heavy application logic lives in App/Src.
 * ============================================================ */

#include "main.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "stm32f4xx_hal_gpio.h"

/* Forward declarations from App layer */
extern void App_Init(void);
extern void App_Loop(void);

void SystemClock_Config(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM4_Init();

    /* TIM1 is configured in CubeMX but PWM outputs are enabled by the
     * motor driver layer only when needed. */

    /* Reconfigure LOW-side pins (PA7, PB0, PB1) from TIM1 AF1 to
     * GPIO push-pull output.  TIM1 complementary outputs (CCxNE +
     * forced inactive mode) don't reliably drive the low-side gates
     * on this hardware.  The Arduino firmware uses GPIO for low-side
     * control, so we replicate that approach.  This MUST be done
     * after MX_TIM1_Init() (which sets AF1) but before App_Init()
     * (which starts the motor driver). */
    {
        GPIO_InitTypeDef gpio = {0};
        gpio.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio.Pull  = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        gpio.Pin   = GPIO_PIN_7;
        HAL_GPIO_Init(GPIOA, &gpio);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
        gpio.Pin   = GPIO_PIN_0;
        HAL_GPIO_Init(GPIOB, &gpio);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
        gpio.Pin   = GPIO_PIN_1;
        HAL_GPIO_Init(GPIOB, &gpio);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
    }

    App_Init();

    for (;;) {
        App_Loop();
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    /* 25 MHz HSE crystal (WeAct BlackPill F411CE):
     *   VCO input = 25 MHz / PLLM(25) = 1 MHz  (spec: 0.95..2.1 MHz)
     *   VCO out   = 1 MHz × PLLN(192) = 192 MHz (spec: 100..432 MHz)
     *   SYSCLK    = 192 MHz / PLLP(2) = 96 MHz
     *   USB clock = 192 MHz / PLLQ(4) = 48 MHz
     *
     * If the board has an 8 MHz crystal, change PLLM to 8. */
    RCC_OscInitStruct.PLL.PLLM       = 25;
    RCC_OscInitStruct.PLL.PLLN       = 192;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 4;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;   /* 48 MHz APB1  -> TIM clk 96 MHz */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;   /* 96 MHz APB2  -> TIM clk 96 MHz */

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK) {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) { }
}
