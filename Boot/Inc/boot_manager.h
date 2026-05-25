#ifndef __BOOT_MANAGER_H__
#define __BOOT_MANAGER_H__

#include <stdint.h>
#include "boot_config.h"
#include "boot_verify.h"
#include "boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bootloader 运行阶段 */
typedef enum {
    BOOT_PHASE_STARTUP,         /* 上电初始化 */
    BOOT_PHASE_CHECK_APP,       /* 检查已有 APP */
    BOOT_PHASE_IDLE,            /* 空闲等待 */
    BOOT_PHASE_UPGRADING,       /* 固件升级中 */
    BOOT_PHASE_VERIFYING,       /* 升级后校验 */
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
 * 请求进入升级模式 (由按键等外部事件触发)
 */
void boot_manager_request_upgrade(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_MANAGER_H__ */