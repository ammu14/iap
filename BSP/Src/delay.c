#include "stm32f4xx_hal.h"
#include "delay.h"
#include "stm32f4xx_hal_cortex.h"

/* 静态变量定义 */
static uint8_t  fac_us = 0;	/* us延时倍乘数 */

/**
 * @brief       初始化延时函数
 * @note        适用于HAL库+168MHz MCU+FreeRTOS，提供精准延时接口
 *              无需中断
 * @param       无
 * @retval      无
 */
void delay_init(void)
{
	/* 168MHz下，SysTick计数器每递减1对应1/168MHz ≈ 5.952ns */
	fac_us = 168;	/* 1us需要计数168次 */
}

/**
 * @brief       微秒级延时
 * @param       nus: 要延时的微秒数，范围：0~204522252 (2^32/168)
 * @retval      无
 */
void delay_us(uint32_t nus)
{		
	uint32_t ticks;
	uint32_t told, tnow, tcnt = 0;
	uint32_t reload = SysTick->LOAD;	/* 获取SysTick重装载值 */
	
	ticks = nus * fac_us;				/* 计算需要计数的节拍数 */
	told = SysTick->VAL;				/* 获取当前计数值 */

	while(1)
	{
		tnow = SysTick->VAL;
		if(tnow != told)
		{
			/* 计算从上一次读取到当前时刻的计数值变化量 */
			if(tnow < told) 
			{
				tcnt += told - tnow;	/* 未发生重装载 */
			}
			else
			{
				tcnt += reload - tnow + told;	/* 发生了重装载 */
			}
			
			told = tnow;
			if(tcnt >= ticks) 
			{
				break;	/* 延时时间达到，退出循环 */
			}
		}
	}

}
 
 