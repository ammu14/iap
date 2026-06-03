#include "boot_confirm.h"
#include "boot_storage.h"
#include "boot_config.h"
#include <stddef.h>

/*
 * 读取当前 Boot 参数 (内部辅助)
 * 返回 0 成功, -1 失败
 */
static int boot_params_read(BootParams_t *p)
{
    if (p == NULL) return -1;
    return boot_storage_load(p);
}

/*
 * 确认当前槽位固件可正常启动
 */
int boot_confirm(void)
{
    BootParams_t params;

    if (boot_params_read(&params) != 0) {
        /* W25Q128 不可用 / 参数损坏 */
        return -1;
    }

    uint8_t active_slot = params.active_slot;

    /* 将当前活跃槽标记为 CONFIRMED */
    if (active_slot == BOOT_SLOT_A) {
        params.slot_a_state = SLOT_STATE_CONFIRMED;
    } else {
        params.slot_b_state = SLOT_STATE_CONFIRMED;
    }

    /* 清零尝试计数 */
    params.boot_attempt_count = 0;

    /* 保存回 W25Q128 */
    if (boot_storage_save(&params) != 0) {
        return -2;
    }

    return 0;
}

/*
 * 获取当前活跃槽位
 */
int boot_get_active_slot(void)
{
    BootParams_t params;

    if (boot_params_read(&params) != 0) {
        return -1;
    }

    return (int)params.active_slot;
}

/*
 * 获取指定槽位状态
 */
int boot_get_slot_state(uint8_t slot)
{
    BootParams_t params;

    if (boot_params_read(&params) != 0) {
        return -1;
    }

    if (slot == BOOT_SLOT_A) {
        return (int)params.slot_a_state;
    } else if (slot == BOOT_SLOT_B) {
        return (int)params.slot_b_state;
    }

    return -1;
}