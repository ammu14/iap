#ifndef __USER_SPI_H
#define __USER_SPI_H

#include "stm32f4xx_hal.h"

/* Software SPI GPIO macros (PB14=CS, PB3=SCK, PB5=MOSI, PB4=MISO)
 * Note: These conflict with hardware SPI1 (AF5 on PB3/4/5).
 *       Use hardware SPI (w25Q128) for production; soft SPI kept as fallback reference.
 */
#define SPI_CS(x)       HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, (GPIO_PinState)(x))
#define SPI_SCK(x)      HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3,  (GPIO_PinState)(x))
#define SPI_MOSI(x)     HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5,  (GPIO_PinState)(x))
#define SPI_MISO()      HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4)

/**
 * @brief  Initialize software SPI GPIOs (CS/SCK/MOSI output, MISO input)
 */
void spi_init(void);

/**
 * @brief  Pull CS low to start an SPI transaction
 */
void spi_start(void);

/**
 * @brief  Pull CS high to end an SPI transaction
 */
void spi_stop(void);

/**
 * @brief  Send/receive one byte over software SPI (SPI Mode 0: CPOL=0, CPHA=0)
 * @param  bytesend: byte to send
 * @retval byte received while sending
 */
uint8_t spi_swapbyte(uint8_t bytesend);

#endif