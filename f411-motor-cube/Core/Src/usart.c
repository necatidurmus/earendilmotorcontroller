/* ============================================================
 * Core/Src/usart.c
 * USART2 init — command UART, 115200 8N1
 * DMA RX (circular) + DMA TX (ring buffer) for non-blocking I/O.
 * ============================================================ */

#include "usart.h"
#include "main.h"

UART_HandleTypeDef huart2;
DMA_HandleTypeDef  hdma_usart2_rx;
DMA_HandleTypeDef  hdma_usart2_tx;

void MX_USART2_UART_Init(void)
{
    huart2.Instance        = USART2;
    huart2.Init.BaudRate   = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits   = UART_STOPBITS_1;
    huart2.Init.Parity     = UART_PARITY_NONE;
    huart2.Init.Mode       = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();
    }
}

void HAL_UART_MspInit(UART_HandleTypeDef *uart)
{
    GPIO_InitTypeDef gpio = {0};

    if (uart->Instance == USART2) {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_DMA1_CLK_ENABLE();

        /* PA2 = TX, PA3 = RX */
        gpio.Pin   = GPIO_PIN_2 | GPIO_PIN_3;
        gpio.Mode  = GPIO_MODE_AF_PP;
        gpio.Pull  = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        gpio.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &gpio);

        /* DMA1 Stream5 Channel4 = USART2_RX (RM0090 table 43) */
        hdma_usart2_rx.Instance                 = DMA1_Stream5;
        hdma_usart2_rx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart2_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        hdma_usart2_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart2_rx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart2_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart2_rx.Init.Mode                = DMA_CIRCULAR;
        hdma_usart2_rx.Init.Priority            = DMA_PRIORITY_HIGH;
        hdma_usart2_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&hdma_usart2_rx) != HAL_OK) {
            Error_Handler();
        }
        __HAL_LINKDMA(uart, hdmarx, hdma_usart2_rx);

        /* DMA1 Stream6 Channel4 = USART2_TX (RM0090 table 43) */
        hdma_usart2_tx.Instance                 = DMA1_Stream6;
        hdma_usart2_tx.Init.Channel             = DMA_CHANNEL_4;
        hdma_usart2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_usart2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_usart2_tx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_usart2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_usart2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_usart2_tx.Init.Mode                = DMA_NORMAL;
        hdma_usart2_tx.Init.Priority            = DMA_PRIORITY_LOW;
        hdma_usart2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&hdma_usart2_tx) != HAL_OK) {
            Error_Handler();
        }
        __HAL_LINKDMA(uart, hdmatx, hdma_usart2_tx);

        HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(USART2_IRQn);

        HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

        HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
    }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *uart)
{
    if (uart->Instance == USART2) {
        __HAL_RCC_USART2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
    }
}
