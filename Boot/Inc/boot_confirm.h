#ifndef __BOOT_CONFIRM_H__
#define __BOOT_CONFIRM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * APP 侧 API: 确认当前槽位固件可正常启动
 *
 * 调用时机:
 *   APP 初始化完成后, 自检通过 (如外设正常、通信正常) 后调用.
 *
 * 行为:
 *   将当前 active_slot 的状态从 SLOT_STATE_NEW 改为 SLOT_STATE_CONFIRMED,
 *   并清零 boot_attempt_count.
 *
 * 返回 0 成功, 非0 失败.
 *
 * 注意:
 *   此函数依赖 W25Q128 外部 Flash, 需确保 SPI 已初始化.
 *   APP 在 10 秒内未调用此函数时, 下次上电 Bootloader 将视为启动失败,
 *   尝试次数用尽后自动回滚到另一个槽.
 */
int boot_confirm(void);

/*
 * 获取当前活跃槽位
 * 返回 BOOT_SLOT_A (0) 或 BOOT_SLOT_B (1)
 */
int boot_get_active_slot(void);

/*
 * 获取槽位状态
 *   slot - BOOT_SLOT_A (0) 或 BOOT_SLOT_B (1)
 * 返回 SLOT_STATE_EMPTY(0), SLOT_STATE_NEW(1), SLOT_STATE_CONFIRMED(2)
 */
int boot_get_slot_state(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_CONFIRM_H__ */