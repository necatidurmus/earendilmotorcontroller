/* ============================================================
 * Core/Src/stm32f4xx_it.c
 * Interrupt service routines — CubeMX generated
 *
 * The motor hot path lives in App/Src files.  ISRs here just
 * delegate to non-blocking handlers.
 * ============================================================ */

#include "stm32f4xx_hal.h"
#include "stm32f4xx_it.h"

#include "usart.h"
#include "tim.h"

extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart2_tx;

/* App-level ISR shims — must be non-blocking */
extern void App_Usart2RxIsr(uint16_t bytes);
extern void App_Tim6SchedulerTick(void);
extern void App_Tim1BrkIsr(void);
extern void App_Tim4HallIsr(void);

void NMI_Handler(void)
{
    for (;;) { }
}

void HardFault_Handler(void)
{
    for (;;) { }
}

void MemManage_Handler(void)
{
    for (;;) { }
}

void BusFault_Handler(void)
{
    for (;;) { }
}

void UsageFault_Handler(void)
{
    for (;;) { }
}

void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART2_IRQHandler(void)
{
    uint32_t sr = READ_REG(huart2.Instance->SR);

    /* IDLE line: burst complete — drain DMA buffer into ring.
     * Clear by reading SR then DR (F4 hardware sequence).
     * Must happen BEFORE HAL_UART_IRQHandler so the HAL does not
     * also try to clear IDLE by reading DR (which would steal a
     * byte from the DMA RX stream). */
    if ((sr & USART_SR_IDLE) != 0U) {
        (void)READ_REG(huart2.Instance->SR);
        (void)READ_REG(huart2.Instance->DR);
        App_Usart2RxIsr(0);
    }

    /* Let HAL handle TC (DMA TX complete → TxCpltCallback) and
     * error flags (ORE/NE/FE/PE).  The HAL clears ORE via SR
     * writes (not DR reads), so DMA RX is not affected. */
    HAL_UART_IRQHandler(&huart2);
}

void TIM1_BRK_TIM9_IRQHandler(void)
{
    /* Break event from TIM1 — used as a hardware fault line if wired. */
    HAL_TIM_IRQHandler(&htim1);
    App_Tim1BrkIsr();
}

void TIM1_UP_TIM10_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}

void TIM1_TRG_COM_TIM11_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}

void TIM1_CC_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim1);
}

void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim4);
    App_Tim4HallIsr();
}

void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);
}

void DMA1_Stream5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart2_rx);
}

void DMA1_Stream6_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart2_tx);
}

void TIM6_DAC_IRQHandler(void)
{
    /* TIM6 does not exist on STM32F411 — stub kept for .ioc vector
     * compatibility.  The 1 kHz scheduler tick is driven by SysTick. */
}

void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);
}
