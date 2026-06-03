#include "iap.h"
#include "boot_config.h"
#include "boot_storage.h"
#include "boot_verify.h"
#include "ymodem.h"
#include "ff.h"
#include "stm32f407xx.h"
#include "fatfs.h"
#include "usart.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stmflash.h"

typedef void (*iapfun)(void);
static iapfun jump2app;

/* ====== 外部引用 (DMA / 串口缓冲区) ====== */
extern DMA_HandleTypeDef  hdma_usart1_rx;
extern UART_HandleTypeDef huart1;

/* Ymodem 接收缓冲区 */
extern uint8_t           app_sram_buf[];
extern volatile uint32_t app_len;
extern volatile uint8_t  is_receiving;

/*
 * 跳转到 APP 区执行 (通用, 不限定 Slot)
 */
void iap_load_app(uint32_t appxaddr)
{
    if (((*(__IO uint32_t*)appxaddr) & 0x2FFE0000) == 0x20000000)
    {
        /* 等待 USART TX 完成, 避免最后一字节被截断产生乱码 */
        while (!(USART1->SR & USART_SR_TC)) { }

        /* 关闭 USART1, 并将 TX 引脚拉高, 防止 APP 启动时 GPIO 初始化产生垃圾字节 */
        HAL_UART_DeInit(&huart1);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET);

        __disable_irq();
        __set_MSP(*(__IO uint32_t*)appxaddr);
        uint32_t jump_addr = *(__IO uint32_t*)(appxaddr + 4);
        jump2app = (iapfun)jump_addr;
        jump2app();
    }
}

/* ====== Flash 写入辅助 (自动对齐到 word) ====== */
static int flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) return 0;

    uint32_t buf[256];  /* 栈上对齐缓冲区, 每批最多 1024 字节 */
    uint32_t written = 0;

    while (len > 0) {
        uint32_t chunk = (len > 1024) ? 1024 : len;
        uint32_t words = (chunk + 3) / 4;

        memcpy(buf, data + written, chunk);
        if (chunk < words * 4) {
            memset((uint8_t*)buf + chunk, 0xFF, words * 4 - chunk);
        }

        if (stmflash_write_word(addr + written, buf, words) != 0)
            return -1;

        written += chunk;
        len    -= chunk;
    }
    return 0;
}

/* ====== 擦除指定槽的全部扇区 ====== */
static int erase_slot(uint8_t slot)
{
    uint32_t slot_addr = BOOT_SLOT_ADDR(slot);
    uint32_t slot_size = BOOT_SLOT_SIZE(slot);

    uint32_t end_addr = slot_addr + slot_size;
    for (uint32_t addr = slot_addr; addr < end_addr; ) {
        stmflash_erase_addr(addr);

        /* 根据扇区大小递增地址 */
        if (addr < 0x08000000 + 0x10000) {
            /* Sector 0~3: 16KB each */
            addr += 0x4000;
        } else if (addr < 0x08000000 + 0x20000) {
            /* Sector 4: 64KB */
            addr += 0x10000;
        } else if (addr < 0x08000000 + 0x80000) {
            /* Sector 5~7: 128KB each */
            addr += 0x20000;
        } else {
            /* Sector 8~11: 128KB each */
            addr += 0x20000;
        }
    }
    return 0;
}

/* ====== 写入固件元数据到槽末尾 ====== */
static int write_meta_to_slot(uint8_t slot, uint32_t fw_size)
{
    uint32_t slot_addr = BOOT_SLOT_ADDR(slot);
    uint32_t meta_addr = BOOT_SLOT_META_ADDR(slot);

    uint32_t calc_crc = boot_crc32((const uint8_t*)slot_addr, fw_size);
    FirmwareHeader_t header;
    header.magic         = FIRMWARE_MAGIC;
    header.firmware_size = fw_size;
    header.firmware_crc  = calc_crc;
    memset(header.reserved, 0xFF, sizeof(header.reserved));

    return flash_write(meta_addr, (const uint8_t*)&header, sizeof(header));
}

/* ====== Ymodem 回调 (动态绑定槽) ====== */
static uint32_t g_target_slot_addr = 0;
static uint32_t g_ymodem_total = 0;
static uint8_t  g_target_slot = 0;

static int ymodem_flash_cb(uint32_t offset, const uint8_t *data, uint32_t len)
{
    return flash_write(g_target_slot_addr + offset, data, len);
}

static void ymodem_done_cb(uint32_t total_size)
{
    g_ymodem_total = total_size;
}

/* ======================================================================
 * 统一升级 API: iap_load_to_slot(slot, mode)
 *
 *   slot: BOOT_SLOT_A (0) 或 BOOT_SLOT_B (1)
 *   mode: IAP_MODE_SD (0) 或 IAP_MODE_UART (1)
 *
 * 返回 0 成功, 负数失败.
 * ====================================================================== */
int iap_load_to_slot(uint8_t slot, uint8_t mode)
{
    uint32_t slot_addr = BOOT_SLOT_ADDR(slot);
    uint32_t slot_size = BOOT_SLOT_SIZE(slot);
    const char *slot_name = (slot == BOOT_SLOT_A) ? "A" : "B";
    const char *mode_name = (mode == IAP_MODE_SD) ? "SD" : "UART";

    printf("[IAP] Target: Slot %s via %s (0x%08lX, %luKB)\r\n",
           slot_name, mode_name,
           (unsigned long)slot_addr,
           (unsigned long)(slot_size / 1024));

    /* 擦除目标槽 */
    printf("[IAP] Erasing Slot %s...\r\n", slot_name);
    erase_slot(slot);

    /* ---- SD 卡模式 ---- */
    if (mode == IAP_MODE_SD) {
        FRESULT res;
        FIL fil;
        UINT br;
        uint32_t flash_addr = slot_addr;
        uint8_t read_buf[1024];

        res = f_mount(&SDFatFS, SDPath, 1);
        if (res != FR_OK) {
            printf("[IAP] SD: Mount FAIL!\r\n");
            return -1;
        }

        res = f_open(&fil, "app.bin", FA_READ);
        if (res != FR_OK) {
            printf("[IAP] SD: app.bin not found!\r\n");
            f_mount(NULL, SDPath, 1);
            return -2;
        }

        FSIZE_t fsize = f_size(&fil);
        if (fsize > FLASH_APP_MAX_SIZE) {
            printf("[IAP] SD: File too large! (%lu > %lu)\r\n",
                   (unsigned long)fsize, (unsigned long)FLASH_APP_MAX_SIZE);
            f_close(&fil);
            f_mount(NULL, SDPath, 1);
            return -3;
        }

        printf("[IAP] SD: %lu bytes, writing to Slot %s...\r\n",
               (unsigned long)fsize, slot_name);

        while (f_read(&fil, read_buf, sizeof(read_buf), &br) == FR_OK && br > 0) {
            flash_write(flash_addr, read_buf, br);
            flash_addr += br;
            printf("[IAP] Writing %lu/%lu\r\n",
                   (unsigned long)(flash_addr - slot_addr), (unsigned long)fsize);
            if (br < sizeof(read_buf))
                break;
        }

        f_close(&fil);
        f_mount(NULL, SDPath, 1);

        /* 写元数据 */
        write_meta_to_slot(slot, (uint32_t)fsize);

        /* 校验 */
        printf("[IAP] Verifying Slot %s...\r\n", slot_name);
        uint32_t fw_size;
        if (boot_verify_slot(slot_addr, slot_size, &fw_size) != 0) {
            printf("[IAP] Slot %s Verify FAILED!\r\n", slot_name);
            return -4;
        }

        printf("[IAP] SD → Slot %s OK (%lu bytes)\r\n", slot_name, (unsigned long)fw_size);
        return 0;
    }

    /* ---- UART 模式 ---- */
    else {
        uint8_t tx_data[2];

        g_target_slot_addr = slot_addr;
        g_target_slot = slot;
        g_ymodem_total = 0;

        ymodem_init(ymodem_flash_cb, ymodem_done_cb);

        printf("[IAP] Waiting Ymodem sender...\r\n");

        HAL_UART_DMAStop(&huart1);
        app_len      = 0;
        is_receiving = 0;
        HAL_UART_Receive_DMA(&huart1, app_sram_buf, APP_MAX_SIZE);

        ymodem_reset();
        int tx_len = ymodem_get_tx_data(tx_data);
        if (tx_len > 0) {
            HAL_UART_Transmit(&huart1, tx_data, tx_len, 100);
        }

        while (1) {
            uint32_t now = HAL_GetTick();

            /* 握手阶段: 每500ms发 'C' */
            {
                static uint32_t last_c_send = 0;
                YmodemState_t ys_now = ymodem_get_state();
                if ((ys_now == YMODEM_STATE_WAIT_HEADER || ys_now == YMODEM_STATE_IDLE)
                    && app_len == 0
                    && now - last_c_send > 500) {
                    uint8_t c = 0x43;
                    HAL_UART_Transmit(&huart1, &c, 1, 100);
                    last_c_send = now;
                }
            }

            if (is_receiving) {
                __disable_irq();
                is_receiving = 0;
                HAL_UART_DMAStop(&huart1);
                uint32_t ndtr      = hdma_usart1_rx.Instance->NDTR;
                uint32_t new_bytes  = APP_MAX_SIZE - ndtr - app_len;
                if (new_bytes > 0)
                    app_len += new_bytes;
                uint32_t cur_len = app_len;
                __enable_irq();

                int consumed = ymodem_feed_buffer(app_sram_buf, cur_len);
                if (consumed < 0) {
                    printf("[IAP] Ymodem Error!\r\n");
                    HAL_UART_DMAStop(&huart1);
                    return -2;
                }

                uint32_t remain = cur_len - (uint32_t)consumed;
                if (remain > 0) {
                    memmove(app_sram_buf, &app_sram_buf[consumed], remain);
                    app_len = remain;
                } else {
                    app_len = 0;
                }

                HAL_UART_Receive_DMA(&huart1, &app_sram_buf[app_len], APP_MAX_SIZE - app_len);

                tx_len = ymodem_get_tx_data(tx_data);
                if (tx_len > 0) {
                    HAL_UART_Transmit(&huart1, tx_data, tx_len, 100);
                }
            }

            YmodemState_t ys = ymodem_get_state();
            if (ys == YMODEM_STATE_DONE) {
                HAL_UART_DMAStop(&huart1);
                break;
            }
            if (ys == YMODEM_STATE_ERROR || ys == YMODEM_STATE_CANCEL) {
                printf("[IAP] Ymodem Error!\r\n");
                HAL_UART_DMAStop(&huart1);
                return -2;
            }
        }

        /* 写元数据 */
        write_meta_to_slot(slot, g_ymodem_total);

        /* 校验 */
        printf("[IAP] Verifying Slot %s...\r\n", slot_name);
        uint32_t fw_size;
        if (boot_verify_slot(slot_addr, slot_size, &fw_size) != 0) {
            printf("[IAP] Slot %s Verify Failed!\r\n", slot_name);
            return -3;
        }

        printf("[IAP] UART → Slot %s OK (%lu bytes)\r\n", slot_name, (unsigned long)fw_size);
        return 0;
    }
}

/* ======================================================================
 * 兼容旧 API
 * ====================================================================== */
int iap_load_from_sd(void)
{
    return iap_load_to_slot(BOOT_SLOT_B, IAP_MODE_SD);
}

int iap_load_from_uart(void)
{
    return iap_load_to_slot(BOOT_SLOT_B, IAP_MODE_UART);
}