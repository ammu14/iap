#include "iap.h"
#include "stm32f407xx.h"

typedef void (*iapfun)(void); 
static iapfun jump2app;

/*
 * 跳转到 APP 区执行
 *
 * 这是 Bootloader 最底层的跳转原语:
 *   1. 检查 SP 是否落在 SRAM 范围 (0x20000000 ~ 0x20020000)
 *   2. 关全局中断 → 设置 MSP → 跳转到 APP 复位向量
 *   3. 若 SP 非法则静默返回, 由调用者处理错误
 *
 * 上层 boot_manager.c 在跳转前应通过 boot_verify_app() 校验固件完整性.
 */
void iap_load_app(uint32_t appxaddr)
{
    if (((*(__IO uint32_t*)appxaddr) & 0x2FFE0000) == 0x20000000)
    { 
        __disable_irq();   
        __set_MSP(*(__IO uint32_t*)appxaddr);
        uint32_t jump_addr = *(__IO uint32_t*)(appxaddr + 4);
        jump2app = (iapfun)jump_addr;  
        jump2app();
    }
}