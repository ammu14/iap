#include "boot_manager.h"
#include "lcd.h"
#include "iap.h"
#include <string.h>
#include "stm32f4xx_hal.h"

static BootPhase_t current_phase = BOOT_PHASE_STARTUP;
static uint32_t   phase_start_tick = 0;
static uint8_t    pending_sd_request = 0;
static uint8_t    pending_uart_request = 0;
static uint8_t    app_is_valid = 0;

void boot_manager_init(void)
{
    current_phase = BOOT_PHASE_STARTUP;
    phase_start_tick = HAL_GetTick();
    pending_sd_request = 0;
    pending_uart_request = 0;
    app_is_valid = 0;
}

void boot_manager_request_sd_upgrade(void)
{
    if (current_phase == BOOT_PHASE_IDLE || 
        current_phase == BOOT_PHASE_WAIT_KEY || 
        current_phase == BOOT_PHASE_ERROR) {
        pending_sd_request = 1;
    }
}

void boot_manager_request_uart_upgrade(void)
{
    if (current_phase == BOOT_PHASE_IDLE || 
        current_phase == BOOT_PHASE_WAIT_KEY || 
        current_phase == BOOT_PHASE_ERROR) {
        pending_uart_request = 1;
    }
}

/*
 * 跳转到 APP
 * 前提: boot_verify_app() 已通过
 */
static void jump_to_app(void)
{
    current_phase = BOOT_PHASE_JUMP_APP;
    LCD_Fill(30, 205, 231, 221, WHITE);
    LCD_ShowString(30, 211, 224, 153, 115, "Jump...");
    iap_load_app(FLASH_APP_ADDR);
    /* 如果返回, 说明跳转失败 */
    LCD_ShowString(30, 228, 248, 172, 131, "JumpBack!");
    current_phase = BOOT_PHASE_ERROR;
}

/*
 * 显示按键提示 (LCD 复用)
 */
static void show_key_prompt(void)
{
    LCD_Fill(30, 190, 230, 246, WHITE);
    LCD_ShowString(30, 190, 200, 16, 16, "KEY0:SD  KEY1:UART");
    LCD_ShowString(30, 210, 200, 16, 16, "Waiting for key input...");
}

void boot_manager_run(void)
{
    uint32_t now = HAL_GetTick();

    switch (current_phase) {

    case BOOT_PHASE_STARTUP:
        /* 短暂停留, 让 LCD 信息显示出来 */
        if (now - phase_start_tick > 200) {
            current_phase = BOOT_PHASE_CHECK_APP;
        }
        break;

    case BOOT_PHASE_CHECK_APP:
        if (boot_verify_app() == 0) {
            app_is_valid = 1;
            LCD_Fill(30, 150, 230, 186, WHITE);
            LCD_ShowString(30, 150, 200, 16, 16, "APP: Valid");
            LCD_ShowString(30, 170, 200, 16, 16, "KEY0:SD  KEY1:UART");
        } else {
            app_is_valid = 0;
            LCD_Fill(30, 150, 230, 186, WHITE);
            LCD_ShowString(30, 150, 200, 16, 16, "APP: Invalid / Missing");
            LCD_ShowString(30, 170, 200, 16, 16, "KEY0:SD  KEY1:UART");
        }
        current_phase = BOOT_PHASE_IDLE;
        phase_start_tick = now;
        break;

    case BOOT_PHASE_IDLE:
        /*
         * 3 秒窗口内:
         *   - Key0 按下 → SD 卡烧写
         *   - Key1 按下 → 串口烧写
         *   - 3 秒超时:
         *       - 有合法 APP → 跳转
         *       - 无合法 APP → 进入 WAIT_KEY 无限等待
         */
        if (pending_sd_request) {
            pending_sd_request = 0;
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }

        /* 3 秒超时判断 */
        if (now - phase_start_tick > BOOT_WAIT_TIMEOUT_MS) {
            if (app_is_valid && boot_verify_app() == 0) {
                jump_to_app();
            } else {
                /* 无合法 APP, 进入无限等待按键模式 */
                current_phase = BOOT_PHASE_WAIT_KEY;
                LCD_Fill(30, 190, 230, 246, WHITE);
                LCD_ShowString(30, 190, 200, 16, 16, "No valid APP found!");
                show_key_prompt();
            }
        }
        break;

    case BOOT_PHASE_SD_UPGRADE:
        /*
         * SD 卡升级: 调用 iap_load_from_sd(),
         * 该函数内部完成 挂载→读文件→擦除→写入→写元数据→校验,
         * 同时负责 LCD 进度显示.
         * 返回 0 表示成功, 非 0 表示失败.
         */
        {
            int ret = iap_load_from_sd();
            if (ret == 0) {
                /* iap_load_from_sd 已显示 "SD Upgrade OK!", 直接跳转 */
                HAL_Delay(300);
                jump_to_app();
            } else {
                /* 失败时清除进度区域, 显示提示并等待按键 */
                current_phase = BOOT_PHASE_WAIT_KEY;
                LCD_Fill(30, 190, 230, 250, WHITE);
                LCD_ShowString(30, 190, 200, 16, 16, "SD Upgrade FAILED!    ");
                show_key_prompt();
            }
        }
        break;

    case BOOT_PHASE_UART_UPGRADE:
        /*
         * 串口升级: 调用 iap_load_from_uart(),
         * 该函数内部完成 协议初始化→DMA接收→解析→擦除→写入→校验,
         * 同时负责 LCD 进度显示.
         * 返回 0 表示成功, 非 0 表示失败.
         */
        {
            int ret = iap_load_from_uart();
            if (ret == 0) {
                /* iap_load_from_uart 已显示 "UART Upgrade OK!", 直接复位 */
                HAL_Delay(300);
                NVIC_SystemReset();
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                LCD_Fill(30, 190, 230, 250, WHITE);
                LCD_ShowString(30, 190, 200, 16, 16, "UART Upgrade FAILED!  ");
                show_key_prompt();
            }
        }
        break;

    case BOOT_PHASE_WAIT_KEY:
        /*
         * 无合法固件, 无限等待按键选择:
         *   - Key0 (pending_sd_request)  → SD 烧写
         *   - Key1 (pending_uart_request)→ 串口烧写
         */
        if (pending_sd_request) {
            pending_sd_request = 0;
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }
        break;

    case BOOT_PHASE_ERROR:
        /*
         * 错误状态, 等待按键选择恢复:
         *   - Key0 (pending_sd_request)  → SD 烧写
         *   - Key1 (pending_uart_request)→ 串口烧写
         */
        if (pending_sd_request) {
            pending_sd_request = 0;
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            LCD_Fill(30, 190, 230, 250, WHITE);
            break;
        }

        /* 每 500ms 刷新一次提示信息, 避免闪烁 */
        {
            static uint32_t last_lcd_refresh = 0;
            if (now - last_lcd_refresh > 500) {
                last_lcd_refresh = now;
                LCD_Fill(30, 190, 230, 266, WHITE);
                LCD_ShowString(30, 190, 200, 16, 16, "Error!               ");
                LCD_ShowString(30, 210, 200, 16, 16, "KEY0:SD  KEY1:UART   ");
                LCD_ShowString(30, 230, 200, 16, 16, "Waiting for key...   ");
            }
        }
        break;

    case BOOT_PHASE_JUMP_APP:
        /* 不会到达这里, jump_to_app 不返回 */
        break;
    }
}