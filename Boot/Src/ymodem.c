#include "ymodem.h"
#include <string.h>
#include <stdlib.h>

/* ====== Ymodem 协议常量 ====== */
#define SOH  0x01
#define STX  0x02
#define EOT  0x04
#define ACK  0x06
#define NAK  0x15
#define CAN  0x18
#define C    0x43

#define DATA_128  128
#define DATA_1024 1024

/* ====== CRC16 查找表 (XMODEM/CCITT) ====== */
static const uint16_t crc16_tab[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

/* ====== 内部解析状态 ====== */
typedef enum {
    PS_HEADER,      /* 等待 SOH/STX/EOT/CAN */
    PS_SEQ,         /* 等待序号 */
    PS_SEQ_CMP,     /* 等待序号补码 */
    PS_DATA,        /* 接收数据负载 */
    PS_CRC_H,       /* CRC 高字节 */
    PS_CRC_L,       /* CRC 低字节 */
} PsState;

static PsState ps;
static uint8_t  pkt_type;       /* SOH 或 STX */
static uint16_t pkt_data_len;   /* 当前包数据长度 (128 或 1024) */
static uint16_t pkt_data_pos;   /* 当前已收数据字节数 */
static uint8_t  pkt_seq;        /* 包序号 */
static uint8_t  pkt_seq_cmp;    /* 包序号补码 */
static uint8_t  pkt_data[1024]; /* 数据缓冲区 */
static uint16_t pkt_crc;        /* 收到的 CRC */

/* 回调 */
static ymodem_on_data_t     data_func;
static ymodem_on_complete_t done_func;

/* 协议状态 */
static YmodemState_t state;
static uint8_t  f_seq;        /* 期望的数据包序号 */
static uint32_t f_total;      /* 已收固件总字节数 */
static uint32_t f_expected;   /* 从文件头解析出的固件大小, 0=未知 */
static uint8_t  tx_buf[2];    /* 待发送的响应 */
static uint8_t  tx_len;       /* 响应长度 */
static uint8_t  eot_count;    /* EOT 计数 */
static uint8_t  in_file;      /* 0:等待文件头, 1:正在接收数据 */

/* ====== 内部函数 ====== */
static uint16_t crc16(const uint8_t *d, uint32_t n)
{
    uint16_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        c = (uint16_t)((c << 8) ^ crc16_tab[((c >> 8) ^ d[i]) & 0xFF]);
    return c;
}

static void put_tx(uint8_t b)
{
    tx_buf[0] = b;
    tx_len = 1;
}

static void put_tx2(uint8_t a, uint8_t b)
{
    tx_buf[0] = a;
    tx_buf[1] = b;
    tx_len = 2;
}

/*
 * 从文件头包中解析固件大小
 * 格式: "filename\0size\0..."
 * 例如: "app.bin\0123456\0..."
 */
static uint32_t parse_filesize(const uint8_t *data, uint32_t len)
{
    /* 跳过文件名 (找到第一个 '\0') */
    uint32_t i = 0;
    while (i < len && data[i] != '\0') i++;
    if (i >= len) return 0;
    i++; /* 跳过 '\0' */

    /* 读取数字字符串 */
    char num_str[16];
    uint32_t j = 0;
    while (i < len && data[i] != '\0' && data[i] != ' ' && j < 15)
        num_str[j++] = (char)data[i++];
    num_str[j] = '\0';

    if (j == 0) return 0;
    return (uint32_t)strtoul(num_str, NULL, 10);
}

/*
 * 处理一个完整接收到的 Ymodem 数据包
 */
static void process_pkt(void)
{
    /* 1. CRC 校验 */
    if (crc16(pkt_data, pkt_data_len) != pkt_crc) {
        put_tx(NAK);
        return;
    }

    /* 2. 序号校验 */
    if (pkt_seq != f_seq || pkt_seq_cmp != (uint8_t)(~pkt_seq)) {
        put_tx(NAK);
        return;
    }

    if (!in_file) {
        /* 文件头包: 解析固件大小, 然后跳过 */
        f_expected = parse_filesize(pkt_data, pkt_data_len);
        in_file = 1;
        f_seq = 1;
        f_total = 0;
        put_tx(ACK);
        /* 紧接着发送 'C' 请求第一个数据包 */
        put_tx2(ACK, C);
        return;
    }

    /* 3. 检查是否为空包 (全零, 表示传输结束) */
    uint8_t all_zero = 1;
    for (uint16_t i = 0; i < pkt_data_len; i++) {
        if (pkt_data[i] != 0) {
            all_zero = 0;
            break;
        }
    }

    if (all_zero) {
        /* NULL 包: 传输结束 */
        put_tx(ACK);
        state = YMODEM_STATE_DONE;
        if (done_func) done_func(f_total);
        return;
    }

    /* 4. 数据包: 确定实际有效数据长度 */
    uint32_t actual_len = pkt_data_len;
    if (f_expected > 0) {
        /* 已知文件大小, 截断尾部填充 */
        uint32_t remaining = f_expected - f_total;
        if (remaining < actual_len) actual_len = remaining;
    }

    /* 5. 通过回调写入 Flash */
    if (data_func && actual_len > 0) {
        if (data_func(f_total, pkt_data, actual_len) != 0) {
            put_tx(NAK);
            state = YMODEM_STATE_ERROR;
            return;
        }
    }

    f_total += actual_len;
    f_seq++;
    put_tx(ACK);
}

/* ====== 逐字节喂入 ====== */
static int feed_byte(uint8_t b)
{
    switch (ps) {
    case PS_HEADER:
        if (b == SOH) {
            pkt_type = SOH;
            pkt_data_len = DATA_128;
            ps = PS_SEQ;
        } else if (b == STX) {
            pkt_type = STX;
            pkt_data_len = DATA_1024;
            ps = PS_SEQ;
        } else if (b == EOT) {
            eot_count++;
            if (eot_count == 1) {
                put_tx(NAK);
            } else {
                put_tx(ACK);
                put_tx2(ACK, C);
                f_seq = 0;  /* 期待 NULL 包 (序号 0) */
            }
        } else if (b == CAN) {
            state = YMODEM_STATE_CANCEL;
            return -1;
        }
        break;

    case PS_SEQ:
        pkt_seq = b;
        ps = PS_SEQ_CMP;
        break;

    case PS_SEQ_CMP:
        pkt_seq_cmp = b;
        pkt_data_pos = 0;
        pkt_crc = 0;
        ps = (pkt_data_len > 0) ? PS_DATA : PS_CRC_H;
        break;

    case PS_DATA:
        pkt_data[pkt_data_pos++] = b;
        if (pkt_data_pos >= pkt_data_len)
            ps = PS_CRC_H;
        break;

    case PS_CRC_H:
        pkt_crc = (uint16_t)b << 8;
        ps = PS_CRC_L;
        break;

    case PS_CRC_L:
        pkt_crc |= b;
        process_pkt();
        ps = PS_HEADER;
        break;
    }
    return 0;
}

/* ====== 公开 API ====== */
void ymodem_init(ymodem_on_data_t on_data, ymodem_on_complete_t on_complete)
{
    data_func = on_data;
    done_func = on_complete;
    ymodem_reset();
}

void ymodem_reset(void)
{
    ps = PS_HEADER;
    state = YMODEM_STATE_IDLE;
    f_seq = 0;
    f_total = 0;
    f_expected = 0;
    tx_len = 0;
    eot_count = 0;
    in_file = 0;
    put_tx(C);  /* 发送 'C' 启动 Ymodem 传输 */
}

int ymodem_feed_byte(uint8_t byte)
{
    if (state == YMODEM_STATE_DONE ||
        state == YMODEM_STATE_ERROR ||
        state == YMODEM_STATE_CANCEL)
        return -1;
    return feed_byte(byte);
}

int ymodem_feed_buffer(const uint8_t *buf, uint32_t len)
{
    if (state == YMODEM_STATE_DONE ||
        state == YMODEM_STATE_ERROR ||
        state == YMODEM_STATE_CANCEL)
        return -1;

    uint32_t consumed = 0;
    while (consumed < len) {
        if (feed_byte(buf[consumed]) != 0)
            break;
        consumed++;
        /* 每处理完一个包就返回, 让调用者有机会发送响应 */
        if (tx_len > 0 && ps == PS_HEADER)
            break;
    }
    return (int)consumed;
}

int ymodem_get_tx_data(uint8_t *buf)
{
    if (tx_len > 0) {
        buf[0] = tx_buf[0];
        if (tx_len > 1) buf[1] = tx_buf[1];
        int n = tx_len;
        tx_len = 0;
        return n;
    }
    return 0;
}

YmodemState_t ymodem_get_state(void)
{
    return state;
}

uint32_t ymodem_get_total_size(void)
{
    return f_total;
}

void ymodem_cancel(void)
{
    put_tx2(CAN, CAN);
    state = YMODEM_STATE_CANCEL;
}