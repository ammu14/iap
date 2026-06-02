#include "iap.h"
#include "boot_config.h"
#include "boot_verify.h"
#include "boot_protocol.h"
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
 *
 * 这是 Bootloader 最底层的跳转原语:
 *   1. 检查 SP 是否落在 SRAM 范围 (0x20000000 ~ 0x20020000)
 *   2. 关全局中断 → 设置 MSP → 跳转到 APP 复位向量
 *   3. 若 SP 非法则静默返回, 由调用者处理错误
 *
 * 上层 boot_manager.c 在跳转前应通过 boot_verify_app() 校验固件完整性.
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

/*
 * 构建一个协议包到 buf 中 (与上位机 flash_tool.py 格式完全一致)
 * 返回实际包长度
 */
static uint32_t build_protocol_packet(uint8_t *buf, uint8_t cmd, uint32_t addr,
                                       const uint8_t *data, uint32_t data_len)
{
    /* buf 布局: header(1) + cmd(1) + addr(4) + len(4) + data(data_len) + crc(4) + tail(1) */
    buf[0] = PACKET_HEADER;
    buf[1] = cmd;
    *(uint32_t *)(buf + 2)  = addr;
    *(uint32_t *)(buf + 6)  = data_len;
    if (data_len > 0 && data != NULL) {
        memcpy(buf + 10, data, data_len);
    }
    /* CRC32 覆盖: cmd + addr + len + data */
    uint32_t payload_len = 1 + 4 + 4 + data_len;   /* cmd(1) + addr(4) + len(4) + data */
    uint32_t calc_crc = boot_crc32(buf + 1, payload_len);
    uint32_t crc_offset = 10 + data_len;
    *(uint32_t *)(buf + crc_offset) = calc_crc;
    buf[crc_offset + 4] = PACKET_TAIL;

    return crc_offset + 5;  /* 总长度: 1 + payload_len + 4(crc) + 1(tail) */
}

/* ======================================================================
 * SD 卡升级
 *
 * 与 UART 升级共享同一套协议处理:
 *   1. 挂载 SD, 打开 app.bin
 *   2. 初始化协议栈 → 执行擦除
 *   3. 分块读取文件 → 组协议包 → boot_protocol_process() 写入
 *   4. 发 VERIFY 命令 → 由协议栈校验
 *
 * 协议帧格式: Header(0xAA) + Cmd + Addr + Len + Data + CRC32 + Tail(0x55)
 * 与 UART 路径和 flash_tool.py 完全一致.
 * ====================================================================== */
int iap_load_from_sd(void) 
{
    FRESULT res;
    FIL fil;
    UINT br;
    uint32_t flash_addr = FLASH_APP_ADDR;
    uint8_t read_buf[BOOT_PACKET_DATA_SIZE];
    uint8_t pkt_buf[BOOT_PACKET_DATA_SIZE + 15];  /* 最大包缓冲区 */
    static char str_buf[40];
    
    // 挂载
    res = f_mount(&SDFatFS, SDPath, 1);
    if (res != FR_OK) {
        LCD_ShowString(30, 210, 200, 16, 16, "SD mount FAIL!        ");
        return -1;
    }

    // 打开文件
    res = f_open(&fil, "app.bin", FA_READ);
    if (res != FR_OK) {
        LCD_ShowString(30, 210, 200, 16, 16, "app.bin not found!    ");
        f_mount(NULL, SDPath, 1);
        return -2;
    }

    FSIZE_t fsize = f_size(&fil);
    if(fsize > FLASH_APP_SIZE - FLASH_APP_META_SIZE) 
    {
        LCD_ShowString(30, 210, 200, 16, 16, "File too large!       ");
        f_close(&fil);
        f_mount(NULL, SDPath, 1);
        return -3;
    }
    
    snprintf(str_buf, sizeof(str_buf), "File size: %lu bytes  ", (unsigned long)fsize);
    LCD_ShowString(30, 190, 200, 16, 16, (uint8_t *)str_buf);
    
    /* 初始化协议栈, 通过协议栈走 ERASE→WRITING→WRITE→VERIFY 完整流程 */
    boot_protocol_init();

    {
        /* 1. 发送 SYNC 包 → 协议栈进入 PROT_STATE_SYNCED */
        uint32_t sync_len = build_protocol_packet(pkt_buf, CMD_SYNC, 0, NULL, 0);
        boot_protocol_process(pkt_buf, sync_len);
    }

    {
        /* 2. 发送 ERASE 包 → PROT_STATE_ERASE_PENDING,
         *    然后调用 boot_protocol_perform_erase() 执行实际擦除 + 切到 WRITING.
         *    SD 路径下擦除是同步的, 不需要 DMA 延迟擦除机制. */
        uint32_t erase_len = build_protocol_packet(pkt_buf, CMD_ERASE, 0, NULL, 0);
        boot_protocol_process(pkt_buf, erase_len);
        boot_protocol_perform_erase();  /* 内部调用 erase_app_region(), → PROT_STATE_WRITING */
    }

    /* 3. 逐块读取文件, 组 CMD_WRITE 包 → 协议栈统一写入 Flash */
    while (f_read(&fil, read_buf, sizeof(read_buf), &br) == FR_OK && br > 0) {
        uint32_t pkt_len = build_protocol_packet(pkt_buf, CMD_WRITE, flash_addr,
                                                  read_buf, br);
        boot_protocol_process(pkt_buf, pkt_len);
        flash_addr += br;

        /* 进度显示 */
        snprintf(str_buf, sizeof(str_buf), "Writing %lu/%lu       ",
                 (unsigned long)(flash_addr - FLASH_APP_ADDR), (unsigned long)fsize);
        LCD_ShowString(30, 210, 200, 16, 16, (uint8_t *)str_buf);

        if (br < sizeof(read_buf))
            break;
    }

    f_close(&fil);
    f_mount(NULL, SDPath, 1);

    /* 4. 构建并写入固件元数据 (FirmwareHeader_t) 到 APP 区末尾
     *    CRC 从刚写入的 Flash 中回读计算, 与旧 SD 路径行为一致 */
    {
        uint32_t cal_crc = boot_crc32((const uint8_t *)FLASH_APP_ADDR, (uint32_t)fsize);
        FirmwareHeader_t header;
        header.magic = FIRMWARE_MAGIC;
        header.firmware_size = (uint32_t)fsize;
        header.firmware_crc = cal_crc;
        memset(header.reserved, 0xFF, sizeof(header.reserved));

        uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
        uint32_t meta_len = build_protocol_packet(pkt_buf, CMD_WRITE, meta_addr,
                                                   (uint8_t *)&header, sizeof(header));
        boot_protocol_process(pkt_buf, meta_len);
    }

    /* 5. 发送 VERIFY 包 → 协议栈调用 boot_verify_app() 校验 */
    {
        uint32_t verify_len = build_protocol_packet(pkt_buf, CMD_VERIFY, 0, NULL, 0);
        boot_protocol_process(pkt_buf, verify_len);
    }

    if (boot_protocol_get_state() == PROT_STATE_DONE) {
        LCD_ShowString(30, 210, 200, 16, 16, "SD Upgrade OK!        ");
        return 0;
    }
    
    LCD_ShowString(30, 210, 200, 16, 16, "Verify FAILED!        ");
    return -4;
}

/* ======================================================================
 * 串口升级
 * 初始化协议栈 → 启动 DMA 接收 → 循环解析协议包 → 校验 → 完成
 * 返回 0 成功, 负数失败
 * ====================================================================== */
int iap_load_from_uart(void)
{
    uint32_t start_tick = HAL_GetTick();

    /* 停止之前的 DMA 循环接收, 重置缓冲区写指针 */
    HAL_UART_DMAStop(&huart1);
    app_len = 0;
    boot_protocol_init();
    HAL_UART_Receive_DMA(&huart1, app_sram_buf, APP_MAX_SIZE);

    LCD_ShowString(30, 210, 200, 16, 16, "UART Upgrade mode...");
    LCD_ShowString(30, 230, 200, 16, 16, "Send firmware via UART");

    while (1)
    {
        uint32_t now = HAL_GetTick();

        /* ---- 超时检查 ---- */
        if (now - start_tick > UPGRADE_TIMEOUT_MS) {
            LCD_ShowString(30, 210, 200, 16, 16, "Upgrade Timeout!      ");
            HAL_UART_DMAStop(&huart1);
            return -1;
        }

        /* ---- 延迟擦除: DMA 运行期间执行实际 Flash 擦除 ---- */
        if (boot_protocol_get_state() == PROT_STATE_ERASE_PENDING) {
            boot_protocol_perform_erase();
        }

        /* ---- DMA 接收处理 ---- */
        if (is_receiving) {
            __disable_irq();
            is_receiving = 0;

            /* 停止 DMA, 冻结缓冲区, 读取当前 NDTR 计算已收字节数 */
            HAL_UART_DMAStop(&huart1);
            uint32_t ndtr = hdma_usart1_rx.Instance->NDTR;
            uint32_t new_bytes = APP_MAX_SIZE - ndtr - app_len;

            if (new_bytes > 0) {
                app_len += new_bytes;
            }

            uint32_t cur_app_len = app_len;
            __enable_irq();

            /*
             * 变长协议包解析: 从缓冲区中逐个提取完整协议包
             * 每个包格式: header(0xAA) + cmd + addr + len + data + crc + tail(0x55)
             * 最小包 = 15 字节, 数据包 = 15 + data_len
             */
            uint32_t offset = 0;
            while (offset + 15 <= cur_app_len) {
                /* 寻找帧头 0xAA */
                if (app_sram_buf[offset] != PACKET_HEADER) {
                    offset++;
                    continue;
                }

                /* 读取 data_len 字段 (位于 offset + 6, 4 字节 little-endian) */
                uint32_t data_len = *(uint32_t *)(&app_sram_buf[offset + 6]);
                if (data_len > BOOT_PACKET_DATA_SIZE) {
                    offset++;
                    continue;
                }

                uint32_t total_pkt_len = 15 + data_len;
                if (offset + total_pkt_len > cur_app_len) {
                    break;  /* 包不完整, 等待下次接收 */
                }

                /* 检查帧尾 */
                if (app_sram_buf[offset + total_pkt_len - 1] != PACKET_TAIL) {
                    offset++;
                    continue;
                }

                /* 解析出一个完整包, 交给协议栈处理 */
                boot_protocol_process(&app_sram_buf[offset], total_pkt_len);
                offset += total_pkt_len;
            }

            /*
             * 解析完毕，处理剩余的半包数据并重启 DMA
             */
            uint32_t remain = cur_app_len - offset;
            if (remain > 0 && remain < cur_app_len) {
                memmove(app_sram_buf, &app_sram_buf[offset], remain);
                app_len = remain;
            } else {
                app_len = 0;
            }
            /* 重启 DMA，从 app_len 偏移处继续接收 */
            HAL_UART_Receive_DMA(&huart1, &app_sram_buf[app_len], APP_MAX_SIZE - app_len);
        }

        /* ---- 升级完成 ---- */
        if (boot_protocol_get_state() == PROT_STATE_DONE) {
            HAL_UART_DMAStop(&huart1);
            break;
        }

        /* ---- 协议错误 ---- */
        if (boot_protocol_get_state() == PROT_STATE_ERROR) {
            LCD_ShowString(30, 210, 200, 16, 16, "Protocol Error!       ");
            HAL_UART_DMAStop(&huart1);
            return -2;
        }
    }

    /* 校验固件 */
    if (boot_verify_app() != 0) {
        LCD_ShowString(30, 210, 200, 16, 16, "Verify Failed!        ");
        return -3;
    }

    LCD_ShowString(30, 210, 200, 16, 16, "UART Upgrade OK!      ");
    return 0;
}