#include "boot_manager.h"
#include "boot_protocol.h"
#include "lcd.h"
#include "usart.h"
#include "stmflash.h"
#include "main.h"
#include "iap.h"
#include <string.h>

static BootPhase_t current_phase = BOOT_PHASE_STARTUP;
static uint32_t   phase_start_tick = 0;
static uint8_t    upgrade_requested = 0;


/* 外部声明 —— DMA 接收用 (来自 usart.c 的 volatile 变量) */
extern volatile uint32_t  app_len;
extern uint8_t            app_sram_buf[];
extern volatile uint8_t   is_receiving;
extern DMA_HandleTypeDef  hdma_usart1_rx;
extern UART_HandleTypeDef huart1;

void boot_manager_init(void)
{
    current_phase = BOOT_PHASE_STARTUP;
    phase_start_tick = HAL_GetTick();
    upgrade_requested = 0;
}


void boot_manager_request_upgrade(void)
{
    if (current_phase == BOOT_PHASE_IDLE) {
        upgrade_requested = 1;
    }
}

/*
 * 跳转到 APP
 * 前提: boot_verify_app() 已通过
 * 底层跳转复用 BSP 层 iap_load_app()
 */
static void jump_to_app(void)
{
    current_phase = BOOT_PHASE_JUMP_APP;

    LCD_Fill(30, 210, 230, 226, WHITE);
    LCD_ShowString(30, 210, 200, 16, 16, "Jumping to APP...");

    /* iap_load_app 内部做 SP 合法检查, 若非法则不跳转 */
    iap_load_app(FLASH_APP_ADDR);

    /* 若 iap_load_app 返回 (SP 非法), 则进入错误状态 */
    current_phase = BOOT_PHASE_ERROR;
}

void boot_manager_run(void)
{
    uint32_t now = HAL_GetTick();

    switch (current_phase) {

    case BOOT_PHASE_STARTUP:
        /* 短暂停留, 给按键检测留时间 */
        if (now - phase_start_tick > 500) {
            current_phase = BOOT_PHASE_CHECK_APP;
        }
        break;

    case BOOT_PHASE_CHECK_APP:
        if (boot_verify_app() == 0) {
            /* 已有合法 APP */
            current_phase = BOOT_PHASE_IDLE;
            LCD_Fill(30, 170, 230, 186, WHITE);
            LCD_ShowString(30, 170, 200, 16, 16, "APP: Valid, waiting...");
        } else {
            /* APP 不存在或被损坏 */
            current_phase = BOOT_PHASE_IDLE;
            LCD_Fill(30, 170, 230, 186, WHITE);
            LCD_ShowString(30, 170, 200, 16, 16, "APP: Invalid or missing");
        }
        break;

    case BOOT_PHASE_IDLE:
        /* 等待超时或按键触发 */
        if (upgrade_requested) {
            upgrade_requested = 0;
            current_phase = BOOT_PHASE_UPGRADING;
            phase_start_tick = now;
            boot_protocol_init();
            LCD_Fill(30, 170, 230, 226, WHITE);
            LCD_ShowString(30, 210, 200, 16, 16, "Upgrade mode...");
        } else if (now - phase_start_tick > BOOT_WAIT_TIMEOUT_MS) {
            /* 超时, 自动跳转 APP (仅当 APP 合法时) */
            if (boot_verify_app() == 0) {
                jump_to_app();
            } else {
                /* 无合法 APP, 自动进入升级模式等待串口固件 */
                current_phase = BOOT_PHASE_UPGRADING;
                phase_start_tick = now;
                boot_protocol_init();
                LCD_Fill(30, 170, 230, 246, WHITE);
                LCD_ShowString(30, 210, 200, 16, 16, "No APP, upgrade mode...");
                LCD_ShowString(30, 230, 200, 16, 16, "Send firmware via UART");
            }
        }
        break;

    case BOOT_PHASE_UPGRADING:
        /*
         * 延迟擦除: 如果协议栈已收到 CMD_ERASE 并置为 ERASE_PENDING,
         * 且 DMA 正在运行 (is_receiving == 0, 说明刚刚重启过 DMA),
         * 则执行实际的 Flash 擦除操作 (~5 秒).
         * 擦除期间 DMA 持续在后台接收 WRITE 包, 不会丢数据.
         */
        if (boot_protocol_get_state() == PROT_STATE_ERASE_PENDING) {
            boot_protocol_perform_erase();
        }

        /*
         * IDLE 中断将 is_receiving 置 1, 但不操作 DMA/NDTR.
         * 此处是唯一的安全处理点: 停止 DMA → 读 NDTR →
         * 计算已收字节 → 解析协议包 → 重启 DMA.
         */
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
                    offset++;  /* 非法长度, 跳过此字节继续找 */
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
             * 将未解析完的尾部数据搬到缓冲区开头，保持 DMA 连续写入
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

        /* 检查升级超时 */
        if (now - phase_start_tick > UPGRADE_TIMEOUT_MS) {
            current_phase = BOOT_PHASE_ERROR;
            LCD_Fill(30, 210, 230, 226, WHITE);
            LCD_ShowString(30, 210, 200, 16, 16, "Upgrade Timeout!");
        }

        /* 升级完成 */
        if (boot_protocol_get_state() == PROT_STATE_DONE) {
            current_phase = BOOT_PHASE_VERIFYING;
        }
        break;

    case BOOT_PHASE_VERIFYING:
        if (boot_verify_app() == 0) {
            LCD_Fill(30, 210, 230, 226, WHITE);
            LCD_ShowString(30, 210, 200, 16, 16, "Upgrade OK, Rebooting...");
            HAL_Delay(500);
            NVIC_SystemReset();  /* 复位后由 boot_verify_app 检查并自动跳转 */
        } else {
            current_phase = BOOT_PHASE_ERROR;
            LCD_Fill(30, 210, 230, 226, WHITE);
            LCD_ShowString(30, 210, 200, 16, 16, "Verify Failed!");
        }
        break;

    case BOOT_PHASE_ERROR:
        /* 错误状态, 保持显示错误信息 */
        LCD_Fill(30, 210, 230, 266, WHITE);
        LCD_ShowString(30, 250, 200, 16, 16, "Error! Press KEY2 to retry");
        break;

    case BOOT_PHASE_JUMP_APP:
        /* 不会到达这里, jump_to_app 不返回 */
        break;
    }
}