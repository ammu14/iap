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

/*
 * SD 卡升级: 挂载 SD → 读 app.bin → 擦除 → 写入 → 写元数据 → 校验
 * 返回 0 成功, 负数失败
 */
int iap_load_from_sd(void);

/*
 * 串口升级: 初始化协议 → 启动 DMA 接收 → 循环解析协议包 → 校验
 * 返回 0 成功, 负数失败
 */
int iap_load_from_uart(void);

#ifdef __cplusplus
}
#endif

#endif /* __IAP_H__ */