/* ============================================================
 * Core/Src/gpio.c
 * GPIO init — CubeMX generated.
 * Configures Hall inputs (PB6/PB7/PB8) and LED (PC13).
 * Motor gate outputs are configured in HAL_TIM_MspPostInit().
 * ============================================================ */

#include "gpio.h"

void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* GPIO ports clock enable */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* LED on PC13 */
    gpio.Pin   = GPIO_PIN_13;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

    /* Hall inputs on PB6/PB7/PB8 — EXTI mode for edge-triggered
     * interrupt.  Rising + falling edge captures every Hall transition
     * even when the main loop is busy (e.g. UART TX).  The actual
     * debounce / state machine lives in hall_sensor.c (ISSUE-007). */
    gpio.Pin   = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8;
    gpio.Mode  = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* EXTI9_5 IRQ is enabled in the NVIC (covers EXTI lines 5..9).
     * Priority 6 — lower urgency than TIM1 / DMA / USART. */
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}
