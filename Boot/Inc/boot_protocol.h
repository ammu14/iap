#ifndef __BOOT_PROTOCOL_H__
#define __BOOT_PROTOCOL_H__

#include <stdint.h>
#include "boot_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== 协议常量 (与 Tools/flash_tool.py 保持一致) ====== */
#define PACKET_HEADER  0xAA
#define PACKET_TAIL    0x55

#define CMD_SYNC       0x55
#define CMD_ERASE      0xAA
#define CMD_WRITE      0xBB
#define CMD_VERIFY     0xCC
#define CMD_RESET      0xDD

/*
 * 协议包结构 (固定大小, 与上位机 flash_tool.py 对应)
 *    [0]    header  = 0xAA
 *    [1]    cmd     (SYNC/ERASE/WRITE/VERIFY/RESET)
 *    [2-5]  addr    (目标地址, little-endian)
 *    [6-9]  len     (有效数据长度, little-endian)
 *    [10..1024+9] data (数据区, BOOT_PACKET_DATA_SIZE 字节)
 *    [tail-5..tail-1] crc32 (cmd+addr+len+data 的 CRC32, little-endian)
 *    [tail]  tail   = 0x55
 */
typedef struct __attribute__((packed)) {
    uint8_t  header;                             /* 帧头 0xAA */
    uint8_t  cmd;                                /* 命令码 */
    uint32_t addr;                               /* 目标地址 */
    uint32_t len;                                /* 有效数据长度 */
    uint8_t  data[BOOT_PACKET_DATA_SIZE];        /* 数据区 (1024 字节) */
    uint32_t crc;                                /* CRC32 */
    uint8_t  tail;                               /* 帧尾 0x55 */
} ProtocolPacket_t;

/*
 * 协议状态
 */
typedef enum {
    PROT_STATE_IDLE,        /* 空闲, 等待命令 */
    PROT_STATE_SYNCED,      /* 已同步 */
    PROT_STATE_ERASE_PENDING, /* 擦除请求已收到, 等待主循环执行 */
    PROT_STATE_ERASING,     /* 擦除中 (Flash 操作中, 禁止打断) */
    PROT_STATE_WRITING,     /* 写入中 */
    PROT_STATE_VERIFYING,   /* 校验中 */
    PROT_STATE_DONE,        /* 升级完成 */
    PROT_STATE_ERROR        /* 错误 */
} ProtocolState_t;

/*
 * 初始化协议栈
 */
void boot_protocol_init(void);

/*
 * 处理一个协议包
 *   buf : 指向接收到的完整协议包
 *   len : 协议包大小 (应为 sizeof(ProtocolPacket_t))
 */
void boot_protocol_process(uint8_t *buf, uint32_t len);

/*
 * 获取当前协议状态
 */
ProtocolState_t boot_protocol_get_state(void);

/*
 * 执行实际的擦除操作 (由 boot_manager 在 DMA 运行期间调用)
 * 返回 0 成功, -1 失败
 */
int boot_protocol_perform_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_PROTOCOL_H__ */