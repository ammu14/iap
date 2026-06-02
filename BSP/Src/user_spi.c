/**
 * @file    user_spi.c
 * @brief   Software (bit-bang) SPI driver — reference/fallback only.
 * @note    This project uses hardware SPI1 (AF5 on PB3/4/5) via HAL for W25Q128.
 *          Using soft SPI together with hardware SPI will cause pin conflicts.
 *          Keep this file for debugging or when hardware SPI is unavailable.
 */

#include "user_spi.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"
#include <stdint.h>

/**
 * @brief  Initialize software SPI GPIOs
 *         PB14 -> CS (output push-pull, default high)
 *         PB3  -> SCK (output push-pull, default low)
 *         PB5  -> MOSI (output push-pull)
 *         PB4  -> MISO (input pull-up)
 */
void spi_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* CS, SCK, MOSI as push-pull outputs */
    GPIO_InitStruct.Pin   = GPIO_PIN_14 | GPIO_PIN_3 | GPIO_PIN_5;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* MISO as input with pull-up */
    GPIO_InitStruct.Pin  = GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* Idle state: CS high, SCK low */
    SPI_CS(1);
    SPI_SCK(0);
}

/**
 * @brief  Assert CS (low) to begin an SPI frame
 */
void spi_start(void)
{
    SPI_CS(0);
}

/**
 * @brief  De-assert CS (high) to end an SPI frame
 */
void spi_stop(void)
{
    SPI_CS(1);
}

/**
 * @brief  Bit-bang one byte on software SPI (SPI Mode 0: CPOL=0, CPHA=0)
 *         - Data sampled on rising edge of SCK
 *         - Data shifted out on falling edge of SCK
 *         - MSB first
 * @param  bytesend: byte to send
 * @retval byte received simultaneously
 */
uint8_t spi_swapbyte(uint8_t bytesend)
{
    uint8_t bytereceive = 0x00;

    for (uint8_t i = 0; i < 8; i++) {
        /* Put MOSI data, then pulse SCK high -> low (Mode 0) */
        SPI_MOSI(bytesend & (0x80 >> i));
        SPI_SCK(1);
        if (SPI_MISO() == 1) {
            bytereceive |= (0x80 >> i);
        }
        SPI_SCK(0);
    }
    return bytereceive;
}