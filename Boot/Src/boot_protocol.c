#include "boot_protocol.h"
#include "boot_verify.h"
#include "stmflash.h"
#include "lcd.h"
#include <string.h>
#include <stdio.h>

/* ====== 协议状态机 ====== */
static ProtocolState_t proto_state = PROT_STATE_IDLE;
static uint32_t        expected_fw_size = 0;
static uint32_t        expected_fw_crc  = 0;
static uint32_t        written_bytes     = 0;

void boot_protocol_init(void)
{
    proto_state     = PROT_STATE_IDLE;
    expected_fw_size = 0;
    expected_fw_crc  = 0;
    written_bytes    = 0;
}

ProtocolState_t boot_protocol_get_state(void)
{
    return proto_state;
}

/*
 * 校验协议包 CRC (变长版本)
 * 协议包格式: header(1) + cmd(1) + addr(4) + len(4) + data(len) + crc(4) + tail(1)
 * @param buf   指向 header 的完整包缓冲区
 * @param pkt_len 实际接收到的包总长度
 */
static int validate_packet_crc(const uint8_t *buf, uint32_t pkt_len)
{
    if (pkt_len < 15) return -1;  /* 最小包: 1+1+4+4+0+4+1 = 15 */

    uint32_t data_len = *(uint32_t *)(buf + 6);  /* len 字段 (little-endian) */
    uint32_t payload_len = 1 + 4 + 4 + data_len; /* cmd + addr + len + data */
    uint32_t crc_offset = 1 + payload_len;       /* header(1) + payload */

    if (crc_offset + 4 + 1 > pkt_len) return -1; /* 数据不够 */

    /* CRC 覆盖: cmd(1) + addr(4) + len(4) + data(data_len) */
    uint32_t calc_crc = boot_crc32(buf + 1, payload_len);
    uint32_t pkt_crc = *(uint32_t *)(buf + crc_offset);

    return (calc_crc == pkt_crc) ? 0 : -1;
}

/*
 * 擦除 APP 区 (Sector 4-11)
 *
 * 此函数耗时较长 (~5 秒), 调用者必须确保 DMA 正在运行中,
 * 以便在此期间接收上位机后续发来的 WRITE 包。
 */
static void erase_app_region(void)
{
    uint32_t app_start = FLASH_APP_ADDR;
    uint32_t app_end   = FLASH_APP_ADDR + FLASH_APP_SIZE;

    char str_buf[40];

    /* 先清空状态显示区域, 避免残留文字 */
    LCD_Fill(30, 210, 230, 226, WHITE);

    /* 依次擦除 APP 区域内的每个扇区 */
    uint32_t addr = app_start;
    while (addr < app_end) {
        sprintf(str_buf, "Erasing 0x%08X...", (unsigned int)addr);
        LCD_Fill(30, 210, 230, 226, WHITE);
        LCD_ShowString(30, 210, 200, 16, 16, (uint8_t *)str_buf);
        stmflash_erase_addr(addr);

        /* 根据扇区大小递增地址 */
        if (addr == 0x08010000) {
            addr = 0x08020000;  /* Sector 4 是 64KB */
        } else {
            addr += 0x20000;    /* Sector 5-11 是 128KB */
        }
    }

    LCD_Fill(30, 210, 230, 226, WHITE);
    LCD_ShowString(30, 210, 200, 16, 16, "Erase done.         ");

    /* 擦除完后写回全 0xFF 的元数据 */
    FirmwareHeader_t header;
    memset(&header, 0xFF, sizeof(header));
    header.magic = 0xFFFFFFFF;
    uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
    stmflash_write_word(meta_addr, (uint32_t *)&header, sizeof(FirmwareHeader_t) / 4);
}

/*
 * 写入固件元数据到 APP 区末尾
 */
static void write_firmware_meta(void)
{
    FirmwareHeader_t header;
    memset(&header, 0xFF, sizeof(header));

    header.magic         = FIRMWARE_MAGIC;
    header.firmware_size = expected_fw_size;
    header.firmware_crc  = expected_fw_crc;

    uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
    stmflash_write_word(meta_addr, (uint32_t *)&header, sizeof(FirmwareHeader_t) / 4);
}

/*
 * 执行实际的擦除操作, 由 boot_manager 在 DMA 重启后调用.
 * 先清除同步状态显示, 执行擦除, 然后切换到 WRITING 状态.
 *
 * 关键设计: CMD_ERASE 只设置 PROT_STATE_ERASE_PENDING,
 * 主循环 boot_manager_run 在重启 DMA 后调用此函数执行实际擦除.
 * 这样 DMA 会在 ~5 秒的擦除期间持续接收 WRITE 包, 不会丢数据.
 */
int boot_protocol_perform_erase(void)
{
    if (proto_state != PROT_STATE_ERASE_PENDING) {
        return -1;
    }

    proto_state = PROT_STATE_ERASING;
    LCD_Fill(30, 210, 230, 226, WHITE);
    LCD_ShowString(30, 210, 200, 16, 16, "Erasing APP...");

    /* 执行实际的 Flash 擦除 (~5 秒, DMA 在此期间接收 WRITE 包) */
    erase_app_region();

    proto_state = PROT_STATE_WRITING;
    written_bytes = 0;

    LCD_Fill(30, 210, 230, 226, WHITE);
    LCD_ShowString(30, 210, 200, 16, 16, "Erase done.         ");
    return 0;
}

/*
 * 从缓冲区中按偏移量读取字段 (变长协议包)
 * buf 格式: header(1) + cmd(1) + addr(4) + len(4) + data[len] + crc(4) + tail(1)
 */
static inline uint8_t  pkt_get_cmd(const uint8_t *buf)   { return buf[1]; }
static inline uint32_t pkt_get_addr(const uint8_t *buf)  { return *(uint32_t *)(buf + 2); }
static inline uint32_t pkt_get_len(const uint8_t *buf)   { return *(uint32_t *)(buf + 6); }
static inline const uint8_t *pkt_get_data(const uint8_t *buf) { return buf + 10; }

void boot_protocol_process(uint8_t *buf, uint32_t len)
{
    /* 1. 基础校验 */
    if (len < 15) return;                                   /* 最小包: 15 字节 */
    if (buf[0] != PACKET_HEADER) return;                    /* 帧头不匹配 */
    if (buf[len - 1] != PACKET_TAIL) return;                /* 帧尾不匹配 */

    uint32_t data_len = pkt_get_len(buf);
    if (data_len > BOOT_PACKET_DATA_SIZE) {
        proto_state = PROT_STATE_ERROR;
        return;
    }

    /* 2. CRC 校验 (变长) */
    if (validate_packet_crc(buf, len) != 0) {
        return;
    }

    uint8_t  cmd  = pkt_get_cmd(buf);
    uint32_t addr = pkt_get_addr(buf);
    const uint8_t *data = pkt_get_data(buf);

    /* 3. 根据状态和命令处理 */
    switch (cmd) {

    case CMD_SYNC:
        proto_state = PROT_STATE_SYNCED;
        written_bytes = 0;
        LCD_Fill(30, 210, 230, 226, WHITE);
        LCD_ShowString(30, 210, 200, 16, 16, "SYNC OK, ready.      ");
        break;

    case CMD_ERASE:
        /*
         * 仅标记擦除请求, 不在此处执行实际的 Flash 擦除.
         * 因为此函数在 DMA 停止期间被调用 (boot_manager_run 中
         * 先调用 HAL_UART_DMAStop, 再解析协议包).
         * 如果在 DMA 停止期间执行 ~5 秒的擦除, 上位机发来的 WRITE
         * 包会全部丢失.
         *
         * 实际擦除由 boot_manager_run 在重启 DMA 后调用
         * boot_protocol_perform_erase() 执行.
         */
        if (proto_state != PROT_STATE_SYNCED) break;
        proto_state = PROT_STATE_ERASE_PENDING;
        LCD_Fill(30, 210, 230, 226, WHITE);
        LCD_ShowString(30, 210, 200, 16, 16, "ERASE pending...      ");
        break;

    case CMD_WRITE:
        if (proto_state != PROT_STATE_WRITING && proto_state != PROT_STATE_SYNCED) break;

        if (data_len > 0 && addr >= FLASH_APP_ADDR) {
            uint32_t meta_addr = FLASH_APP_ADDR + FLASH_APP_META_OFFSET;
            if (addr < meta_addr) {
                /* 固件本体写入 */
                stmflash_write_word(addr, (uint32_t *)data,
                                    (data_len + 3) / 4);
                written_bytes += data_len;
            } else if (addr == meta_addr) {
                /* 元数据写入, 提取固件大小和 CRC */
                FirmwareHeader_t *meta = (FirmwareHeader_t *)data;
                expected_fw_size = meta->firmware_size;
                expected_fw_crc  = meta->firmware_crc;
                stmflash_write_word(meta_addr, (uint32_t *)data,
                                    sizeof(FirmwareHeader_t) / 4);
            }
        }
        break;

    case CMD_VERIFY:
        proto_state = PROT_STATE_VERIFYING;
        {
            int ret = boot_verify_app();
            if (ret == 0) {
                proto_state = PROT_STATE_DONE;
                LCD_Fill(30, 210, 230, 226, WHITE);
                LCD_ShowString(30, 210, 200, 16, 16, "Verify PASS!         ");
            } else {
                proto_state = PROT_STATE_ERROR;
                LCD_Fill(30, 210, 230, 226, WHITE);
                LCD_ShowString(30, 210, 200, 16, 16, "Verify FAIL!         ");
            }
        }
        break;

    case CMD_RESET:
        LCD_Fill(30, 210, 230, 226, WHITE);
        LCD_ShowString(30, 210, 200, 16, 16, "Reset MCU...         ");
        __disable_irq();
        NVIC_SystemReset();
        break;

    default:
        break;
    }
}