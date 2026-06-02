#include "iap.h"
#include "boot_config.h"
#include "boot_verify.h"
#include "ymodem.h"
#include "ff.h"
#include "stm32f407xx.h"
#include "fatfs.h"
#include "lcd.h"
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

/*
 * 跳转到 APP 区执行
 */
void iap_load_app(uint32_t appxaddr)
{
    if (((*(__IO uint32_t*)appxaddr) & 0x2FFE0000) == 0x20000000)
    { 
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

/* ====== 擦除整个 APP 区 (Sector 4~11, 960KB) ====== */
static int erase_app_region(void)
{
    static const uint32_t sector_addrs[] = {
        0x08010000, /* Sector 4  (64KB)  */
        0x08020000, /* Sector 5  (128KB) */
        0x08040000, /* Sector 6  (128KB) */
        0x08060000, /* Sector 7  (128KB) */
        0x08080000, /* Sector 8  (128KB) */
        0x080A0000, /* Sector 9  (128KB) */
        0x080C0000, /* Sector 10 (128KB) */
        0x080E0000, /* Sector 11 (128KB) */
    };

    for (int i = 0; i < 8; i++) {
        stmflash_erase_addr(sector_addrs[i]);
    }
    return 0;
}

/* ====== Ymodem 回调 ====== */
static int ymodem_flash_cb(uint32_t offset, const uint8_t *data, uint32_t len)
{
    return flash_write(FLASH_APP_ADDR + offset, data, len);
}

static uint32_t g_ymodem_total = 0;
static void ymodem_done_cb(uint32_t total_size)
{
    g_ymodem_total = total_size;
}

/* ======================================================================
 * SD 卡升级
 *
 * 直接从 SD 卡读取 app.bin, 擦除 Flash 后逐块写入.
 * 不经过任何协议栈, 是最快的升级路径.
 * ====================================================================== */
int iap_load_from_sd(void) 
{
    FRESULT res;
    FIL fil;
    UINT br;
    uint32_t flash_addr = FLASH_APP_ADDR;
    uint8_t read_buf[1024];
    static char str_buf[40];

    /* 1. 挂载 SD */
    res = f_mount(&SDFatFS, SDPath, 1);
    if (res != FR_OK) {
        POINT_COLOR = RED;
        LCD_ShowString(LCD_X, LCD_LINE_S2, 280, LCD_FONT_H, LCD_FONT_W, "SD: Mount FAIL!");
        return -1;
    }

    /* 2. 打开 app.bin */
    res = f_open(&fil, "app.bin", FA_READ);
    if (res != FR_OK) {
        POINT_COLOR = RED;
        LCD_ShowString(LCD_X, LCD_LINE_S2, 280, LCD_FONT_H, LCD_FONT_W, "SD: app.bin not found!");
        f_mount(NULL, SDPath, 1);
        return -2;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize > FLASH_APP_SIZE - FLASH_APP_META_SIZE) {
        POINT_COLOR = RED;
        LCD_ShowString(LCD_X, LCD_LINE_S2, 280, LCD_FONT_H, LCD_FONT_W, "SD: File too large!");
        f_close(&fil);
        f_mount(NULL, SDPath, 1);
        return -3;
    }

    snprintf(str_buf, sizeof(str_buf), "SD: %lu bytes", (unsigned long)fsize);
    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S2, 280, LCD_FONT_H, LCD_FONT_W, (uint8_t*)str_buf);

    /* 3. 擦除 APP 区 */
    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Erasing...");
    erase_app_region();

    /* 4. 逐块写入 Flash */
    while (f_read(&fil, read_buf, sizeof(read_buf), &br) == FR_OK && br > 0) {
        flash_write(flash_addr, read_buf, br);
        flash_addr += br;

        snprintf(str_buf, sizeof(str_buf), "Writing %lu/%lu",
                 (unsigned long)(flash_addr - FLASH_APP_ADDR), (unsigned long)fsize);
        POINT_COLOR = BLUE;
        LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, (uint8_t*)str_buf);

        if (br < sizeof(read_buf))
            break;
    }

    f_close(&fil);
    f_mount(NULL, SDPath, 1);

    /* 5. 写入固件元数据 */
    {
        uint32_t calc_crc = boot_crc32((const uint8_t*)FLASH_APP_ADDR, (uint32_t)fsize);
        FirmwareHeader_t header;
        header.magic         = FIRMWARE_MAGIC;
        header.firmware_size = (uint32_t)fsize;
        header.firmware_crc  = calc_crc;
        memset(header.reserved, 0xFF, sizeof(header.reserved));

        uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
        flash_write(meta_addr, (const uint8_t*)&header, sizeof(header));
    }

    /* 6. 校验 */
    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Verifying...");

    if (boot_verify_app() != 0) {
        POINT_COLOR = RED;
        LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Verify FAILED!");
        return -4;
    }

    POINT_COLOR = GREEN;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "SD Upgrade OK!");
    return 0;
}

/* ======================================================================
 * 串口升级 (Ymodem 协议)
 *
 * 使用标准 Ymodem 协议, 兼容 SecureCRT / Tera Term / SSCOM 等
 * 任何支持 Ymodem 发送的串口终端.
 *
 * 流程:
 *   1. 擦除 APP 区
 *   2. 启动 UART DMA 接收
 *   3. 发送 'C' → 终端开始 Ymodem 传输
 *   4. 循环: IDLE 中断收数据 → 喂 ymodem 状态机 → 回 ACK/NAK
 *   5. 传输完成 → 写元数据 → 校验
 * ====================================================================== */
int iap_load_from_uart(void)
{
    uint8_t tx_data[2];
    static char str_buf[40];

    /* 1. 初始化 Ymodem 并擦除 APP 区 */
    ymodem_init(ymodem_flash_cb, ymodem_done_cb);
    g_ymodem_total = 0;

    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Erasing...");
    erase_app_region();

    /* 2. 启动 DMA 循环接收 */
    HAL_UART_DMAStop(&huart1);
    app_len      = 0;
    is_receiving = 0;
    HAL_UART_Receive_DMA(&huart1, app_sram_buf, APP_MAX_SIZE);

    /* 3. 发送 'C' 发起 Ymodem 传输 */
    ymodem_reset();
    int tx_len = ymodem_get_tx_data(tx_data);
    if (tx_len > 0) {
        HAL_UART_Transmit(&huart1, tx_data, tx_len, 100);
    }

    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Waiting Ymodem sender...");

    /* 4. 主循环 */
    while (1) {
        uint32_t now = HAL_GetTick();

        /* 初始握手阶段: 如果还没收到任何数据, 每隔500ms重发 'C', 一直等上位机 */
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

        /* IDLE 中断通知: 有新的 DMA 数据到达 */
        if (is_receiving) {
            /*
             * ★ 关键: 先快速冻结 DMA 并读取数据量, 然后立刻重启 DMA.
             *    这样 DMA 只停极短时间, 上位机发送的下一包数据不会丢失.
             *    ymodem 解析和 Flash 写入在后面进行, DMA 持续运行.
             */
            __disable_irq();
            is_receiving = 0;
            HAL_UART_DMAStop(&huart1);
            uint32_t ndtr      = hdma_usart1_rx.Instance->NDTR;
            uint32_t new_bytes  = APP_MAX_SIZE - ndtr - app_len;
            if (new_bytes > 0)
                app_len += new_bytes;
            uint32_t cur_len = app_len;
            __enable_irq();

            /* 喂给 Ymodem 状态机 (纯解析, 不写 Flash) */
            int consumed = ymodem_feed_buffer(app_sram_buf, cur_len);
            if (consumed < 0) {
                POINT_COLOR = RED;
                LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Ymodem Error!");
                HAL_UART_DMAStop(&huart1);
                return -2;
            }

            /* 移动剩余未处理数据到缓冲区头部 */
            uint32_t remain = cur_len - (uint32_t)consumed;
            if (remain > 0) {
                memmove(app_sram_buf, &app_sram_buf[consumed], remain);
                app_len = remain;
            } else {
                app_len = 0;
            }

            /*
             * ★ 先重启 DMA, 再发送 ACK.
             *    这样上位机收到 ACK 后立刻发送下一包时,
             *    MCU 的 DMA 已经在运行, 不会丢数据.
             */
            HAL_UART_Receive_DMA(&huart1, &app_sram_buf[app_len], APP_MAX_SIZE - app_len);

            /* 发送 ACK/NAK/C 响应 */
            tx_len = ymodem_get_tx_data(tx_data);
            if (tx_len > 0) {
                HAL_UART_Transmit(&huart1, tx_data, tx_len, 100);
            }

            /* 进度显示 */
            {
                uint32_t total = ymodem_get_total_size();
                snprintf(str_buf, sizeof(str_buf), "UART RX: %lu bytes", (unsigned long)total);
                POINT_COLOR = BLUE;
                LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, (uint8_t*)str_buf);
            }
        }

        /* 检查 Ymodem 状态 */
        YmodemState_t ys = ymodem_get_state();
        if (ys == YMODEM_STATE_DONE) {
            HAL_UART_DMAStop(&huart1);
            break;
        }
        if (ys == YMODEM_STATE_ERROR || ys == YMODEM_STATE_CANCEL) {
            POINT_COLOR = RED;
            LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Ymodem Error!");
            HAL_UART_DMAStop(&huart1);
            return -2;
        }
    }

    /* 5. 写入固件元数据并校验 */
    {
        uint32_t calc_crc = boot_crc32((const uint8_t*)FLASH_APP_ADDR, g_ymodem_total);
        FirmwareHeader_t header;
        header.magic         = FIRMWARE_MAGIC;
        header.firmware_size = g_ymodem_total;
        header.firmware_crc  = calc_crc;
        memset(header.reserved, 0xFF, sizeof(header.reserved));

        uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
        flash_write(meta_addr, (const uint8_t*)&header, sizeof(header));
    }

    POINT_COLOR = BLACK;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Verifying...");

    if (boot_verify_app() != 0) {
        POINT_COLOR = RED;
        LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "Verify Failed!");
        return -3;
    }

    POINT_COLOR = GREEN;
    LCD_ShowString(LCD_X, LCD_LINE_S3, 280, LCD_FONT_H, LCD_FONT_W, "UART Upgrade OK!");
    return 0;
}