#include "boot_manager.h"
#include "iap.h"
#include "boot_storage.h"
#include <stdio.h>
#include "boot_config.h"
#include <string.h>
#include "stm32f4xx_hal.h"

static BootPhase_t   current_phase = BOOT_PHASE_STARTUP;
static uint32_t      phase_start_tick = 0;
static uint8_t       pending_sd_request = 0;
static uint8_t       pending_uart_request = 0;
static uint8_t       app_is_valid = 0;
static BootParams_t  boot_params;

/* 当前选择的启动槽 */
static BootSlot_t    current_boot_slot = BOOT_SLOT_A;

/*============================================================================
 * 内部辅助 — 槽位管理
 *============================================================================*/

/* 获取非活跃槽 */
static BootSlot_t get_inactive_slot(void)
{
    return (boot_params.active_slot == BOOT_SLOT_A) ? BOOT_SLOT_B : BOOT_SLOT_A;
}

/* 校验某个槽并回填 fw_size */
static int check_slot(BootSlot_t slot)
{
    uint32_t fw_size = 0;
    uint32_t addr = BOOT_SLOT_ADDR(slot);
    uint32_t size = BOOT_SLOT_SIZE(slot);
    int ret = boot_verify_slot(addr, size, &fw_size);
    if (ret == 0) {
        if (slot == BOOT_SLOT_A)
            boot_params.slot_a_fw_size = fw_size;
        else
            boot_params.slot_b_fw_size = fw_size;
    }
    return ret;
}

/* 获取槽位状态 */
static uint8_t get_slot_state(BootSlot_t slot)
{
    return (slot == BOOT_SLOT_A) ? boot_params.slot_a_state : boot_params.slot_b_state;
}

/* 标记槽位为 NEW (刚升级/刚写入, 待 APP 确认) */
static void mark_slot_state(BootSlot_t slot, SlotState_t state)
{
    if (slot == BOOT_SLOT_A)
        boot_params.slot_a_state = state;
    else
        boot_params.slot_b_state = state;
    boot_storage_save(&boot_params);
}

/*============================================================================
 * 跳转
 *============================================================================*/

static void jump_to_slot(BootSlot_t slot)
{
    current_phase = BOOT_PHASE_JUMP_APP;
    uint32_t addr = BOOT_SLOT_ADDR(slot);
    printf("[BOOT] Jumping to Slot %c (0x%08lX)...\r\n",
           (slot == BOOT_SLOT_A) ? 'A' : 'B', (unsigned long)addr);

    uint8_t prev_state = get_slot_state(slot);
    boot_params.active_slot = slot;

    /* 仅当槽位未被确认时才标记为 NEW 并递增尝试计数.
     * CONFIRMED 槽位保持状态不变, 允许 APP 任意次数复位而不会触发 attempt 递增. */
    if (prev_state != SLOT_STATE_CONFIRMED) {
        mark_slot_state(slot, SLOT_STATE_NEW);
        boot_params.boot_attempt_count++;
    } else {
        /* CONFIRMED 槽位: 保持 CONFIRMED, 清零尝试计数以防残留值 */
        boot_params.boot_attempt_count = 0;
    }
    boot_storage_save(&boot_params);

    iap_load_app(addr);

    /* 跳转失败 */
    printf("[BOOT] Jump Failed!\r\n");
    current_phase = BOOT_PHASE_ERROR;
}

/*============================================================================
 * A/B 启动决策核心
 *============================================================================*/

/*
 * 决策: 选哪个槽启动
 * 返回值: 0=成功选定, -1=无有效槽
 */
static int ab_decide_boot_slot(void)
{
    BootSlot_t prev_slot = (BootSlot_t)boot_params.active_slot;
    uint8_t    prev_state = get_slot_state(prev_slot);

    printf("[BOOT] A/B: prev=%c state=%d attempt=%lu/%lu\r\n",
           (prev_slot == BOOT_SLOT_A) ? 'A' : 'B',
           prev_state,
           (unsigned long)boot_params.boot_attempt_count,
           (unsigned long)boot_params.boot_attempt_max);

    /*--- 情况1: 上次槽是 NEW → APP 未确认 (可能启动失败) ---*/
    if (prev_state == SLOT_STATE_NEW) {
        if (boot_params.boot_attempt_count >= boot_params.boot_attempt_max) {
            /* 尝试次数用尽 → 回退到另一槽 */
            printf("[BOOT] Max attempts, fallback...\r\n");
            BootSlot_t fallback = get_inactive_slot();

            /* 标记当前槽为空 */
            mark_slot_state(prev_slot, SLOT_STATE_EMPTY);

            if (check_slot(fallback) == 0 && get_slot_state(fallback) == SLOT_STATE_CONFIRMED) {
                printf("[BOOT] Fallback to Slot %c\r\n",
                       (fallback == BOOT_SLOT_A) ? 'A' : 'B');
                current_boot_slot = fallback;
                boot_params.active_slot = fallback;
            } else {
                printf("[BOOT] No valid fallback!\r\n");
                current_boot_slot = fallback;
                boot_params.active_slot = fallback;
            }
            boot_params.boot_attempt_count = 0;
            boot_storage_save(&boot_params);
            return 0;
        }
        /* 仍有尝试次数, 继续 */
        printf("[BOOT] Retry Slot %c\r\n",
               (prev_slot == BOOT_SLOT_A) ? 'A' : 'B');
        current_boot_slot = prev_slot;
        return 0;
    }

    /*--- 情况2: 上次槽是 CONFIRMED → 正常启动 ---*/
    if (prev_state == SLOT_STATE_CONFIRMED) {
        if (check_slot(prev_slot) == 0) {
            printf("[BOOT] Slot %c confirmed, booting\r\n",
                   (prev_slot == BOOT_SLOT_A) ? 'A' : 'B');
            current_boot_slot = prev_slot;
            boot_params.boot_attempt_count = 0;
            boot_storage_save(&boot_params);
            return 0;
        }
        /* CONFIRMED 槽损坏 → 尝试回退 */
        printf("[BOOT] Slot %c corrupted! Fallback...\r\n",
               (prev_slot == BOOT_SLOT_A) ? 'A' : 'B');
        BootSlot_t fallback = get_inactive_slot();
        if (check_slot(fallback) == 0 && get_slot_state(fallback) == SLOT_STATE_CONFIRMED) {
            current_boot_slot = fallback;
            boot_params.active_slot = fallback;
            boot_params.boot_attempt_count = 0;
            boot_storage_save(&boot_params);
            return 0;
        }
        printf("[BOOT] Both slots invalid!\r\n");
        return -1;
    }

    /*--- 情况3: EMPTY / 首次上电 → 扫描两槽 ---*/
    printf("[BOOT] Scanning slots...\r\n");
    int a_ok = (check_slot(BOOT_SLOT_A) == 0);
    int b_ok = (check_slot(BOOT_SLOT_B) == 0);
    uint8_t a_st = get_slot_state(BOOT_SLOT_A);
    uint8_t b_st = get_slot_state(BOOT_SLOT_B);

    if (a_ok && b_ok) {
        /* 两个都有效: 优先 CONFIRMED, 其次 B (更新), 最后 A */
        if (b_st == SLOT_STATE_CONFIRMED && a_st != SLOT_STATE_CONFIRMED) {
            current_boot_slot = BOOT_SLOT_B;
        } else {
            current_boot_slot = BOOT_SLOT_A;
        }
    } else if (a_ok) {
        current_boot_slot = BOOT_SLOT_A;
    } else if (b_ok) {
        current_boot_slot = BOOT_SLOT_B;
    } else {
        printf("[BOOT] No valid firmware!\r\n");
        return -1;
    }

    printf("[BOOT] Chose Slot %c\r\n",
           (current_boot_slot == BOOT_SLOT_A) ? 'A' : 'B');
    boot_params.active_slot = current_boot_slot;
    boot_params.boot_attempt_count = 0;
    boot_storage_save(&boot_params);
    return 0;
}

/*============================================================================
 * Public API
 *============================================================================*/

void boot_manager_init(void)
{
    current_phase = BOOT_PHASE_STARTUP;
    phase_start_tick = HAL_GetTick();
    pending_sd_request = 0;
    pending_uart_request = 0;
    app_is_valid = 0;

    memset(&boot_params, 0, sizeof(boot_params));
    int ret = boot_storage_init(&boot_params);
    if (ret != 0) {
        printf("[BOOT] W25Q128 init failed, using defaults\r\n");
    } else {
        printf("[BOOT] Params loaded, active_slot=%d\r\n", boot_params.active_slot);
    }
}

void boot_manager_request_sd_upgrade(void)
{
    if (current_phase == BOOT_PHASE_IDLE ||
        current_phase == BOOT_PHASE_WAIT_KEY ||
        current_phase == BOOT_PHASE_ERROR) {
        pending_sd_request = 1;
    }
}

void boot_manager_request_uart_upgrade(void)
{
    if (current_phase == BOOT_PHASE_IDLE ||
        current_phase == BOOT_PHASE_WAIT_KEY ||
        current_phase == BOOT_PHASE_ERROR) {
        pending_uart_request = 1;
    }
}

void boot_manager_run(void)
{
    uint32_t now = HAL_GetTick();

    switch (current_phase) {

    case BOOT_PHASE_STARTUP:
        if (now - phase_start_tick > 200) {
            current_phase = BOOT_PHASE_CHECK_APP;
        }
        break;

    case BOOT_PHASE_CHECK_APP:
        printf("[BOOT] Checking firmware...\r\n");

        if (ab_decide_boot_slot() == 0) {
            if (check_slot(current_boot_slot) == 0) {
                app_is_valid = 1;
                printf("[BOOT] Slot %c: Valid\r\n",
                       (current_boot_slot == BOOT_SLOT_A) ? 'A' : 'B');
            }
        }

        printf("[BOOT] Active:%c  Inactive:%c\r\n",
               (current_boot_slot == BOOT_SLOT_A) ? 'A' : 'B',
               (get_inactive_slot() == BOOT_SLOT_A) ? 'A' : 'B');
        printf("[BOOT] KEY0:SD  KEY1:UART\r\n");
        printf("[BOOT] Auto jump in 3s...\r\n");
        current_phase = BOOT_PHASE_IDLE;
        phase_start_tick = now;
        break;

    case BOOT_PHASE_IDLE:
        if (pending_sd_request) {
            pending_sd_request = 0;
            printf("[BOOT] SD Upgrade Selected\r\n");
            current_phase = BOOT_PHASE_SD_UPGRADE;
            break;
        }
        if (pending_uart_request) {
            pending_uart_request = 0;
            printf("[BOOT] UART Upgrade Selected\r\n");
            current_phase = BOOT_PHASE_UART_UPGRADE;
            break;
        }

        /* 超时自动跳转 */
        if (now - phase_start_tick > BOOT_WAIT_TIMEOUT_MS) {
            if (app_is_valid && check_slot(current_boot_slot) == 0) {
                jump_to_slot(current_boot_slot);
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                printf("[BOOT] No Valid APP!\r\n");
                printf("[BOOT] KEY0:SD  KEY1:UART\r\n");
            }
        }
        break;

    case BOOT_PHASE_SD_UPGRADE:
        {
            /* 升级目标 = 非活跃槽 */
            BootSlot_t target = get_inactive_slot();
            printf("[BOOT] SD → Slot %c (0x%08lX)\r\n",
                   (target == BOOT_SLOT_A) ? 'A' : 'B',
                   (unsigned long)BOOT_SLOT_ADDR(target));

            int ret = iap_load_to_slot(target, IAP_MODE_SD);
            if (ret == 0) {
                printf("[BOOT] Download OK, verify...\r\n");
                if (check_slot(target) == 0) {
                    printf("[BOOT] Slot %c OK, switching active slot...\r\n",
                           (target == BOOT_SLOT_A) ? 'A' : 'B');
                    mark_slot_state(target, SLOT_STATE_NEW);
                    boot_params.active_slot = target;
                    boot_params.boot_attempt_count = 0;
                    current_boot_slot = target;
                    app_is_valid = 1;
                    boot_storage_save(&boot_params);
                    printf("[BOOT] Jumping to new firmware...\r\n");
                    HAL_Delay(500);
                    jump_to_slot(target);
                } else {
                    current_phase = BOOT_PHASE_WAIT_KEY;
                    printf("[BOOT] Slot %c verify FAILED!\r\n",
                           (target == BOOT_SLOT_A) ? 'A' : 'B');
                }
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                printf("[BOOT] SD Upgrade FAILED!\r\n");
            }
            if (current_phase == BOOT_PHASE_WAIT_KEY) {
                printf("[BOOT] KEY0:SD  KEY1:UART — Retry?\r\n");
            }
        }
        break;

    case BOOT_PHASE_UART_UPGRADE:
        {
            BootSlot_t target = get_inactive_slot();
            printf("[BOOT] UART → Slot %c (0x%08lX)\r\n",
                   (target == BOOT_SLOT_A) ? 'A' : 'B',
                   (unsigned long)BOOT_SLOT_ADDR(target));

            int ret = iap_load_to_slot(target, IAP_MODE_UART);
            if (ret == 0) {
                printf("[BOOT] Download OK, verify...\r\n");
                if (check_slot(target) == 0) {
                    printf("[BOOT] Slot %c OK, switching active slot...\r\n",
                           (target == BOOT_SLOT_A) ? 'A' : 'B');
                    mark_slot_state(target, SLOT_STATE_NEW);
                    boot_params.active_slot = target;
                    boot_params.boot_attempt_count = 0;
                    current_boot_slot = target;
                    app_is_valid = 1;
                    boot_storage_save(&boot_params);
                    printf("[BOOT] Jumping to new firmware...\r\n");
                    HAL_Delay(500);
                    jump_to_slot(target);
                } else {
                    current_phase = BOOT_PHASE_WAIT_KEY;
                    printf("[BOOT] Slot %c verify FAILED!\r\n",
                           (target == BOOT_SLOT_A) ? 'A' : 'B');
                }
            } else {
                current_phase = BOOT_PHASE_WAIT_KEY;
                printf("[BOOT] UART Upgrade FAILED!\r\n");
            }
            if (current_phase == BOOT_PHASE_WAIT_KEY) {
                printf("[BOOT] KEY0:SD  KEY1:UART — Retry?\r\n");
            }
        }
        break;

    case BOOT_PHASE_WAIT_KEY:
        if (pending_sd_request) {
            pending_sd_request = 0;
            printf("[BOOT] SD Upgrade Selected\r\n");
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            break;
        }
        if (pending_uart_request) {
            pending_uart_request = 0;
            printf("[BOOT] UART Upgrade Selected\r\n");
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            break;
        }
        break;

    case BOOT_PHASE_ERROR:
        if (pending_sd_request) {
            pending_sd_request = 0;
            current_phase = BOOT_PHASE_SD_UPGRADE;
            phase_start_tick = now;
            break;
        }
        if (pending_uart_request) {
            pending_uart_request = 0;
            current_phase = BOOT_PHASE_UART_UPGRADE;
            phase_start_tick = now;
            break;
        }
        {
            static uint32_t last_refresh = 0;
            if (now - last_refresh > 500) {
                last_refresh = now;
                printf("[BOOT] Error — KEY0:SD  KEY1:UART\r\n");
            }
        }
        break;

    case BOOT_PHASE_JUMP_APP:
        break;
    }
}