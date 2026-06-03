#ifndef __BOOT_STORAGE_H__
#define __BOOT_STORAGE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------
 * W25Q128 参数存储布局
 *----------------------------------------------------------------------------
 * 扇区 0  (0x000000 - 0x000FFF,  4KB): Boot 参数区
 * 扇区 1+ (0x001000 - 0x0FFFFF, ~1MB): 备份固件区 (预留)
 * 剩余    (0x100000 - 0xFFFFFF, 15MB): 应用数据区 (预留)
 *----------------------------------------------------------------------------*/
#define BOOT_PARAMS_ADDR          0x00000000UL
#define BOOT_PARAMS_SECTOR_SIZE   4096UL
#define BOOT_PARAMS_MAGIC         0x5041524DUL   /* "PARM" */

/*----------------------------------------------------------------------------
 * 启动模式
 *----------------------------------------------------------------------------*/
typedef enum {
    BOOT_MODE_AUTO      = 0,   /* 自动: 有有效 APP 则直接启动 */
    BOOT_MODE_SD        = 1,   /* SD 卡升级模式 */
    BOOT_MODE_UART      = 2,   /* UART Ymodem 升级模式 */
    BOOT_MODE_RECOVERY  = 3,   /* 恢复模式: 从备份固件恢复 */
} BootMode_t;

/*----------------------------------------------------------------------------
 * 升级状态
 *----------------------------------------------------------------------------*/
typedef enum {
    UPGRADE_IDLE        = 0,   /* 空闲 */
    UPGRADE_DOWNLOADING = 1,   /* 正在下载 */
    UPGRADE_VERIFYING   = 2,   /* 正在校验 */
    UPGRADE_SUCCESS     = 3,   /* 升级成功 */
    UPGRADE_FAILED      = 4,   /* 升级失败 */
} UpgradeState_t;

/*----------------------------------------------------------------------------
 * 激活槽位
 *----------------------------------------------------------------------------*/
typedef enum {
    BOOT_SLOT_A = 0,            /* Slot A: 0x08010000 (可执行) */
    BOOT_SLOT_B = 1             /* Slot B: 0x08080000 (可执行) */
} BootSlot_t;

/*----------------------------------------------------------------------------
 * 槽位固件状态 (存储在 W25Q128 的 slot_state 中)
 *----------------------------------------------------------------------------*/
typedef enum {
    SLOT_STATE_EMPTY    = 0,    /* 槽位为空 / 无有效固件 */
    SLOT_STATE_NEW      = 1,    /* 刚写入, 待启动确认 */
    SLOT_STATE_CONFIRMED = 2,   /* 已确认可启动 */
} SlotState_t;

/*----------------------------------------------------------------------------
 * Boot 参数结构体 (4KB, 需与扇区大小一致)
 *----------------------------------------------------------------------------*/
typedef struct {
    /* ---- 完整性校验 ---- */
    uint32_t magic;                 /* 魔数 BOOT_PARAMS_MAGIC            */
    uint32_t crc;                   /* 除 magic/crc 外其余字段的 CRC32  */
    uint32_t version;               /* 参数结构体版本号                  */

    /* ---- 启动控制 ---- */
    uint8_t  boot_mode;             /* BootMode_t                        */
    uint8_t  upgrade_state;         /* UpgradeState_t                    */
    uint8_t  last_upgrade_result;   /* 0=成功, 非0=失败码               */
    uint8_t  active_slot;           /* BootSlot_t: 当前活跃槽            */

    uint32_t boot_attempt_count;    /* 当前启动尝试次数                  */
    uint32_t boot_attempt_max;      /* 最大启动尝试次数 (默认 3)         */

    /* ---- 固件信息 ---- */
    uint32_t firmware_version;      /* 上次成功烧录的固件版本            */
    uint32_t firmware_size;         /* 上次成功烧录的固件大小            */

    /* ---- A/B 槽固件大小 + 状态 (用于 A/B 切换) ---- */
    uint32_t slot_a_fw_size;        /* Slot A 固件大小 (字节)            */
    uint32_t slot_b_fw_size;        /* Slot B 固件大小 (字节)            */
    uint8_t  slot_a_state;          /* SlotState_t: Slot A 状态         */
    uint8_t  slot_b_state;          /* SlotState_t: Slot B 状态         */
    uint8_t  resv_pad[2];           /* 对齐填充                        */

    /* ---- 设备信息 ---- */
    uint8_t  device_serial[16];     /* 设备序列号                        */
    uint32_t hardware_rev;          /* 硬件版本号                        */

    /* ---- 填充 (保持 4KB 结构体大小不变) ---- */
    uint8_t  reserved2[4012];       /* 保留, 填充到扇区大小              */
} BootParams_t;

/* 编译期断言: 结构体大小不能超过一个扇区 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(BootParams_t) <= BOOT_PARAMS_SECTOR_SIZE,
               "BootParams_t exceeds sector size");
#endif

/*----------------------------------------------------------------------------
 * API
 *----------------------------------------------------------------------------*/

/**
 * @brief  初始化参数区 (上电时调用一次)
 * @note   从 W25Q128 读取参数并校验 CRC。
 *         若校验失败 (首次上电/数据损坏), 自动写入默认值。
 * @param  p: 指向调用方提供的 BootParams_t 缓冲区
 * @retval 0=成功, -1=W25Q128 硬件错误
 */
int boot_storage_init(BootParams_t *p);

/**
 * @brief  保存参数到 W25Q128 (自动擦除 + CRC 计算)
 * @param  p: 指向待保存的参数
 * @retval 0=成功, -1=写入失败
 */
int boot_storage_save(BootParams_t *p);

/**
 * @brief  从 W25Q128 读取参数并校验 CRC
 * @param  p: 指向接收缓冲区
 * @retval 0=成功, -1=CRC 校验失败或读取错误
 */
int boot_storage_load(BootParams_t *p);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_STORAGE_H__ */