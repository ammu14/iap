#ifndef __IAP_H__
#define __IAP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 跳转到 APP 区执行
 *
 * 这是 Bootloader 最底层的跳转原语:
 *   - 检查 SP 是否落在 SRAM 范围 (0x20000000~0x20020000)
 *   - 关中断 → 设 MSP → 跳转复位向量
 *   - SP 非法时静默返回, 由调用者处理错误
 *
 * 调用者 (boot_manager.c) 应在跳转前通过 boot_verify_app() 校验固件完整性.
 */
void iap_load_app(uint32_t appxaddr);

#ifdef __cplusplus
}
#endif

#endif /* __IAP_H__ */