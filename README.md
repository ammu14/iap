# STM32F407ZGT6 IAP Bootloader

基于 STM32F407ZGT6 的 IAP (In-Application Programming) Bootloader，支持 **SD 卡**和**串口**双通道固件升级。

---

## 1. Flash 分区布局

| 区域 | 起始地址 | 大小 | Sector | 说明 |
|------|----------|------|--------|------|
| Bootloader | `0x08000000` | 64 KB | 0 ~ 3 | IAP 引导程序 |
| APP | `0x08010000` | 960 KB | 4 ~ 11 | 用户固件 |
| 元数据 | APP 末尾 32 B | 32 B | 11 | 固件校验信息 |

**元数据结构 (`FirmwareHeader_t`):**

| 偏移 | 大小 | 字段 |
|------|------|------|
| 0 | 4 B | `magic` = `0x544F4F42` ("BOOT") |
| 4 | 4 B | `firmware_size` — 固件大小 |
| 8 | 4 B | `firmware_crc` — 固件 CRC32 |
| 12 | 20 B | `reserved` = `0xFF` |

---

## 2. 启动流程

```
上电
  │
  ▼
初始化外设 (时钟/GPIO/LCD/UART/SDIO/FATFS)
  │
  ▼
boot_manager_init()
  │
  ▼
┌─────────────────────────────┐
│ BOOT_PHASE_CHECK_APP        │  检查 APP 区是否有合法固件
│ (显示 "APP: Valid" 或       │  CRC32 + magic 校验
│  "APP: Invalid/Missing")    │
└─────────────┬───────────────┘
              │
              ▼
┌─────────────────────────────┐
│ BOOT_PHASE_IDLE             │  3 秒等待窗口
│ KEY0:SD  KEY1:UART          │
└─────────────┬───────────────┘
              │
     ┌────────┼────────┐
     │        │        │
   Key0     Key1    3 秒超时
     │        │        │
     ▼        ▼        ▼
  SD 升级  串口升级  ┌──────────────┐
                     │ APP 合法?     │
                     └──┬────────┬──┘
                        │ 是     │ 否
                        ▼        ▼
                   跳转 APP   BOOT_PHASE_WAIT_KEY
                              (无限等待 Key0/Key1)
```

### 3 秒窗口行为

| 条件 | 动作 |
|------|------|
| 按 Key0 | 进入 SD 卡烧写模式 (`BOOT_PHASE_SD_UPGRADE`) |
| 按 Key1 | 进入串口烧写模式 (`BOOT_PHASE_UPGRADING`) |
| 3 秒超时 + APP 合法 | 直接跳转 APP |
| 3 秒超时 + APP 无效 | 进入 `BOOT_PHASE_WAIT_KEY` 无限等待 |

### 无限等待模式

当没有合法 APP 时，进入 `BOOT_PHASE_WAIT_KEY`：
- **Key0** → SD 卡烧写固件，成功则跳转，失败则回到等待
- **Key1** → 串口烧写固件，成功则复位跳转，失败则回到等待

---

## 3. 按键操作

| 按键 | 操作 | 功能 |
|------|------|------|
| **Key0** | 短按 | 请求 SD 卡烧写 (`boot_manager_request_sd_upgrade()`) |
| **Key1** | 短按 | 请求串口烧写 (`boot_manager_request_uart_upgrade()`) |
| **Key2** | 长按 | 请求串口烧写（备用通道） |

按键扫描以 10ms 间隔在主循环中轮询。

---

## 4. SD 卡烧写

### 使用方法

1. 将编译好的 `app.bin` 放入 FAT32 格式的 SD 卡根目录
2. 将 SD 卡插入开发板
3. 上电后 3 秒内按 **Key0**（或在无限等待模式按 Key0）
4. Bootloader 自动完成：挂载 → 读文件 → 内部组协议包 → 协议栈写入 → 校验
5. 校验通过后直接跳转到 APP

### 流程细节

SD 卡烧写与串口烧写使用**同一套协议栈**，区别仅在于数据来源不同：
- **SD 路径**：从文件读取固件数据，MCU 内部调用 `build_protocol_packet()` 组包
- **UART 路径**：从 DMA 缓冲区解析协议包

两者最终都经过 `boot_protocol_process()` 统一处理写入和校验：

```
f_mount → f_open("app.bin") → f_size 检查大小
  → boot_protocol_init() 初始化协议栈
  → build_protocol_packet(CMD_SYNC)  → boot_protocol_process()  (握手)
  → build_protocol_packet(CMD_ERASE) → boot_protocol_process()  (请求擦除)
  → boot_protocol_perform_erase()                                (执行擦除)
  → 循环 f_read → build_protocol_packet(CMD_WRITE)
                → boot_protocol_process()                        (写入 Flash)
  → build_protocol_packet(CMD_WRITE) 元数据 → boot_protocol_process()  (写入 FirmwareHeader)
  → build_protocol_packet(CMD_VERIFY) → boot_protocol_process()  (CRC32 校验)
  → jump_to_app()
```

- **协议包格式与串口完全一致**：Header(0xAA) + Cmd + Addr + Len + Data + CRC32 + Tail(0x55)
- **bin 文件必须是原始二进制格式**，不需要任何头部或封装
- 最大支持 960 KB - 32 B ≈ 983008 字节

---

## 5. 串口烧写

### 使用方法

```bash
# 上位机 (PC) 执行
python Tools/flash_tool.py app.bin -p COM3 -b 115200
```

1. MCU 上电后 3 秒内按 **Key1**（或在无限等待模式按 Key1）
2. LCD 显示 `UART Upgrade mode...`
3. PC 端运行 `flash_tool.py`，通过串口发送固件
4. 升级完成后自动复位并跳转

### 协议格式

MCU 与 PC 之间使用变长协议包通信：

| 偏移 | 大小 | 字段 |
|------|------|------|
| 0 | 1 B | Header `0xAA` |
| 1 | 1 B | CMD (命令码) |
| 2 | 4 B | Addr (目标地址, LE) |
| 6 | 4 B | Len (数据长度, LE) |
| 10 | Len | Data (固件数据, 最多 1024 B) |
| 10+Len | 4 B | CRC32 (覆盖 cmd+addr+len+data) |
| 14+Len | 1 B | Tail `0x55` |

### 命令码

| 命令 | 值 | 说明 |
|------|-----|------|
| `CMD_SYNC` | `0x55` | 握手同步 |
| `CMD_ERASE` | `0xAA` | 请求擦除 APP 区（延迟执行） |
| `CMD_WRITE` | `0xBB` | 写入数据块 |
| `CMD_VERIFY` | `0xCC` | CRC32 校验固件 |
| `CMD_RESET` | `0xDD` | 复位 MCU |

### 延迟擦除机制

`CMD_ERASE` 到达时只设置 `PROT_STATE_ERASE_PENDING` 标志，**不立即执行擦除**。主循环检测到该标志后：
1. 重启 UART DMA（让后续 WRITE 包不会丢失）
2. 在 DMA 运行期间执行 Flash 擦除（约 5~6 秒）

这样上位机不需要等待擦除完成就可以持续发送 WRITE 包。

### DMA + IDLE 接收机制

- UART 使用 **DMA 循环模式** 接收数据
- **IDLE 中断** 检测到帧间隙后只置 `is_receiving = 1` 标志位
- 主循环安全处理：停止 DMA → 读 NDTR 计算已收字节 → 逐包解析 → 重启 DMA
- 避免在中断上下文中操作 DMA 寄存器导致竞态

---

## 6. 编译与烧录

### 工具链

- **编译器**: `arm-none-eabi-gcc`
- **构建系统**: CMake + Ninja

### 编译

```bash
cmake --preset GCC-Build
cmake --build build
```

输出文件位于 `build/iap.bin` / `build/iap.elf` / `build/iap.hex`。

### 烧录 Bootloader

使用 J-Link / ST-Link 将 `iap.bin` 烧录到 `0x08000000`：

```bash
# J-Link 示例
JLinkExe -device STM32F407ZG -speed 4000 -if SWD -autoconnect 1 \
  -CommanderScript scripts/flash.jlink
```

### 生成 APP 固件

APP 项目需要：
1. 修改链接脚本，起始地址设为 `0x08010000`
2. 在 `main()` 开头调用 `SCB->VTOR = 0x08010000` 重映射向量表
3. 编译生成 `.bin` 文件

---

## 7. 目录结构

```
iap/
├── Boot/                       # Bootloader 核心模块
│   ├── Inc/
│   │   ├── boot_config.h       # Flash 布局、超时等配置常量
│   │   ├── boot_manager.h      # 启动状态机接口
│   │   ├── boot_protocol.h     # 串口协议定义
│   │   ├── boot_verify.h       # CRC32 校验接口
│   │   └── iap.h               # SD 卡 IAP 接口
│   └── Src/
│       ├── boot_manager.c      # 启动状态机主逻辑
│       ├── boot_protocol.c     # 串口协议解析与处理
│       ├── boot_verify.c       # 固件校验 (CRC32 + magic)
│       └── iap.c               # SD 卡 / 串口烧写 (读文件/收包 → 协议栈处理)
├── BSP/                        # 板级支持
│   ├── Inc/
│   │   └── key.h               # 按键驱动接口
│   └── Src/
│       ├── key.c               # 按键扫描状态机 (短按/长按/双击)
│       └── stmflash.c          # STM32 内部 Flash 读写驱动
├── Core/                       # HAL 外设初始化
│   ├── Inc/
│   └── Src/
│       ├── main.c              # 主入口
│       ├── stm32f4xx_it.c      # 中断服务 (含 IDLE 中断)
│       ├── sdio.c / dma.c / usart.c / ...
├── FATFS/                      # FatFs 文件系统
│   ├── App/fatfs.c
│   └── Target/sd_diskio.c      # SD 卡底层磁盘 I/O
├── Drivers/                    # HAL 驱动库
│   ├── CMSIS/
│   └── STM32F4xx_HAL_Driver/
├── Middlewares/
│   └── Third_Party/FatFs/      # FatFs R0.14
├── Tools/
│   └── flash_tool.py           # 串口烧录上位机脚本
├── CMakeLists.txt              # CMake 构建配置
├── STM32F407ZGTx_FLASH.ld      # 链接脚本 (Bootloader @ 0x08000000)
└── startup_stm32f407xx.s       # 启动汇编
```

---

## 8. 状态机概览

`boot_manager.c` 中的状态机定义 (`BootPhase_t`)：

```
STARTUP → CHECK_APP → IDLE ──3秒超时+有效APP→ JUMP_APP
                 │       │
                 │       ├── Key0 → SD_UPGRADE → JUMP_APP
                 │       │                └(失败)→ WAIT_KEY
                 │       │
                 │       └── Key1 → UPGRADING → VERIFYING → RESET
                 │                         └(失败/超时)→ WAIT_KEY/ERROR
                 │
                 └── 3秒超时+无效APP → WAIT_KEY ←── ERROR
                                           │
                                      Key0/Key1 → SD_UPGRADE/UPGRADING