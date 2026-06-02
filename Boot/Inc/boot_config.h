#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Bootloader 版本号 */
#define BOOT_VERSION          "v1.0.0"

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

/* ====== LCD 显示布局 (800x480) ====== */
#define LCD_X           30      /* 左边距 */
#define LCD_FONT_W      16      /* 字体宽度 */
#define LCD_FONT_H      16      /* 字体高度 */

/* 各行 Y 坐标 (每行间隔 20px) */
#define LCD_LINE_TITLE   50     /* 标题 */
#define LCD_LINE_VER     70     /* 版本号 */
#define LCD_LINE_DIV1    90     /* 分隔线 */
#define LCD_LINE_APP    110     /* APP 状态 */
#define LCD_LINE_KEY    130     /* 按键提示 */
#define LCD_LINE_DIV2   150     /* 分隔线 2 */
#define LCD_LINE_S1     180     /* 状态行 1 */
#define LCD_LINE_S2     200     /* 状态行 2 */
#define LCD_LINE_S3     220     /* 状态行 3 */
#define LCD_LINE_S4     240     /* 状态行 4 */

/* 区域清屏 */
#define LCD_AREA_STATUS  (190)  /* 状态区域起点 Y, 高度 80 */

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_CONFIG_H__ */