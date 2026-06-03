#ifndef __IAP_H__
#define __IAP_H__

#include <stdint.h>
#include "boot_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iap_load_to_slot 的 mode 参数 */
#define IAP_MODE_SD    0
#define IAP_MODE_UART  1

/*
 * 通用: 跳转到指定 APP 地址执行
 *
 * 这是 Bootloader 最底层的跳转原语:
 *   - 检查 SP 是否落在 SRAM 范围 (0x20000000~0x20020000)
 *   - 关中断 → 设 MSP → 跳转复位向量
 *   - SP 非法时静默返回, 由调用者处理错误
 *
 * 调用者 (boot_manager.c) 应在跳转前通过 boot_verify_app() 校验固件完整性.
 */
void iap_load_app(uint32_t appxaddr);

/*
 * 统一升级 API: 将固件烧录到指定槽
 *
 *   slot  - BOOT_SLOT_A (0) 或 BOOT_SLOT_B (1)
 *   mode  - IAP_MODE_SD (0) 或 IAP_MODE_UART (1)
 *
 * 返回 0 成功, 负数失败.
 * 升级成功后固件元数据已写入目标槽末尾.
 */
int iap_load_to_slot(uint8_t slot, uint8_t mode);

/*
 * SD 卡升级 (兼容旧 API, 等同于 iap_load_to_slot(BOOT_SLOT_B, IAP_MODE_SD))
 * 返回 0 成功, 负数失败
 */
int iap_load_from_sd(void);

/*
 * 串口升级 (兼容旧 API, 等同于 iap_load_to_slot(BOOT_SLOT_B, IAP_MODE_UART))
 * 返回 0 成功, 负数失败
 */
int iap_load_from_uart(void);

#ifdef __cplusplus
}
#endif

#endif /* __IAP_H__ */