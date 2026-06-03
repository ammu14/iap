#ifndef __STMFLASH_H__
#define __STMFLASH_H__

#include "stm32f4xx_hal.h"

/* FLASH 基地址和总大小 */
#define STM32_FLASH_BASE 0x08000000            /* STM32 FLASH 起始地址 */
#define STM32_FLASH_SIZE 0x100000              /* STM32 FLASH 总大小 (1MB) */

/*
 * 按字 (32-bit) 写入 Flash, 自动擦除, 自动跨扇区
 *   waddr  : 起始地址 (必须 4 字节对齐)
 *   pbuf   : 32 位数据指针
 *   length : 要写入的字数 (word count, 不是字节数!)
 *   返回   : 0 成功, 1 地址未对齐, 2 写入错误
 *
 * 用于 Boot 协议层的固件数据写入.
 */
uint8_t stmflash_write_word(uint32_t waddr, uint32_t *pbuf, uint32_t length);

/*
 * 按字节写入 Flash (内部自动对齐到 word 并自动擦除跨扇区)
 *   addr : 起始地址
 *   pbuf : 数据指针
 *   len  : 字节数
 *   返回 : 0 成功, 非0 失败
 */
int stmflash_write(uint32_t addr, uint32_t *pbuf, uint32_t len);

/*
 * 擦除地址所在的扇区 (带解锁/上锁)
 * 用于 Boot 协议层擦除 APP 区.
 */
void stmflash_erase_addr(uint32_t addr);

/*
 * 擦除指定扇区编号的扇区 (带解锁/上锁)
 *   sector_idx : 扇区编号 0~11
 */
void stmflash_erase_sector(uint32_t sector_idx);

/*
 * 获取地址所属的扇区编号
 *   addr : 地址
 *   返回 : 扇区编号 0~11
 */
uint32_t stmflash_get_sector(uint32_t addr);

#endif
