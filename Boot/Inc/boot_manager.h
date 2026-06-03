#ifndef __BOOT_MANAGER_H__
#define __BOOT_MANAGER_H__

#include <stdint.h>
#include "boot_config.h"
#include "boot_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bootloader 运行阶段 */
typedef enum {
    BOOT_PHASE_STARTUP,         /* 上电初始化 */
    BOOT_PHASE_CHECK_APP,       /* 检查已有 APP */
    BOOT_PHASE_IDLE,            /* 空闲等待 (3秒内检测按键) */
    BOOT_PHASE_SD_UPGRADE,      /* SD卡升级中 */
    BOOT_PHASE_UART_UPGRADE,    /* 串口升级中 */
    BOOT_PHASE_WAIT_KEY,        /* 无合法固件, 等待按键选择 */
    BOOT_PHASE_JUMP_APP,        /* 跳转 APP */
    BOOT_PHASE_ERROR            /* 错误状态 */
} BootPhase_t;

/*
 * Bootloader 主状态机初始化
 */
void boot_manager_init(void);

/*
 * Bootloader 主循环 (需要在 while(1) 里反复调用)
 */
void boot_manager_run(void);

/*
 * 请求进入 SD 卡升级模式 (Key0 触发)
 */
void boot_manager_request_sd_upgrade(void);

/*
 * 请求进入串口升级模式 (Key1 触发)
 */
void boot_manager_request_uart_upgrade(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_MANAGER_H__ */