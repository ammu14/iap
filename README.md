# STM32F407ZGT6 IAP Bootloader

基于 STM32F407ZGT6 的 IAP (In-Application Programming) Bootloader，版本 **v1.0.0**，支持 **SD 卡**和**串口 (Ymodem)** 双通道固件升级。

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
│ KEY0:SD  KEY1:UART          │  (LCD 实时提示)
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
| **Key2** | 长按 | 请求串口烧写（备用通道，防误触） |

按键采用**多击状态机**扫描，支持短按 / 长按 / 双击检测，以 10ms 间隔在主循环中轮询。

---

## 4. SD 卡烧写

### 使用方法

1. 将编译好的 `app.bin` 放入 FAT32 格式的 SD 卡根目录
2. 将 SD 卡插入开发板
3. 上电后 3 秒内按 **Key0**（或在无限等待模式按 Key0）
4. Bootloader 自动完成：挂载 → 读文件 → 擦除 Flash → 逐块写入 → 写元数据 → CRC 校验
5. 校验通过后直接跳转到 APP

### 流程细节

SD 卡烧写**直接操作 Flash**，不经过任何协议栈：

```
f_mount → f_open("app.bin") → f_size 检查大小
  → erase_app_region()              (擦除 Sector 4~11)
  → 循环 f_read → flash_write()     (逐块写入 Flash)
  → 写入 FirmwareHeader 到 APP 区末尾
  → boot_verify_app()               (CRC32 校验)
  → jump_to_app()
```

- **bin 文件必须是原始二进制格式**，不需要任何头部或封装
- 最大支持 960 KB - 32 B ≈ 983008 字节
- 写入过程中 LCD 实时显示进度

---

## 5. 串口烧写 (Ymodem)

### 使用方法

支持**标准 Ymodem 协议**，可选用以下方式之一：

#### 方式 A：专用上位机（推荐）

项目自带跨平台 Ymodem 串口助手，支持 GUI 和 CLI 两种模式：

```bash
# GUI 模式 (图形界面)
python Tools/ymodem_sender.py

# CLI 模式 (命令行)
python Tools/ymodem_sender.py --cli -p COM3 -b 115200 app.bin

# 列出可用串口
python Tools/ymodem_sender.py --list
```

**GUI 功能：**
- 串口参数配置（端口 / 波特率 / 数据位 / 停止位 / 校验）
- 收发区：支持 HEX/文本 显示与发送、定时发送
- Ymodem 固件升级区：选择 .bin 文件、选择包大小 (128/1024)、进度条显示
- 数据收发计数、接收数据保存到文件

Windows 用户也可直接运行预编译的 `Tools/ymodem_sender.exe`（无需 Python 环境）。

#### 方式 B：第三方终端

兼容任何支持 Ymodem 的串口终端：
- **SecureCRT**: Transfer → Send Ymodem → 选择 app.bin
- **Tera Term**: File → Transfer → Ymodem → Send
- **MobaXterm**: 同样支持 Ymodem

### 操作步骤

1. MCU 上电后 3 秒内按 **Key1**（或在无限等待模式按 Key1，或用 Key2 长按备用触发）
2. Bootloader 自动擦除 APP 区，然后发送 `'C'` 字符发起 Ymodem 传输
3. 在上位机 / 终端中选择 `app.bin` 并通过 Ymodem 发送
4. Bootloader 逐包接收、写入 Flash 并回 ACK
5. 传输完成后写入元数据、CRC 校验
6. 校验通过 → 自动复位跳转

### 协议说明

Ymodem 是一个广泛使用的**嵌入式固件传输标准协议**，内建 CRC16 校验和自动重传。相比自定义协议：
- **无需专用上位机** — 任何串口终端软件即可（也可使用本项目自带的上位机）
- **标准化** — 被众多 Bootloader (U-Boot、MCUBoot 等) 采用
- **可靠性高** — 每包 128/1024 字节 + CRC16，错误自动重传

### 实现细节

| 项目 | 说明 |
|------|------|
| 接收方式 | **DMA 循环模式** + **IDLE 中断** |
| 数据包大小 | 1024 字节（`BOOT_PACKET_DATA_SIZE`） |
| 升级超时 | 30 秒无数据自动超时（`UPGRADE_TIMEOUT_MS`） |
| 协议实现 | `Boot/Src/ymodem.c` / `Boot/Inc/ymodem.h` |
| 上位机 | `Tools/ymodem_sender.py` (Python 3 + tkinter + pyserial) |

---

## 6. LCD 显示

Bootloader 使用 **800×480 LCD** 显示状态信息：

```
╔══════════════════════════════╗
║  IAP Bootloader              ║  ← 标题
║  Version: v1.0.0             ║  ← 版本号
║  ─────────────────────────   ║  ← 分隔线
║  APP: Valid / Invalid        ║  ← APP 校验结果
║  Key0: SD Upgrade            ║  ← 按键提示
║  Key1: UART Upgrade          ║
║  ─────────────────────────   ║
║  [状态信息区域]               ║  ← 烧写进度/状态
║  ...                         ║
╚══════════════════════════════╝
```

布局配置在 `Boot/Inc/boot_config.h` 中（`LCD_X`, `LCD_LINE_*` 等宏）。

---

## 7. 编译与烧录

### 工具链

- **编译器**: `arm-none-eabi-gcc`
- **构建系统**: CMake + Ninja
- **Python**: 3.x（上位机 `ymodem_sender.py` 需要 `pyserial` 库）

### 编译


```bash
cmake --preset GCC-Build
cmake --build build
```

输出文件位于 `build/` 目录（`.bin` / `.elf` / `.hex`）。

### 烧录 Bootloader

使用 J-Link / ST-Link 将 bootloader 固件烧录到 `0x08000000`：

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

## 8. 目录结构

```
iap/
├── Boot/                       # Bootloader 核心模块
│   ├── Inc/
│   │   ├── boot_config.h       # Flash 布局、超时、LCD 布局等配置
│   │   ├── boot_manager.h      # 启动状态机接口
│   │   ├── boot_verify.h       # CRC32 校验接口
│   │   ├── iap.h               # IAP 升级接口 (SD + UART)
│   │   └── ymodem.h            # Ymodem 协议栈接口
│   └── Src/
│       ├── boot_manager.c      # 启动状态机主逻辑
│       ├── boot_verify.c       # 固件校验 (CRC32 + magic)
│       ├── iap.c               # SD 卡 / 串口烧写实现
│       └── ymodem.c            # Ymodem 协议状态机
├── BSP/                        # 板级支持
│   ├── Inc/
│   │   ├── delay.h             # 延时接口
│   │   ├── key.h               # 按键驱动接口 (短按/长按/双击)
│   │   ├── lcd.h               # LCD 显示驱动接口
│   │   └── stmflash.h          # Flash 读写接口
│   └── Src/
│       ├── delay.c             # 微秒级延时
│       ├── key.c               # 按键多击状态机 + 回调注册
│       ├── lcd.c               # LCD 显示驱动
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
├── Tools/                      # 工具集
│   ├── ymodem_sender.py        # Ymodem 串口助手上位机 (GUI + CLI)
│   └── ymodem_sender.exe       # Windows 预编译版
├── cmake/                      # CMake 配置
│   ├── gcc-arm-none-eabi.cmake # 交叉编译工具链
│   └── stm32cubemx/            # STM32CubeMX 生成代码的 CMake 集成
├── CMakeLists.txt              # CMake 构建配置
├── CMakePresets.json           # CMake 预设 (GCC-Build)
├── STM32F407ZGTx_FLASH.ld      # 链接脚本 (Bootloader @ 0x08000000)
├── startup_stm32f407xx.s       # 启动汇编
└── iap.ioc                     # STM32CubeMX 项目配置文件
```

---

## 9. 状态机概览

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
```

---

## 10. 配置常量

主要配置宏定义在 `Boot/Inc/boot_config.h`：

| 宏 | 值 | 说明 |
|----|----|------|
| `BOOT_VERSION` | `"v1.0.0"` | Bootloader 版本号 |
| `FLASH_APP_ADDR` | `0x08010000` | APP 起始地址 |
| `FLASH_APP_SIZE` | `0x000F0000` (960 KB) | APP 区大小 |
| `FLASH_APP_META_SIZE` | `32` | 元数据大小 |
| `FIRMWARE_MAGIC` | `0x544F4F42` | 固件魔数 ("BOOT") |
| `UPGRADE_TIMEOUT_MS` | `30000` | 升级超时 (30s) |
| `BOOT_WAIT_TIMEOUT_MS` | `3000` | 启动等待超时 (3s) |
| `BOOT_PACKET_DATA_SIZE`| `1024` | Ymodem 数据包大小 |