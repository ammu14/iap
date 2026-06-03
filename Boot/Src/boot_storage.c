/**
 * @file    boot_storage.c
 * @brief   Boot 参数存储 — 基于 W25Q128 外部 SPI Flash
 * @note    将运行时可调参数持久化到 W25Q128 扇区 0 (4KB),
 *          带 CRC32 完整性校验, 数据损坏时自动恢复默认值。
 */

#include "boot_storage.h"
#include "w25Q128.h"
#include "boot_verify.h"   /* boot_crc32() */
#include <string.h>

/*----------------------------------------------------------------------------
 * Local helpers
 *----------------------------------------------------------------------------*/

/**
 * @brief  加载默认参数值
 */
static void boot_params_set_defaults(BootParams_t *p)
{
    memset(p, 0, sizeof(BootParams_t));

    p->magic              = BOOT_PARAMS_MAGIC;
    p->version            = 1;
    p->boot_mode          = BOOT_MODE_AUTO;
    p->upgrade_state      = UPGRADE_IDLE;
    p->last_upgrade_result = 0;
    p->boot_attempt_count  = 0;
    p->boot_attempt_max    = 3;
    p->firmware_version    = 0;
    p->firmware_size       = 0;
    p->slot_a_fw_size      = 0;
    p->slot_b_fw_size      = 0;
    p->slot_a_state        = SLOT_STATE_EMPTY;
    p->slot_b_state        = SLOT_STATE_EMPTY;
    p->hardware_rev        = 0;

    /* device_serial 保持全零, 可由外部写入 */
    memset(p->device_serial, 0, sizeof(p->device_serial));
    memset(p->reserved2, 0, sizeof(p->reserved2));
}

/**
 * @brief  计算参数结构体的 CRC32 (跳过 magic 和 crc 字段).
 * @note   从 version 字段开始计算, 覆盖到 reserved2 末尾.
 */
static uint32_t boot_params_calc_crc(const BootParams_t *p)
{
    /* 跳过 magic(4) + crc(4) = 8 字节, 其余全部参与 CRC */
    const uint8_t *data = (const uint8_t *)p;
    uint32_t len = sizeof(BootParams_t) - 8;

    return boot_crc32(data + 8, len);
}

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * @brief  初始化参数区
 */
int boot_storage_init(BootParams_t *p)
{
    if (p == NULL) {
        return -1;
    }

    int ret = boot_storage_load(p);
    if (ret != 0) {
        /* 首次上电或数据损坏 → 写入默认值 */
        boot_params_set_defaults(p);
        p->crc = boot_params_calc_crc(p);

        if (boot_storage_save(p) != 0) {
            /* W25Q128 硬件错误, 参数保留在 RAM 中使用默认值 */
            return -1;
        }
    }

    return 0;
}

/**
 * @brief  从 W25Q128 读取参数并校验 CRC
 */
int boot_storage_load(BootParams_t *p)
{
    if (p == NULL) {
        return -1;
    }

    W25Q128_ReadData(BOOT_PARAMS_ADDR, (uint8_t *)p, sizeof(BootParams_t));

    /* 检查魔数 */
    if (p->magic != BOOT_PARAMS_MAGIC) {
        return -1;
    }

    /* 校验 CRC */
    uint32_t calc_crc = boot_params_calc_crc(p);
    if (calc_crc != p->crc) {
        return -1;
    }

    return 0;
}

/**
 * @brief  保存参数到 W25Q128
 */
int boot_storage_save(BootParams_t *p)
{
    if (p == NULL) {
        return -1;
    }

    p->magic = BOOT_PARAMS_MAGIC;
    p->crc   = boot_params_calc_crc(p);

    /* 使用高级写接口, 自动处理扇区擦除和跨页写入 */
    W25Q128_WRITE((uint8_t *)p, BOOT_PARAMS_ADDR, sizeof(BootParams_t));

    /* 读回校验 */
    BootParams_t verify_buf;
    W25Q128_ReadData(BOOT_PARAMS_ADDR, (uint8_t *)&verify_buf, sizeof(BootParams_t));
    if (memcmp(p, &verify_buf, sizeof(BootParams_t)) != 0) {
        return -1;
    }

    return 0;
}