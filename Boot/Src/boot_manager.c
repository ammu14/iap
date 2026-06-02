#include "boot_manager.h"
#include "lcd.h"
#include "iap.h"
#include "boot_config.h"
#include <string.h>
#include "stm32f4xx_hal.h"

static BootPhase_t current_phase = BOOT_PHASE_STARTUP;
static uint32_t   phase_start_tick = 0;
static uint8_t    pending_sd_request = 0;
static uint8_t    pending_uart_request = 0;
static uint8_t    app_is_valid = 0;

/* 清空状态区域 (4 行, Y: 180~260) */
static void lcd_clear_status(void)
{
    LCD_Fill(0, LCD_LINE_S1, 320, LCD_LINE_S4 + LCD_FONT_H, WHITE);
}

/* 显示一行状态信息, 自动选择行号 */
static void lcd_status(uint8_t line, uint16_t color, const char *str)
{
    uint16_t y = LCD_LINE_S1 + (line - 1) * 20;
    POINT_COLOR = color;
    LCD_ShowString(LCD_X, y, 280, LCD_FONT_H, LCD_FONT_W, (char *)str);
}

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

static void jump_to_app(void)
{
    current_phase = BOOT_PHASE_JUMP_APP;
    lcd_clear_status();
    lcd_status(1, BLUE, "Jumping to APP...");
    iap_load_app(FLASH_APP_ADDR);

    /* 跳转失败 */
    lcd_status(2, RED, "Jump Failed!");
    current_phase = BOOT_PHASE_ERROR;
}

void boot_manager_run(void)
{
    uint32_t now = HAL_GetTick();

    switch (current_phase) {

    case BOOT_PHASE_STARTUP:
        if (now - phase_start_tick > 200) {
            current_phase = BOOT_PHASE_CHECK_APP;
        }
        break;

    case BOOT_PHASE_CHECK_APP:
        lcd_clear_status();

        if (boot_verify_app() == 0) {
            app_is_valid = 1;
            lcd_status(1, GREEN, "APP: Valid");
        } else {
            app_is_valid = 0;
            lcd_status(1, RED,  "APP: Not Found");
        }

        lcd_status(2, BLACK, "KEY0:SD  KEY1:UART");
        lcd_status(3, BLACK, "Auto jump in 3s...");
        current_phase = BOOT_PHASE_IDLE;
        phase_start_tick = now;
        break;

    case BOOT_PHASE_IDLE:
        /* 按键请求直接切换 */
        if (pending_sd_request) {
            pending_sd_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "SD Upgrade Selected");
            current_phase = BOOT_PHASE_SD_UPGRADE;
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "UART Upgrade Selected");
            current_phase = BOOT_PHASE_UART_UPGRADE;
            break;
        }

        /* 超时处理 */
        if (now - phase_start_tick > BOOT_WAIT_TIMEOUT_MS) {
            if (app_is_valid && boot_verify_app() == 0) {
                jump_to_app();
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                lcd_clear_status();
                lcd_status(1, RED,  "No Valid APP!");
                lcd_status(2, BLACK, "KEY0:SD  KEY1:UART");
                lcd_status(3, BLACK, "Waiting key...");
            }
        }
        break;

    case BOOT_PHASE_SD_UPGRADE:
        {
            lcd_clear_status();
            lcd_status(1, BLACK, "SD Upgrade Starting...");
            int ret = iap_load_from_sd();
            if (ret == 0) {
                HAL_Delay(500);
                jump_to_app();
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                lcd_clear_status();
                lcd_status(1, RED,  "SD Upgrade FAILED!");
                lcd_status(2, BLACK, "KEY0:SD  KEY1:UART");
                lcd_status(3, BLACK, "Retry?");
            }
        }
        break;

    case BOOT_PHASE_UART_UPGRADE:
        {
            lcd_clear_status();
            lcd_status(1, BLACK, "UART Upgrade Starting...");
            int ret = iap_load_from_uart();
            if (ret == 0) {
                HAL_Delay(500);
                jump_to_app();
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                lcd_clear_status();
                lcd_status(1, RED,  "UART Upgrade FAILED!");
                lcd_status(2, BLACK, "KEY0:SD  KEY1:UART");
                lcd_status(3, BLACK, "Retry?");
            }
        }
        break;

    case BOOT_PHASE_WAIT_KEY:
        if (pending_sd_request) {
            pending_sd_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "SD Upgrade Selected");
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "UART Upgrade Selected");
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            break;
        }
        break;

    case BOOT_PHASE_ERROR:
        if (pending_sd_request) {
            pending_sd_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "SD Upgrade Selected");
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            break;
        }

        if (pending_uart_request) {
            pending_uart_request = 0;
            lcd_clear_status();
            lcd_status(1, BLACK, "UART Upgrade Selected");
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            break;
        }

        /* 每 500ms 刷新一次 */
        {
            static uint32_t last_refresh = 0;
            if (now - last_refresh > 500) {
                last_refresh = now;
                lcd_clear_status();
                lcd_status(1, RED,  "Boot Error!");
                lcd_status(2, BLACK, "KEY0:SD  KEY1:UART");
                lcd_status(3, BLACK, "Waiting key...");
            }
        }
        break;

    case BOOT_PHASE_JUMP_APP:
        break;
    }
}