#ifndef __YMODEM_H__
#define __YMODEM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ymodem 接收状态 */
typedef enum {
    YMODEM_STATE_IDLE,       /* 空闲 */
    YMODEM_STATE_WAIT_HEADER,/* 等待数据帧头 */
    YMODEM_STATE_RECV_DATA,  /* 接收数据中 */
    YMODEM_STATE_WAIT_EOT,   /* 等待传输结束 */
    YMODEM_STATE_DONE,       /* 传输完成 */
    YMODEM_STATE_ERROR,      /* 错误 */
    YMODEM_STATE_CANCEL      /* 被取消 */
} YmodemState_t;

/*
 * 数据回调: 当收到一个完整的数据包时调用
 *   offset: 当前数据在整个固件中的偏移 (字节)
 *   data:   数据指针
 *   len:    数据长度
 *   返回 0 表示成功, 非 0 表示写入失败
 */
typedef int (*ymodem_on_data_t)(uint32_t offset, const uint8_t *data, uint32_t len);

/*
 * 完成回调: 传输全部完成后调用
 *   total_size: 实际收到的固件总大小
 */
typedef void (*ymodem_on_complete_t)(uint32_t total_size);

/*
 * 初始化 Ymodem 接收端
 *   on_data:     数据回调 (写入 Flash)
 *   on_complete: 传输完成回调
 */
void ymodem_init(ymodem_on_data_t on_data, ymodem_on_complete_t on_complete);

/*
 * 复位 Ymodem 状态机
 */
void ymodem_reset(void);

/*
 * 喂入接收到的字节, 由 DMA 中断处理调用
 * 返回: 0 正常, -1 错误
 */
int ymodem_feed_byte(uint8_t byte);

/*
 * 喂入接收到的数据块 (批量处理, 效率更高)
 *   buf: 数据缓冲区
 *   len: 数据长度
 * 返回: 实际消耗的字节数, <0 表示错误
 */
int ymodem_feed_buffer(const uint8_t *buf, uint32_t len);

/*
 * 获取 Ymodem 产生的发送数据 (ACK/NAK/C/CAN)
 *   buf: 输出缓冲区
 * 返回: 需要发送的字节数, 0 表示无需发送
 */
int ymodem_get_tx_data(uint8_t *buf);

/*
 * 获取当前状态
 */
YmodemState_t ymodem_get_state(void);

/*
 * 获取已接收的固件总大小
 */
uint32_t ymodem_get_total_size(void);

/*
 * 取消传输
 */
void ymodem_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* __YMODEM_H__ */