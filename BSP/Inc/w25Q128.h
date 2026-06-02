#ifndef __W25Q128_H
#define __W25Q128_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

/*----------------------------------------------------------------------------
 * W25Q128 Command Set
 *----------------------------------------------------------------------------*/
#define W25Q128_WRITE_ENABLE                        0x06
#define W25Q128_WRITE_DISABLE                       0x04
#define W25Q128_READ_STATUS_REGISTER_1              0x05
#define W25Q128_READ_STATUS_REGISTER_2              0x35
#define W25Q128_READ_STATUS_REGISTER_3              0x15
#define W25Q128_WRITE_STATUS_REGISTER_1             0x01
#define W25Q128_WRITE_STATUS_REGISTER_2             0x31
#define W25Q128_WRITE_STATUS_REGISTER_3             0x11
#define W25Q128_READ_DATA                           0x03
#define W25Q128_FAST_READ                           0x0B
#define W25Q128_FAST_READ_DUAL_OUTPUT               0x3B
#define W25Q128_FAST_READ_QUAD_IO                   0xEB
#define W25Q128_PAGE_PROGRAM                        0x02
#define W25Q128_QUAD_PAGE_PROGRAM                   0x32
#define W25Q128_BLOCK_ERASE_64KB                    0xD8
#define W25Q128_SECTOR_ERASE_4KB                    0x20
#define W25Q128_CHIP_ERASE                          0xC7
#define W25Q128_POWER_DOWN                          0xB9
#define W25Q128_RELEASE_POWER_DOWN_HPM_DEVICE_ID    0xAB
#define W25Q128_MANUFACTURER_DEVICE_ID              0x90
#define W25Q128_JEDEC_ID                            0x9F
#define W25Q128_BLOCK_ERASE_32KB                    0x52
#define W25Q128_ERASE_SUSPEND                       0x75
#define W25Q128_ERASE_RESUME                        0x7A
#define W25Q128_HIGH_PERFORMANCE_MODE               0xA3
#define W25Q128_CONTINUOUS_READ_MODE_RESET          0xFF
#define W25Q128_READ_UNIQUE_ID                      0x4B
#define W25Q128_FAST_READ_DUAL_IO                   0xBB
#define W25Q128_FAST_READ_QUAD_OUTPUT               0x6B
#define W25Q128_OCTAL_WORD_READ_QUAD_IO             0xE3
#define W25Q128_DUMMY_BYTE                          0xFF

/*----------------------------------------------------------------------------
 * W25Q128 Hardware Parameters
 *----------------------------------------------------------------------------*/
#define W25Q128_TOTAL_SIZE          0x1000000U   /* 16 MB (128 Mbit)        */
#define W25Q128_PAGE_SIZE           256U         /* 256 bytes (min write)   */
#define W25Q128_PHYS_SECTOR_SIZE    4096U        /* 4 KB (min erase)        */

/*----------------------------------------------------------------------------
 * API — all functions use blocking HAL SPI (polling mode, no DMA / no RTOS)
 *----------------------------------------------------------------------------*/

/**
 * @brief  Initialize W25Q128 (dummy, actual SPI init done by MX_SPI1_Init)
 */
void W25Q128_init(void);

/**
 * @brief  Send Write Enable command
 */
void W25Q128_WriteEnable(void);

/**
 * @brief  Read JEDEC manufacturer + device ID
 * @retval 32-bit ID: [23:16]=Manufacturer ID, [15:0]=Device ID
 */
uint32_t W25Q128_read_ID(void);

/**
 * @brief  Poll status register until BUSY bit clears (with timeout)
 */
void W25Q128_WaitBusy(void);

/**
 * @brief  Program up to 256 bytes within a single page
 * @param  Address: 24-bit flash byte address
 * @param  pData:   pointer to data buffer
 * @param  count:   number of bytes (clamped to 256 max)
 */
void W25Q128_PageProgram(uint32_t Address, uint8_t *pData, uint16_t count);

/**
 * @brief  Read arbitrary length of data from flash
 * @param  Address: 24-bit flash byte address
 * @param  pData:   pointer to receive buffer
 * @param  count:   number of bytes to read
 */
void W25Q128_ReadData(uint32_t Address, uint8_t *pData, uint16_t count);

/**
 * @brief  Erase one 4 KB sector
 * @param  Address: 24-bit address within the sector to erase
 */
void W25Q128_SectorErase(uint32_t Address);

/**
 * @brief  Erase entire chip (very slow, ~80s typical)
 */
void W25Q128_ERASE_CHIP(void);

/**
 * @brief  Write arbitrary data with automatic sector-erase-before-write
 * @param  pbuf:    pointer to data buffer
 * @param  addr:    24-bit flash byte address
 * @param  datalen: number of bytes to write
 */
void W25Q128_WRITE(uint8_t *pbuf, uint32_t addr, uint16_t datalen);

#endif /* __W25Q128_H */