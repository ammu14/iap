#include "stmflash.h"
#include "stm32f407xx.h"
#include "boot_config.h"
#include <stdint.h>

/*
 * 扇区起始地址表 (STM32F407ZGT6, 1MB Flash)
 *
 * Sector 0~3  : 16KB each (Bootloader 区)
 * Sector 4    : 64KB     (APP 区起始, = FLASH_APP_ADDR)
 * Sector 5~11 : 128KB each
 *
 * 此表与 boot_config.h 中的 FLASH_APP_ADDR 必须同步.
 */
static const uint32_t SECTOR_ADDR_TABLE[] = {
	0x08000000, 0x08004000, 0x08008000, 0x0800C000, // Sector 0~3: 16KB
	FLASH_APP_ADDR,                                 // Sector 4:   64KB (= 0x08010000)
	0x08020000, 0x08040000, 0x08060000, 0x08080000, // Sector 5~8: 128KB
	0x080A0000, 0x080C0000, 0x080E0000,             // Sector 9~11:128KB
	0x08100000                                      // 结束地址 (1MB)
};

/* 编译期检查: 扇区表必须与 boot_config.h 一致 */
_Static_assert(FLASH_APP_ADDR == 0x08010000UL, "FLASH_APP_ADDR must be 0x08010000 (Sector 4)");

/*
 * 根据地址获取所属扇区编号 (0~11)
 */
static uint8_t GetSector(uint32_t addr) {
	for (uint8_t i = 0; i < 12; i++) {
		if (addr < SECTOR_ADDR_TABLE[i + 1]) 
			return i;
	}
	return 11;
}

/*
 * 内部扇区擦除 (不加锁不解锁, 纯硬件操作)
 * 擦除一个扇区约需 0.4~0.8 秒 (16KB) 到 2~4 秒 (128KB)
 */
static void stmflash_erase_sector_internal(uint8_t sector_idx) {
	while(FLASH->SR & FLASH_SR_BSY);            /* 等待 Flash 就绪 */
	FLASH->SR = (FLASH_SR_PGAERR | FLASH_SR_WRPERR | FLASH_SR_EOP); /* 清除错误标志 */

	FLASH->CR |= FLASH_CR_SER;                  /* 开启扇区擦除模式 */
	FLASH->CR &= ~(0x0F << 3);                  /* 清除 SNB[3:0] */
	FLASH->CR |= (uint32_t)sector_idx << 3;     /* 设置目标扇区编号 */
	FLASH->CR |= FLASH_CR_STRT;                 /* 触发擦除 */
	while(FLASH->SR & FLASH_SR_BSY);            /* 等待擦除完成 */
	FLASH->CR &= ~FLASH_CR_SER;                 /* 退出扇区擦除模式 */
}

/*
 * 按字 (32-bit) 写入 Flash, 自动擦除, 自动跨扇区
 *
 * 这是 Bootloader 写入固件数据的核心函数, 被 boot_protocol.c 调用.
 *
 * 流程:
 *   1. 解锁 Flash
 *   2. 配置 x32 并行写入模式 (一次写 4 字节)
 *   3. 逐扇区: 检查是否需要擦除 → 逐字写入
 *   4. 上锁 Flash
 *
 * @param waddr  起始地址 (必须 4 字节对齐)
 * @param pbuf   源数据指针 (uint32_t*)
 * @param length 要写入的字数 (word count, 不是字节数!)
 * @return 0=成功, 1=地址非法或未对齐, 2=编程错误
 */
uint8_t stmflash_write_word(uint32_t waddr, uint32_t *pbuf, uint32_t length)
{
	if(waddr < SECTOR_ADDR_TABLE[0] || (waddr + length * 4 > SECTOR_ADDR_TABLE[12]) || (waddr % 4 != 0)) {
		return 1; /* 非法地址或未对齐 */
	}

	uint32_t cur_addr = waddr;
	uint32_t end_addr = waddr + length * 4;
	uint32_t *data_ptr = pbuf;

	/* 解锁 Flash */
	if(FLASH->CR & FLASH_CR_LOCK) {
		FLASH->KEYR = 0x45670123;
		FLASH->KEYR = 0xCDEF89AB;
	}

	/* 配置 x32 并行写入模式 */
	FLASH->CR &= ~FLASH_CR_PSIZE;           /* 清除并行位 */
	FLASH->CR |= (2 << 8);                  /* PSIZE=10b: x32 模式 */

	__disable_irq();  /* 关中断, 防止 Flash 操作被打断 */

	while(cur_addr < end_addr) {
		uint8_t sec_idx = GetSector(cur_addr); 
		uint32_t sec_end = SECTOR_ADDR_TABLE[sec_idx + 1];
		
		/* 计算本扇区还能写多少个字 */
		uint32_t sec_remain_words = (sec_end - cur_addr) / 4;
		uint32_t words_left = (end_addr - cur_addr) / 4;
		uint32_t to_write = (words_left > sec_remain_words) ? sec_remain_words : words_left;

		/* 检查是否需要擦除: 目标区域非全 0xFF 则擦除整个扇区 */
		{
			uint8_t need_erase = 0;
			for (uint32_t i = 0; i < to_write * 4; i++) {
				if (*(__IO uint8_t*)(cur_addr + i) != 0xFF) {
					need_erase = 1;
					break;
				}
			}
			if (need_erase) {
				stmflash_erase_sector_internal(sec_idx);
			}
		}

		/* 逐字 (32-bit) 写入 */
		for(uint32_t i = 0; i < to_write; i++) {
			while(FLASH->SR & FLASH_SR_BSY); 
			FLASH->SR = (FLASH_SR_PGAERR | FLASH_SR_WRPERR | FLASH_SR_EOP);

			FLASH->CR |= FLASH_CR_PG;               /* 进入编程模式 */
			*(__IO uint32_t*)cur_addr = *data_ptr;  /* 一次写入 4 字节 */
			while (FLASH->SR & FLASH_SR_BSY); 
			FLASH->CR &= ~FLASH_CR_PG;              /* 退出编程模式 */

			if(FLASH->SR & (FLASH_SR_PGAERR | FLASH_SR_WRPERR)) {
				__enable_irq();
				FLASH->CR |= FLASH_CR_LOCK;
				return 2; /* 编程错误 */
			}

			cur_addr += 4;
			data_ptr++;
		}
	}
	
	/* 上锁 */
	__enable_irq();
	FLASH->CR |= FLASH_CR_LOCK; 
	return 0;
}

/*
 * 擦除指定地址所在的扇区 (带解锁/上锁)
 *
 * 被 boot_protocol.c 的 erase_app_region() 调用,
 * 在固件升级前擦除整个 APP 区 (960KB, 约 5~6 秒).
 *
 * @param addr 任意属于目标扇区的地址
 */
void stmflash_erase_addr(uint32_t addr) {
	uint8_t sector_number = GetSector(addr);

	if(sector_number > 11) return;

	if(FLASH->CR & FLASH_CR_LOCK) {
		FLASH->KEYR = 0x45670123;
		FLASH->KEYR = 0xCDEF89AB;
	}

	__disable_irq();
	stmflash_erase_sector_internal(sector_number);
	__enable_irq();

	FLASH->CR |= FLASH_CR_LOCK;
}
