#ifndef __BOOT_VERIFY_H__
#define __BOOT_VERIFY_H__

#include <stdint.h>
#include "boot_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 固件元数据结构 —— 放在 APP 区末尾 32 字节
 * 布局 (FLASH_APP_META_OFFSET 偏移处):
 *   [0:3]   魔数     FIRMWARE_MAGIC
 *   [4:7]   固件本体大小(字节)
 *   [8:11]  固件本体 CRC32
 *   [12:31] 保留 (填充 0xFF)
 *
 * 这样固件本体可以直接占用 0x08010000 起始地址,
 * 向量表 (SP + ResetVector) 不受影响, APP 无需重定位 VTOR.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;           /* 魔数, 必须等于 FIRMWARE_MAGIC */
    uint32_t firmware_size;   /* 固件本体字节数 */
    uint32_t firmware_crc;    /* 固件本体 CRC32 */
    uint8_t  reserved[20];    /* 保留, 凑够 32 字节 */
} FirmwareHeader_t;


/*
 * CRC32 计算 (软件实现)
 *   buf  - 数据指针
 *   len  - 字节数
 * 返回 CRC32 值
 */
uint32_t boot_crc32(const uint8_t *buf, uint32_t len);

/*
 * 综合校验: 读头 + 验魔数 + 验 CRC
 * 返回 0 则 APP 合法, 可以跳转
 */
int boot_verify_app(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_VERIFY_H__ */