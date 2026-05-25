# STM32F407ZGT6 IAP Bootloader 设计文档

## 1. 架构概览

```
┌─────────────────────────────────────────────────────┐
│                    flash_tool.py                     │  ← PC 端固件发送工具
│              (XMODEM-1K 协议, 串口)                  │
└──────────────────────┬──────────────────────────────┘
                       │ USART1 (115200-8-N-1)
                       │ DMA RX (循环)
                       ▼
┌─────────────────────────────────────────────────────┐
│               Bootloader (main.c)                    │
│                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │   USART ISR  │  │ boot_manager │  │   key.c    │ │
│  │  IDLE→flag   │  │   状态机     │  │  按键轮询  │ │
│  └──────┬───────┘  └──────┬───────┘  └─────┬──────┘ │
│         │                 │                 │        │
│         ▼                 ▼                 │        │
│  ┌──────────────┐  ┌──────────────┐         │        │
│  │ app_sram_buf │  │boot_protocol │◄────────┘        │
│  │  (60KB SRAM) │  │  协议解析    │ Key2长按→升级     │
│  └──────────────┘  └──────┬───────┘                  │
│                            │                          │
│                            ▼                          │
│                    ┌──────────────┐                   │
│                    │ stmflash.c   │                   │
│                    │ (寄存器级操作)│                   │
│                    └──────┬───────┘                   │
│                           │                          │
│                           ▼                          │
│                    ┌──────────────┐                   │
│                    │ 内部 Flash   │                   │
│                    │  0x08010000  │  ← APP 区         │
│                    └──────────────┘                   │
│                                                      │
│  ┌──────────────┐  ┌──────────────┐                  │
│  │ boot_verify  │  │   iap.c     │                  │
│  │ CRC32+Magic  │  │  跳转原语   │                  │
│  └──────────────┘  └──────────────┘                  │
└─────────────────────────────────────────────────────┘
```

### 文件职责

| 文件 | 层 | 职责 |
|------|----|------|
| `Core/Src/main.c` | 应用入口 | 初始化外设, 主循环调用 `boot_manager_run()` + `key_handle()` |
| `Boot/Src/boot_manager.c` | Boot 核心 | **状态机**: IDLE → UPGRADE → VERIFY → JUMP, 串口接收缓冲区管理 |
| `Boot/Src/boot_protocol.c` | Boot 协议 | XMODEM-1K 协议解析: 包校验, 写 Flash, CRC 校验应答 |
| `Boot/Src/boot_verify.c` | Boot 校验 | CRC32 校验 APP 区, magic number (SP 值) 检查 |
| `Boot/Src/iap.c` | Boot 跳转 | 底层跳转原语: 检查 SP → 关中断 → 设 MSP → 跳转复位向量 |
| `Core/Src/usart.c` | 驱动 | USART1 + DMA 收发初始化, 全局缓冲区和标志位 |
| `Core/Src/stm32f4xx_it.c` | 中断 | USART1 IDLE 中断 → `is_receiving = 1` |
| `BSP/Src/stmflash.c` | BSP | Flash 寄存器级操作: 扇区擦除, 按字写入 (含 `_Static_assert` 地址校验) |
| `BSP/Src/key.c` | BSP | 按键状态机: 短按/长按/双击检测, Key2 长按触发升级请求 |
| `Tools/flash_tool.py` | 工具 | PC 端 Python 脚本, XMODEM-1K 协议发送 `.bin` 文件 |

---

## 2. 内存布局

```
Flash (1MB):
0x08000000 ┌──────────────┐
           │  Bootloader  │  64KB (Sector 0~3, 4×16KB)
0x08010000 ├──────────────┤  ← FLASH_APP_ADDR (boot_config.h)
           │              │
           │   APP 固件   │  960KB (Sector 4: 64KB, Sector 5~11: 7×128KB)
           │              │
0x08100000 └──────────────┘

SRAM (192KB, 由两块独立的物理 SRAM 组成):

  [块1] RAM (普通 SRAM) — 128KB
  0x20000000 ┌──────────────┐
             │ app_sram_buf │  60KB (DMA 循环接收缓冲区)
             │  协议包缓冲  │  ⚠ DMA 只能访问此块!
  0x2000F000 ├──────────────┤
             │ .data / .bss │  全局变量、静态变量
             │ heap / stack │  堆 (_Min_Heap_Size=0x200) + 栈 (_Min_Stack_Size=0x400)
  0x20020000 └──────────────┘

  [块2] CCMRAM (内核耦合存储器) — 64KB
  0x10000000 ┌──────────────┐
             │  (空闲/可选) │  直连 CPU 数据总线, 零等待访问
  0x10010000 └──────────────┘  ❌ DMA 无法访问此块!
                              可手动将频繁访问的变量放入 .ccmram 段
```

### 为什么 APP 区起始于 0x08010000?

- Bootloader 自身占 64KB (Sector 0~3)
- APP 区从 Sector 4 开始, 共 960KB: `64KB + 7×128KB = 960KB`
- STM32F407ZGT6 共 1MB Flash: `64KB + 960KB = 1024KB ✓`

---

## 3. Boot 状态机 (boot_manager.c)

### 状态转移图

```
上电/复位
    │
    ▼
┌───────────┐
│ STARTUP   │  上电初始化 (500ms)
└─────┬─────┘
      │
      ▼
┌──────────────┐
│ CHECK_APP    │  检查 APP 区是否有合法固件
└──────┬───────┘
       │
       ▼
┌───────────┐     3秒超时 + APP合法          ┌───────────┐
│  IDLE     │ ──────────────────────────────→│ JUMP_APP  │
│  空闲等待 │                                │ 跳转 APP  │
└─────┬─────┘                                └───────────┘
      │
      │ Key2 长按 或 3秒超时 + APP非法
      │
      ▼
┌───────────┐     30秒超时                  ┌───────────┐
│ UPGRADING │ ──────────────────────────────→│  ERROR    │
│  固件升级 │                                │  错误状态 │
└─────┬─────┘                                └─────┬─────┘
      │                                            │
      │ 协议完成                                   │ Key2
      ▼                                            │ 重试
┌───────────┐     校验通过 → 复位                  │
│ VERIFYING │ ─────────────────────→ 回到 STARTUP  │
│  固件校验 │                                     │
└─────┬─────┘                                     │
      │ 校验失败                                   │
      └────────────────────────────────────────────┘
```

### 超时机制详解

项目中定义了两个超时, 都在 `boot_config.h` 中:

```c
#define BOOT_WAIT_TIMEOUT_MS  3000UL   /* 3秒 启动等待超时 */
#define UPGRADE_TIMEOUT_MS    30000UL  /* 30秒 升级超时 */
```

---

#### 超时一: BOOT_WAIT_TIMEOUT_MS (3 秒) — 启动等待超时

| 项目 | 说明 |
|------|------|
| **生效阶段** | `BOOT_PHASE_IDLE` |
| **计时起点** | 进入 IDLE 态的那一刻 (~上电后约 500ms) |
| **超时条件** | 在 IDLE 态停留超过 3 秒, 且期间未长按 Key2、未收到串口升级指令 |
| **未超时** | 在 3 秒内长按 Key2 (2秒阈值) → 立即进入 `UPGRADING` 态, 计时被打断 |
| **超时结果: APP 合法** | → `JUMP_APP`: 关全局中断, 设 MSP=APP 栈顶, 跳转 PC=APP 复位向量 |
| **超时结果: APP 非法** | → `UPGRADING`: 自动进入升级模式, 等待串口固件 ("No APP, upgrade mode...") |

**代码位置**: `boot_manager.c` → `case BOOT_PHASE_IDLE` (第 89-111 行)

```
IDLE 态事件:
  ├─ Key2 长按 (upgrade_requested=1)  → UPGRADING (不等 3s)
  ├─ 已过 3s && APP 合法             → JUMP_APP
  ├─ 已过 3s && APP 非法             → UPGRADING
  └─ 未到 3s                         → 继续 IDLE 轮询
```

---

#### 超时二: UPGRADE_TIMEOUT_MS (30 秒) — 升级超时

| 项目 | 说明 |
|------|------|
| **生效阶段** | `BOOT_PHASE_UPGRADING` |
| **计时起点** | 进入 UPGRADING 态的那一刻 (无论来自 Key2 还是自动进入) |
| **超时条件** | 在 UPGRADING 态停留超过 30 秒, 且协议未报告 `PROT_STATE_DONE` |
| **未超时 (正常)** | 持续解析串口协议包、写 Flash, 直到协议栈报告完成 |
| **超时结果** | → `BOOT_PHASE_ERROR`: LCD 显示 "Upgrade Timeout!", 停止接收 |
| **正常结束** | `boot_protocol_get_state() == PROT_STATE_DONE` → `VERIFYING` → 复位 |

**代码位置**: `boot_manager.c` → `case BOOT_PHASE_UPGRADING` (第 198-208 行)

```
UPGRADING 态事件:
  ├─ 协议完成 (PROT_STATE_DONE)  → VERIFYING
  ├─ 已过 30s                     → ERROR ("Upgrade Timeout!")
  └─ 未到 30s                     → 继续解析协议包
```

---

#### 时序全景

```
上电  ──STARTUP(0.5s)── CHECK_APP ── IDLE ──[3s 倒计时开始]──
                                         │
                    ┌────────────────────┼────────────────────┐
                    ▼                    ▼                    ▼
              Key2 长按              3s + 有 APP          3s + 无 APP
                    │                    │                    │
                    ▼                    ▼                    ▼
              UPGRADING              JUMP_APP            UPGRADING
              [30s 倒计时开始]       (运行用户程序)       [30s 倒计时开始]
                    │
         ┌─────────┼─────────┐
         ▼                   ▼
    协议完成              30s 超时
         │                   │
         ▼                   ▼
     VERIFYING             ERROR
    (校验通过→复位)      (等待 Key2 重试)
```

### IDLE 态核心逻辑

在 IDLE 态, DMA + IDLE 中断持续工作: 串口数据到达 → IDLE 中断标记 `is_receiving=1` → 主循环在 `boot_manager_run()` 中消费。**DMA 不由 IDLE 中断停止**, 只在主循环中安全消费。擦除 Flash 期间 (~5 秒) DMA 硬件持续接收, 不会丢包。

---

## 4. XMODEM-1K 协议 (boot_protocol.c)

### 协议包格式

```
┌──────┬──────┬──────┬──────────┬──────┬──────┐
│ STX  │ Seq  │ ~Seq  │  Payload  │ CRC  │ CRC  │
│ 0x02 │ 自增 │ 取反  │  1024字节 │  Hi  │  Lo  │
└──────┴──────┴──────┴──────────┴──────┴──────┘
  1B     1B     1B       1024B      2B

总包长: 1029 字节
```

### 应答码 (发送端)

| 字节 | 含义 |
|------|------|
| `ACK (0x06)` | 包接收成功, 继续下一包 |
| `NAK (0x15)` | 包校验失败, 请求重发 |
| `CAN (0x18)` | 取消传输 |

### 接收端 (Bootloader) 协议流

```
接收 XMODEM 包
    │
    ├─ STX != 0x02?  → 返回 NAK
    ├─ Seq != expected_seq?  → 返回 NAK (或处理重发)
    ├─ ~Seq != (0xFF - Seq)?  → 返回 NAK
    ├─ CRC 校验失败?  → 返回 NAK
    │
    └─ 全部通过:
       ├─ 写 Flash: stmflash_write_word(addr, payload, 256 words)
       ├─ 累加偏移量
       ├─ expected_seq++
       └─ 返回 ACK
```

### CRC-CCITT 计算

```c
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}
```

---

## 5. 固件校验 (boot_verify.c)

### 校验流程

```
APP 区 Flash 数据
    │
    ├── 1. Magic Number 检查:
    │       读取 0x08010000 处的 SP 初始值
    │       必须在 0x20000000 ~ 0x20020000 (SRAM 范围)
    │       失败 → 返回错误
    │
    └── 2. CRC32 完整性校验:
            逐字读取 APP 区 (960KB)
            计算 CRC32 (多项式 0xEDB88320)
            与存储在 APP 末尾 4 字节的 CRC 比对
            不匹配 → 返回错误
```

### CRC 存储位置

APP `.bin` 文件最后 4 字节预留为 CRC32 值, `flash_tool.py` 发送时自动计算并附加。

---

## 6. Flash 操作 (stmflash.c)

### 关键特性

- **寄存器级操作**: 直接操作 FLASH 硬件寄存器, 不依赖 HAL Flash 驱动
- **编译期地址校验**: `_Static_assert(FLASH_APP_ADDR == 0x08010000UL, ...)`
- **x32 并行写入**: PSIZE=2 (32-bit), 一次写 4 字节, 最大化吞吐
- **自动跨扇区**: 数据跨越多个扇区时自动处理扇区边界
- **写前检查擦除**: 检查目标区域是否为全 0xFF, 是则跳过擦除

### 两种擦除模式

| 场景 | 调用路径 | 擦除范围 |
|------|----------|----------|
| 升级前预擦除 | `boot_manager → boot_protocol → erase_app_region()` | 整个 APP 区 (960KB, ~5s) |
| 写入时自动擦 | `stmflash_write_word() 内部` | 仅需写入的扇区, 且仅当非全 0xFF 时 |

---

## 7. 串口接收机制

### DMA + IDLE 中断

```
USART1 RX → DMA2 Stream2 → app_sram_buf[60KB]
                │
                ▼
         IDLE 中断 (帧间隔检测)
                │
                ▼
         is_receiving = 1
                │
                ▼
    主循环 boot_manager_run()
        读取 NDTR → 计算接收长度
        解析协议包
        重启 DMA
```

### 为什么不停止 DMA?

Flash 擦除操作 (~5 秒) 期间不能执行代码 (全部暂停在 `while(FLASH_SR_BSY)`), 但 DMA 硬件在后台持续接收数据。这确保擦除期间到达的数据不会丢失。

---

## 8. 按键系统 (key.c)

### 按键配置

| 按键 | GPIO | 功能 |
|------|------|------|
| Key0 | PE4 | 预留 |
| Key1 | PE3 | 预留 (旧代码跳过 Flash APP, 已废弃) |
| Key2 | PE2 | **长按 (2s): 请求进入升级模式** |

### 按键状态机

```
IDLE → PRESS_DETECTED → SHORT_PRESS / LONG_PRESS / DOUBLE_PRESS → IDLE
```

长按阈值: 2000ms (`LONG_PRESS_THRESHOLD`)
去抖时间: ~20ms (`PRESS_Time`)

---

## 9. 跳转原语 (iap.c)

```c
void iap_load_app(uint32_t appxaddr) {
    // 1. 检查 SP 是否在 SRAM 范围
    if (((*(__IO uint32_t*)appxaddr) & 0x2FFE0000) == 0x20000000) {
        // 2. 关全局中断
        __disable_irq();
        // 3. 设置 MSP = APP 向量表第一个字
        __set_MSP(*(__IO uint32_t*)appxaddr);
        // 4. 读取 APP 复位向量, 跳转
        uint32_t jump_addr = *(__IO uint32_t*)(appxaddr + 4);
        jump2app = (iapfun)jump_addr;
        jump2app();
    }
    // SP 非法时静默返回, 由 boot_manager 处理
}
```

**SP 检查原理**: Cortex-M4 的向量表第一个字是初始 MSP 值, 必须指向 SRAM (`0x20000000 ~ 0x20020000`), 低 17 位任意 (所以 mask `0x2FFE0000`)。

---

## 10. 使用流程

### PC 端发送固件

```bash
python Tools/flash_tool.py app.bin COM3 115200
```

### 完整升级流程

```
1. STM32 上电 → Bootloader 启动
2. LCD 显示 "Waiting firmware..."
3. PC 端运行 flash_tool.py
   ├── 发送 XMODEM 起始包 (STX, Seq=1)
   ├── Bootloader 检测包头 → 进入 UPGRADE 态
   ├── 擦除 APP 区 (960KB, ~5秒)
   ├── 逐包发送固件数据
   │   ├── 每包 1024 字节
   │   ├── 接收 ACK 则发下一包
   │   └── 接收 NAK 则重发当前包
   ├── 发送 EOT (0x04) 表示传输结束
   └── 发送 CRC32 校验值
4. Bootloader 收到 EOT → 进入 VERIFY 态
   ├── CRC32 校验 APP 区
   ├── Magic Number 检查
   └── 校验通过 → JUMP
5. 跳转到 APP: 关中断 → 设 MSP → 跳转复位向量
```

### 手动触发升级

长按 Key2 (PE2, 约2秒) → Bootloader 进入升级模式 → 等待 PC 端发送固件。

---

## 11. 配置宏速查

| 宏 | 定义位置 | 值 | 说明 |
|----|----------|----|------|
| `FLASH_APP_ADDR` | `boot_config.h` | `0x08010000` | APP 区起始地址 (Sector 4) |
| `FLASH_APP_SIZE` | `boot_config.h` | `960 * 1024` | APP 区总大小 |
| `APP_MAX_SIZE` | `usart.h` | `60 * 1024` | SRAM 协议包缓冲区大小 |
| `XMODEM_PACKET_SIZE` | `boot_protocol.h` | `1024` | XMODEM 每包数据大小 |
| `XMODEM_PACKET_TOTAL` | `boot_protocol.h` | `1029` | XMODEM 每包总大小 (含头尾) |
| `STM32_FLASH_BASE` | `stmflash.h` | `0x08000000` | Flash 起始地址 |
| `STM32_FLASH_SIZE` | `stmflash.h` | `0x100000` | Flash 总大小 (1MB) |

---

## 12. 编译与烧录

```bash
# 使用 CMake + ARM GCC 编译
cmake --preset STM32F407ZGT6
cmake --build build/

# 使用 ST-LINK / J-Link 烧录 Bootloader
# Bootloader 位于 0x08000000 (Sector 0~3)
```

---

## 13. 故障排查

| 现象 | 可能原因 | 解决 |
|------|----------|------|
| 一直显示 "Waiting firmware..." | 串口线未接或波特率不匹配 | 检查 USART1 TX(PA9)/RX(PA10), 确认 115200 |
| Flash 写入失败 | 地址计算错误, 或 APP 覆盖了 Bootloader | 检查 `FLASH_APP_ADDR=0x08010000`, 确认 boot_config.h 与 stmflash.c 扇区表一致 |
| 跳转后 HardFault | APP 固件损坏或 SP 非法 | 检查 APP bin 文件是否完整, CRC32 是否正确 |
| 串口频繁丢包 | DMA 缓冲区溢出 | 增大 `APP_MAX_SIZE` (当前 60KB) 或降低发送速率 |
| Key2 按键无响应 | GPIO 配置错误 | 确认 PE2 引脚配置为上拉输入 |