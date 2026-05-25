#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Bootloader 版本号 (暂未使用, 预留) */

#define FLASH_APP_ADDR        0x08010000UL   /* APP 起始地址 (Sector 4) */
#define FLASH_APP_SIZE        0x000F0000UL   /* APP 区大小: 960KB */

#define FLASH_APP_META_SIZE   32             /* 固件元数据放在 APP 区末尾, 32 字节 */
#define FLASH_APP_META_OFFSET (FLASH_APP_SIZE - FLASH_APP_META_SIZE)

/* 固件魔数 */
#define FIRMWARE_MAGIC        0x544F4F42UL   /* "BOOT" */

/* 升级超时 (ms) */
#define UPGRADE_TIMEOUT_MS    30000UL        /* 30秒无数据则升级超时 */

/* 启动等待超时 (ms) */
#define BOOT_WAIT_TIMEOUT_MS  3000UL         /* 3秒超时自动跳转 APP */

/* 串口通信参数 (波特率在 CubeMX 中配置, 此处仅保留协议参数) */
#define BOOT_PACKET_DATA_SIZE 1024           /* 每包数据大小 */

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_CONFIG_H__ */