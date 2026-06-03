#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Bootloader 版本号 */
#define BOOT_VERSION          "v2.0.0"

/*----------------------------------------------------------------------------
 * 真正 A/B 双槽分区布局
 *----------------------------------------------------------------------------
 * 0x08000000  Bootloader     64KB  (Sector 0~3)
 * 0x08010000  Slot A         448KB (Sector 4~7)   ← 可执行
 * 0x08080000  Slot B         512KB (Sector 8~11)  ← 可执行 (有效固件 ≤448KB)
 *
 * 两个槽均可独立执行固件.
 * 升级始终写入非活跃槽, 启动后通过 attempt 机制确认, 失败自动回滚.
 *----------------------------------------------------------------------------*/
#define FLASH_BOOT_ADDR        0x08000000UL
#define FLASH_BOOT_SIZE        0x00010000UL   /* 64KB */

#define FLASH_SLOT_A_ADDR      0x08010000UL
#define FLASH_SLOT_A_SIZE      0x00070000UL   /* 448KB (Sector 4~7) */

#define FLASH_SLOT_B_ADDR      0x08080000UL
#define FLASH_SLOT_B_SIZE      0x00080000UL   /* 512KB 物理 (Sector 8~11), 有效固件 ≤448KB */

/* 固件元数据 (放在每个槽末尾 32 字节) */
#define FLASH_APP_META_SIZE    32
#define FLASH_SLOT_A_META_ADDR (FLASH_SLOT_A_ADDR + FLASH_SLOT_A_SIZE - FLASH_APP_META_SIZE)
#define FLASH_SLOT_B_META_ADDR (FLASH_SLOT_B_ADDR + FLASH_SLOT_B_SIZE - FLASH_APP_META_SIZE)

/* 最大固件大小 = min(SlotA有效, SlotB有效) - 元数据 */
#define FLASH_APP_MAX_SIZE     (FLASH_SLOT_A_SIZE - FLASH_APP_META_SIZE)  /* 448KB - 32B */

/* 固件魔数 */
#define FIRMWARE_MAGIC        0x544F4F42UL   /* "BOOT" */

/* 升级超时 (ms) */
#define UPGRADE_TIMEOUT_MS    30000UL        /* 30秒无数据则升级超时 */

/* 启动等待超时 (ms) */
#define BOOT_WAIT_TIMEOUT_MS  3000UL         /* 3秒超时自动跳转 APP */

/* 启动尝试等待 (ms): APP 运行此时间后由 APP 调用 boot_confirm API 确认 */
#define BOOT_CONFIRM_TIMEOUT_MS  10000UL     /* 10秒内 APP 需确认 */

/* 串口通信参数 (波特率在 CubeMX 中配置, 此处仅保留协议参数) */
#define BOOT_PACKET_DATA_SIZE 1024           /* 每包数据大小 */

/*============================================================================
 * 槽位辅助宏 — 根据 slot 枚举返回地址/大小/元数据地址
 *============================================================================*/
#define BOOT_SLOT_ADDR(slot)      ((slot) == 0 ? FLASH_SLOT_A_ADDR : FLASH_SLOT_B_ADDR)
#define BOOT_SLOT_SIZE(slot)      ((slot) == 0 ? FLASH_SLOT_A_SIZE : FLASH_SLOT_B_SIZE)
#define BOOT_SLOT_META_ADDR(slot) ((slot) == 0 ? FLASH_SLOT_A_META_ADDR : FLASH_SLOT_B_META_ADDR)

/*----------------------------------------------------------------------------
 * 兼容旧代码的别名 (逐步废弃)
 *----------------------------------------------------------------------------*/
#define FLASH_APP_ADDR        FLASH_SLOT_A_ADDR
#define FLASH_APP_SIZE        FLASH_SLOT_A_SIZE
#define FLASH_APP_META_OFFSET (FLASH_SLOT_A_SIZE - FLASH_APP_META_SIZE)

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_CONFIG_H__ */