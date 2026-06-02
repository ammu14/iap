/**
 * @file    w25Q128.c
 * @brief   W25Q128 SPI Flash driver — blocking HAL SPI (polling mode).
 * @note    Uses hardware SPI1 (PB3=SCK, PB4=MISO, PB5=MOSI, PB14=CS).
 *          No RTOS / DMA dependencies.
 */

#include "w25Q128.h"
#include "spi.h"         /* hspi1 extern */
#include "main.h"        /* SPI1_CS_Pin, SPI1_CS_GPIO_Port */
#include <stdint.h>

/*----------------------------------------------------------------------------
 * Local helpers (inline CS control via GPIO)
 *----------------------------------------------------------------------------*/
static inline void CS_Low(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

static inline void CS_High(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

/*----------------------------------------------------------------------------
 * Timeout for blocking SPI calls
 *----------------------------------------------------------------------------*/
#define W25Q128_SPI_TIMEOUT_MS   100U
#define W25Q128_BUSY_TIMEOUT     100000U   /* wait-busy loop max iterations */

/*----------------------------------------------------------------------------
 * Helper: blocking SPI transmit  (without DMA)
 *----------------------------------------------------------------------------*/
static HAL_StatusTypeDef SPI_Tx(uint8_t *pData, uint16_t size)
{
    return HAL_SPI_Transmit(&hspi1, pData, size, W25Q128_SPI_TIMEOUT_MS);
}

/*----------------------------------------------------------------------------
 * Helper: blocking SPI receive  (without DMA)
 *----------------------------------------------------------------------------*/
static HAL_StatusTypeDef SPI_Rx(uint8_t *pData, uint16_t size)
{
    return HAL_SPI_Receive(&hspi1, pData, size, W25Q128_SPI_TIMEOUT_MS);
}

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * @brief  Initialize W25Q128.
 * @note   Actual SPI1 init is done by MX_SPI1_Init() in CubeMX code.
 *         This function just ensures CS is de-asserted.
 */
void W25Q128_init(void)
{
    CS_High();
}

/**
 * @brief  Send Write Enable (0x06) command.
 */
void W25Q128_WriteEnable(void)
{
    uint8_t cmd = W25Q128_WRITE_ENABLE;
    CS_Low();
    SPI_Tx(&cmd, 1);
    CS_High();
}

/**
 * @brief  Read JEDEC ID: Manufacturer ID + Device ID.
 * @retval 32-bit ID: [23:16]=MID (W25Q128=0xEF), [15:0]=DID (W25Q128=0x4018)
 */
uint32_t W25Q128_read_ID(void)
{
    uint8_t tx_buf[4] = {W25Q128_JEDEC_ID, 0xFF, 0xFF, 0xFF};
    uint8_t rx_buf[4] = {0};

    CS_Low();
    SPI_Tx(tx_buf, 1);           /* send command */
    SPI_Rx(rx_buf + 1, 3);       /* receive MID + DID[15:8] + DID[7:0] */
    CS_High();

    uint8_t  mid = rx_buf[1];
    uint16_t did = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];

    return ((uint32_t)mid << 16) | did;
}

/**
 * @brief  Poll Status Register-1 until BUSY bit (bit 0) clears.
 *         Blocks with timeout to avoid infinite loop.
 */
void W25Q128_WaitBusy(void)
{
    uint32_t timeout = W25Q128_BUSY_TIMEOUT;
    uint8_t  cmd     = W25Q128_READ_STATUS_REGISTER_1;
    uint8_t  status  = 0x01;    /* assume busy initially */

    CS_Low();
    SPI_Tx(&cmd, 1);            /* send read-status command once */

    while ((status & 0x01) == 0x01) {
        if (timeout == 0) break;
        timeout--;

        SPI_Rx(&status, 1);     /* read 1-byte status */
    }
    CS_High();
}

/**
 * @brief  Page Program (max 256 bytes within a single page).
 *         Automatically sends Write Enable and waits for completion.
 * @param  Address: 24-bit flash byte address
 * @param  pData:   pointer to data
 * @param  count:   byte count (clamped to W25Q128_PAGE_SIZE)
 */
void W25Q128_PageProgram(uint32_t Address, uint8_t *pData, uint16_t count)
{
    if (count > W25Q128_PAGE_SIZE) {
        count = W25Q128_PAGE_SIZE;
    }

    W25Q128_WriteEnable();

    uint8_t header[4] = {
        W25Q128_PAGE_PROGRAM,
        (uint8_t)(Address >> 16),
        (uint8_t)(Address >> 8),
        (uint8_t)Address
    };

    CS_Low();
    SPI_Tx(header, 4);
    SPI_Tx(pData, count);
    CS_High();

    W25Q128_WaitBusy();
}

/**
 * @brief  Read arbitrary length of data from flash.
 * @param  Address: 24-bit flash byte address
 * @param  pData:   pointer to receive buffer
 * @param  count:   number of bytes to read
 */
void W25Q128_ReadData(uint32_t Address, uint8_t *pData, uint16_t count)
{
    uint8_t header[4] = {
        W25Q128_READ_DATA,
        (uint8_t)(Address >> 16),
        (uint8_t)(Address >> 8),
        (uint8_t)Address
    };

    CS_Low();
    SPI_Tx(header, 4);
    SPI_Rx(pData, count);
    CS_High();
}

/**
 * @brief  Erase one 4 KB sector.
 * @param  Address: 24-bit address within the target sector
 */
void W25Q128_SectorErase(uint32_t Address)
{
    W25Q128_WriteEnable();

    uint8_t header[4] = {
        W25Q128_SECTOR_ERASE_4KB,
        (uint8_t)(Address >> 16),
        (uint8_t)(Address >> 8),
        (uint8_t)Address
    };

    CS_Low();
    SPI_Tx(header, 4);
    CS_High();

    W25Q128_WaitBusy();
}

/**
 * @brief  Erase entire chip (typical ~80 s).
 */
void W25Q128_ERASE_CHIP(void)
{
    uint8_t cmd = W25Q128_CHIP_ERASE;

    W25Q128_WriteEnable();

    CS_Low();
    SPI_Tx(&cmd, 1);
    CS_High();

    W25Q128_WaitBusy();
}

/*----------------------------------------------------------------------------
 * Sector-boundary-aware write  (internal helper)
 *----------------------------------------------------------------------------*/

/**
 * @brief  Write data without checking for 0xFF (assumes pre-erased).
 * @param  pbuf:    data buffer
 * @param  addr:    flash byte address
 * @param  datalen: byte count
 */
static void W25Q128_WRITE_NOCHECK(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    uint16_t pageremain = W25Q128_PAGE_SIZE - (uint16_t)(addr % W25Q128_PAGE_SIZE);

    if (datalen <= pageremain) {
        pageremain = datalen;
    }

    while (1) {
        W25Q128_PageProgram(addr, pbuf, pageremain);

        if (datalen == pageremain) {
            break;                          /* all done */
        }

        /* advance to next page */
        pbuf    += pageremain;
        addr    += pageremain;
        datalen -= pageremain;

        if (datalen > W25Q128_PAGE_SIZE) {
            pageremain = W25Q128_PAGE_SIZE;
        } else {
            pageremain = datalen;
        }
    }
}

/*----------------------------------------------------------------------------
 * Public sector-boundary-aware write with auto-erase
 *----------------------------------------------------------------------------*/

static uint8_t g_W25Q128_BUF[W25Q128_PHYS_SECTOR_SIZE];   /* erase buffer */

/**
 * @brief  Write data to flash, automatically erasing sectors as needed.
 * @note   Erases are done sector-by-sector (4 KB minimum).
 *         If target data is shorter than a sector, the sector is
 *         read-modify-written: read whole sector, modify the target
 *         range, erase sector, write back.
 * @param  pbuf:    data buffer
 * @param  addr:    24-bit flash byte address
 * @param  datalen: number of bytes to write
 */
void W25Q128_WRITE(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    uint32_t secpos    = addr / W25Q128_PHYS_SECTOR_SIZE;
    uint16_t secoff    = (uint16_t)(addr % W25Q128_PHYS_SECTOR_SIZE);
    uint16_t secremain = W25Q128_PHYS_SECTOR_SIZE - secoff;

    if (datalen <= secremain) {
        secremain = datalen;
    }

    while (1) {
        /* Read the entire sector into buffer */
        W25Q128_ReadData(secpos * W25Q128_PHYS_SECTOR_SIZE,
                         g_W25Q128_BUF, W25Q128_PHYS_SECTOR_SIZE);

        /* Check if target range is already all 0xFF */
        uint16_t i;
        for (i = 0; i < secremain; i++) {
            if (g_W25Q128_BUF[secoff + i] != 0xFF) {
                break;
            }
        }

        if (i < secremain) {
            /* Need erase: merge new data into buffer, erase, write back */
            W25Q128_SectorErase(secpos);

            for (i = 0; i < secremain; i++) {
                g_W25Q128_BUF[secoff + i] = pbuf[i];
            }
            W25Q128_WRITE_NOCHECK(g_W25Q128_BUF,
                                  secpos * W25Q128_PHYS_SECTOR_SIZE,
                                  W25Q128_PHYS_SECTOR_SIZE);
        } else {
            /* Already erased — write directly */
            W25Q128_WRITE_NOCHECK(pbuf, addr, secremain);
        }

        if (datalen == secremain) {
            break;                          /* all done */
        }

        /* Advance to next sector */
        secpos++;
        secoff = 0;
        pbuf    += secremain;
        addr    += secremain;
        datalen -= secremain;

        if (datalen > W25Q128_PHYS_SECTOR_SIZE) {
            secremain = W25Q128_PHYS_SECTOR_SIZE;
        } else {
            secremain = datalen;
        }
    }
}

/*============================================================================
 * Self-test (static, for debugging only)
 *============================================================================*/

#define W25Q128_TEST_ADDR      0x000000U
#define W25Q128_TEST_DATA_LEN  64U
#define W25Q128_TEST_STRING    "Hello_W25Q128_1234"

/**
 * @brief  Self-test: write a test string, read back, compare.
 * @retval 1 = pass, 0 = fail
 */
static uint8_t W25Q128_ReadData_Test(void)
{
    uint8_t write_buf[W25Q128_TEST_DATA_LEN] = W25Q128_TEST_STRING;
    uint8_t read_buf[W25Q128_TEST_DATA_LEN]  = {0};
    uint8_t result = 1;

    W25Q128_init();
    HAL_Delay(100);

    /* Erase sector first, then program test data */
    W25Q128_SectorErase(W25Q128_TEST_ADDR);
    HAL_Delay(50);

    W25Q128_PageProgram(W25Q128_TEST_ADDR, write_buf, W25Q128_TEST_DATA_LEN);
    HAL_Delay(50);

    W25Q128_ReadData(W25Q128_TEST_ADDR, read_buf, W25Q128_TEST_DATA_LEN);

    for (uint8_t i = 0; i < W25Q128_TEST_DATA_LEN; i++) {
        if (read_buf[i] != write_buf[i]) {
            result = 0;
            break;
        }
    }

    return result;
}