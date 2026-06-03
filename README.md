# STM32F407ZGT6 IAP Bootloader v2.0.0

基于 STM32F407ZGT6 的 IAP Bootloader，支持 **SD 卡**和**串口 Ymodem** 双通道固件升级，**A/B 双槽位安全启动**。

---

## Flash 分区布局

```
0x08000000 ┌──────────────┐
           │  Bootloader  │  64 KB  (Sector 0~3)
0x08010000 ├──────────────┤
           │   Slot A     │  448 KB (Sector 4~7)   ← APP 固件槽 A
0x08080000 ├──────────────┤
           │   Slot B     │  512 KB (Sector 8~11)  ← APP 固件槽 B
0x08100000 └──────────────┘
```

每个槽末尾 32 字节存放固件元数据 (magic + size + CRC32)。槽位状态 (EMPTY/NEW/CONFIRMED) 保存在外部 W25Q128 SPI Flash 的扇区 0 中，带 CRC32 完整性校验，掉电不丢失。

---

## A/B 双槽安全启动机制

### 升级策略

新固件**始终写入非活跃槽**，不覆盖当前运行固件：

- 当前运行 Slot A → 升级写入 Slot B → 校验通过 → 切换活跃槽 → 跳转
- 当前运行 Slot B → 升级写入 Slot A → 校验通过 → 切换活跃槽 → 跳转

### A/B 槽生命周期

```
首次上电: 两槽均为 EMPTY
     ↓ 第一次烧录 (写入 Slot A)
Slot A = NEW → active_slot = A → 跳转执行
     ↓ APP 调 boot_confirm()
Slot A = CONFIRMED ✓
     ↓ 再次烧录升级 (写入 Slot B, 非活跃槽)
Slot B = NEW → active_slot = B → 跳转执行
     ↓ APP 调 boot_confirm()
Slot B = CONFIRMED, Slot A 保持 CONFIRMED (作为回退)
     ↓ 第三次烧录 (写入 Slot A)
Slot A = NEW → active_slot = A → 跳转执行
     ↓ ...
每次升级在 A ↔ B 之间交替切换
```

### 槽位状态机

```
 EMPTY ──[烧录成功]──→ NEW ──[APP 调 boot_confirm()]──→ CONFIRMED
                         │
                         └──[重试3次仍失败]──→ EMPTY (回退到另一槽)
```

### 启动决策

```
上电 → 读取 W25Q128 参数
  ├── 上次槽 NEW + 尝试次数充足 → 继续重试启动，等待 APP 确认
  ├── 上次槽 NEW + 尝试次数耗尽 → 回退到另一 CONFIRMED 槽
  ├── 上次槽 CONFIRMED + 校验通过 → 直接启动
  ├── 上次槽 CONFIRMED + 校验失败 → 尝试回退到另一槽
  └── 首次上电/EMPTY → 扫描两槽，按优先级选择 (CONFIRMED > 任一有效)
```

### 启动重试与回退

- APP 启动后 **10 秒内**必须调用 `boot_confirm()` 确认固件可用
- 若未确认，下次上电视为"启动失败"
- 每次上电 `boot_attempt_count` 递增
- 最多重试 **3 次**（`BOOT_ATTEMPT_MAX`）
- 3 次耗尽后标记当前槽为 EMPTY，自动**回退到另一个 CONFIRMED 槽**
- 若两个槽都无效，进入错误状态等待手动升级

---

## 断电容错

升级过程中写入顺序：**擦除 → 写入固件数据 → 写入元数据 (commit) → 校验**

元数据在末尾写入，相当于"提交"信号。没有合法元数据（魔数 + CRC）的槽视为无效，不会被启动。

| 断电时机 | 后果 | 下次上电 |
|---|---|---|
| 擦除过程中 | 目标槽部分为空，无元数据 | 校验失败，旧槽正常启动 ✅ |
| 写固件过程中 | 数据不完整，无元数据 | 校验失败，旧槽正常启动 ✅ |
| 写元数据过程中 | 数据完整，元数据损坏 | 魔数/CRC 不匹配，旧槽正常启动 ✅ |
| 校验完成后、跳转前 | 新槽有效但未被激活 | 旧 CONFIRMED 槽正常启动 ✅ |

**核心保证**：整个升级过程中，活跃槽的 Flash 区域完全不被修改。只要升级未成功提交元数据并切换 active_slot，设备始终可以从旧固件启动。

---

## 升级通道

| 通道 | 触发方式 | 协议 |
|------|----------|------|
| SD 卡 | 上电按 Key0 | FatFS 读取 app.bin |
| 串口 | 上电按 Key1 | Ymodem 协议 |

串口上位机: `python Tools/ymodem_sender.py --cli -p COM3 -b 115200 app.bin`

---

## 启动流程

```
上电 → 初始化外设 → A/B 槽决策
  ├── 有有效槽 → 3 秒窗口 (按键可进入升级) → 超时自动跳转 APP
  └── 无有效槽 → 等待按键进入升级模式
```

---

## 按键操作

| 按键 | 操作 | 功能 |
|------|------|------|
| Key0 | 短按 | SD 卡升级 |
| Key1 | 短按 | 串口 Ymodem 升级 |

---

## 目录结构

```
iap/
├── Boot/                    # Bootloader 核心模块
│   ├── Inc/                 # 头文件
│   │   ├── boot_config.h    # Flash 分区 / 宏配置
│   │   ├── boot_manager.h   # 启动状态机 (BootPhase, BootSlot, BootParams)
│   │   ├── boot_storage.h   # W25Q128 参数持久化 API
│   │   ├── boot_verify.h    # CRC32 / 固件校验 / Flash 复制 API
│   │   ├── boot_confirm.h   # APP 端固件确认 API
│   │   └── iap.h            # IAP 下载升级 API
│   └── Src/                 # 实现文件
│       ├── boot_manager.c   # A/B 槽决策 + 升级流程控制
│       ├── boot_storage.c   # W25Q128 读写 + CRC 校验
│       ├── boot_verify.c    # 固件头校验 + CRC32 计算 + Flash 复制
│       ├── boot_confirm.c   # APP 确认实现
│       ├── iap.c            # SD/UART 固件下载 + 写入 Flash
│       └── ymodem.c         # Ymodem 协议接收
├── BSP/                     # 板级支持
│   ├── Inc/
│   │   ├── stmflash.h       # 片内 Flash 操作 API
│   │   ├── w25Q128.h        # W25Q128 外部 SPI Flash API
│   │   └── key.h            # 按键驱动 API
│   └── Src/
│       ├── stmflash.c       # 片内 Flash 擦除/写入/扇区查询
│       ├── w25Q128.c        # W25Q128 读写/擦除
│       └── key.c            # 按键扫描
├── Core/                    # HAL 外设初始化 (main.c, stm32f4xx_it.c 等)
├── FATFS/                   # FatFs 文件系统 (SD 卡读取固件)
├── Drivers/                 # STM32 HAL 驱动库
├── Middlewares/              # FatFs R0.14
├── Tools/                   # Ymodem 上位机
│   ├── ymodem_sender.py
│   └── ymodem_sender.exe
├── STM32F407ZGTx_FLASH.ld           # Bootloader 链接脚本 (Flash 64KB)
├── STM32F407ZGTx_FLASH_SLOTB.ld     # APP 链接脚本 (Slot A/B)
└── CMakeLists.txt
```

---

## 编译

```bash
cmake --preset Debug
cmake --build build/Debug
```

工具链: `arm-none-eabi-gcc` + CMake + Ninja

---

## 关键配置 (`Boot/Inc/boot_config.h`)

| 宏 | 值 | 说明 |
|----|----|------|
| `FLASH_BOOT_ADDR` | `0x08000000` | Bootloader 起始 |
| `FLASH_BOOT_SIZE` | `0x00010000` | Bootloader 大小 (64 KB) |
| `FLASH_SLOT_A_ADDR` | `0x08010000` | Slot A 起始 |
| `FLASH_SLOT_A_SIZE` | `0x00070000` | Slot A 大小 (448 KB) |
| `FLASH_SLOT_B_ADDR` | `0x08080000` | Slot B 起始 |
| `FLASH_SLOT_B_SIZE` | `0x00080000` | Slot B 大小 (512 KB) |
| `FLASH_APP_MAX_SIZE` | 448 KB - 32 B | 最大固件大小 |
| `FLASH_APP_META_SIZE` | 32 | 固件元数据大小 |
| `FIRMWARE_MAGIC` | `0x544F4F42` | 固件魔数 ("BOOT") |
| `BOOT_ATTEMPT_MAX` | 3 | 最大启动重试次数 |
| `BOOT_WAIT_TIMEOUT_MS` | 3000 | 按键等待超时 (ms) |
| `BOOT_CONFIRM_TIMEOUT_MS` | 10000 | APP 确认超时 (ms) |
| `UPGRADE_TIMEOUT_MS` | 30000 | 升级数据接收超时 (ms) |

---

## 上位机使用

```bash
# 串口 Ymodem 升级
python Tools/ymodem_sender.py --cli -p COM3 -b 115200 app.bin

# SD 卡升级
# 将 app.bin 放入 SD 卡根目录, 上电按 Key0