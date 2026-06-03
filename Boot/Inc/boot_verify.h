#ifndef __BOOT_VERIFY_H__
#define __BOOT_VERIFY_H__

#include <stdint.h>
#include "boot_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 固件元数据结构 —— 放在每个槽末尾 32 字节
 * 布局 (槽末尾 - 32 字节处):
 *   [0:3]   魔数     FIRMWARE_MAGIC
 *   [4:7]   固件本体大小(字节)
 *   [8:11]  固件本体 CRC32
 *   [12:31] 保留 (填充 0xFF)
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
 * 校验指定槽的固件 (兼容旧 API)
 *   slot_addr - 槽起始地址 (FLASH_SLOT_A_ADDR 或 FLASH_SLOT_B_ADDR)
 *   slot_size - 槽总大小
 *   fw_size   - [出] 固件大小, 可为 NULL
 * 返回 0 则固件合法
 */
int boot_verify_slot(uint32_t slot_addr, uint32_t slot_size, uint32_t *fw_size);

/*
 * 综合校验: 读头 + 验魔数 + 验 CRC (默认校验 Slot A)
 * 返回 0 则 APP 合法, 可以跳转
 */
int boot_verify_app(void);

/*
 * 将固件从源槽复制到目标槽 (Flash 内部复制)
 *   src_addr  - 源槽起始地址
 *   dst_addr  - 目标槽起始地址
 *   size      - 要复制的字节数 (含元数据)
 * 返回 0 成功, 非0 失败
 */
int boot_flash_copy(uint32_t src_addr, uint32_t dst_addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_VERIFY_H__ */