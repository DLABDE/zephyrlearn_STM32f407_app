# MCUboot 移植与使用教程 — 基于 STM32F407VET6 + Zephyr RTOS

> **验证状态**: 本文档所有配置已在 STM32F407VET6 DevEBox 开发板上验证通过。
> **环境**: Zephyr 4.4.1-rc1 + MCUboot v2.4.0
> **芯片**: STM32F407VET6 (512KB Flash, 非均匀扇区)
> **项目路径**: `code/blinky_test` (App), `bootloader/mcuboot/boot/zephyr` (MCUboot)
> **板级 DTS**: `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts`

---

## 目录

1. [MCUboot 是什么](#1-mcuboot-是什么)
2. [核心概念](#2-核心概念)
3. [Flash 分区布局设计](#3-flash-分区布局设计)
4. [设备树配置](#4-设备树配置)
5. [编译优化与空间规划](#5-编译优化与空间规划)
6. [编译 MCUboot Bootloader](#6-编译-mcuboot-bootloader)
7. [编译 App（应用程序）](#7-编译-app应用程序)
8. [签名与烧录](#8-签名与烧录)
9. [升级模式详解](#9-升级模式详解)
10. [外部 Flash 作为 slot1 的可行性分析](#10-外部-flash-作为-slot1-的可行性分析)
11. [常见问题排查](#11-常见问题排查)
12. [生产环境注意事项](#12-生产环境注意事项)
13. [实战验证记录](#13-实战验证记录)

---

## 1. MCUboot 是什么

MCUboot 是一个**开源的安全 bootloader**，专为资源受限的微控制器设计。它的核心职责：

```
┌─────────────────────────────────────────────────┐
│                   MCUboot                        │
│                                                  │
│  1. 验证 App 镜像签名（防篡改）                   │
│  2. 管理固件升级（A/B 槽位交换/覆写）              │
│  3. 升级失败自动回滚（防变砖，Swap 模式）          │
│  4. 支持多种签名算法（RSA/ECDSA/Ed25519）         │
│  5. 支持镜像加密                                  │
└─────────────────────────────────────────────────┘
```

**整体架构**：

```
  ┌──────────────┐
  │   MCUboot    │  ← Bootloader，驻留 Flash 起始位置
  │  (48KB)      │     验证签名 → 跳转 App
  └──────┬───────┘
         │ 跳转
         ▼
  ┌──────────────┐
  │   App        │  ← 你的应用程序
  │  (slot0)     │     运行业务逻辑
  └──────────────┘

  OTA 升级流程（Overwrite 模式）：
  App 通过 MCUmgr 接收新固件 → 写入 slot1 → 重启
  → MCUboot 将 slot1 覆写到 slot0 → 新 App 从 slot0 启动
```

**MCUboot 与 MCUmgr 的关系**：

| 组件 | 角色 | 类比 |
|------|------|------|
| MCUboot | Bootloader，负责验证和启动 | 门卫 |
| MCUmgr | 运行时固件传输协议 | 快递员 |

MCUboot 本身**不负责传输固件**，它只做验证和交换。固件传输由 MCUmgr（运行在 App 中）完成。

---

## 2. 核心概念

### 2.1 Flash 分区（Partition）

MCUboot 要求 Flash 划分为以下分区：

| 分区 | 标签 | 用途 | 是否必需 |
|------|------|------|----------|
| boot | mcuboot | 存放 MCUboot 自身代码 | 必需 |
| slot0 | image-0 | 当前运行的 App 镜像（主槽） | 必需 |
| slot1 | image-1 | 升级时存放新 App 镜像（次槽） | 必需 |
| scratch | image-scratch | Swap 模式下的临时交换区 | 仅 Swap 模式 |
| storage | storage | 用户数据存储区 | 可选 |

**关键区别**：
- **Overwrite 模式**：不需要 scratch 分区，slot0 和 slot1 大小不要求相等（slot1 >= slot0 即可）
- **Swap 模式**：需要 scratch 分区，slot0 和 slot1 大小必须相同

### 2.2 签名（Signature）

MCUboot 使用**非对称签名**验证镜像完整性：

```
签名流程（编译时）:
  App 二进制 → imgtool 添加头部 → 用私钥签名 → zephyr.signed.bin

验证流程（启动时）:
  zephyr.signed.bin → MCUboot 用公钥验证签名 → 通过则启动，失败则拒绝
```

**密钥对**：
- **私钥**（`.pem`）：编译 App 时用于签名，**绝不能泄露**
- **公钥**（`.pub`）：编译 MCUboot 时嵌入，用于启动时验证

MCUboot 自带开发用默认密钥 `root-rsa-2048.pem`，**仅用于开发**。

### 2.3 镜像头（Image Header）

签名后的镜像在原始二进制前添加了一个头部（默认 0x400 字节），包含：

```
┌─────────────────────────┐  ← 镜像起始地址
│  MCUboot Image Header   │  0x000 ~ 0x3FF (1KB)
│  - 魔数 (0x96f3b83c)    │
│  - 镜像大小             │
│  - 版本号               │
│  - TLV 区域（签名等）    │
├─────────────────────────┤  ← 0x400
│  App 原始二进制代码      │
│  (向量表 + 代码 + 数据)  │
│  ...                    │
└─────────────────────────┘
```

这就是为什么 App 的 `ROM_START_OFFSET=0x400`——向量表要跳过这个头部。

### 2.4 App 与 MCUboot 的 Kconfig 边界

这是初学者最容易混淆的概念：

```
┌─────────────────────────────────────────────────────────┐
│  App 的 prj.conf (code/blinky_test/prj.conf)            │
│                                                         │
│  CONFIG_BOOTLOADER_MCUBOOT=y        ← 告知 App 在      │
│  CONFIG_MCUBOOT_SIGNATURE_KEY_FILE  ←   MCUboot 下运行  │
│                                                         │
│  ❌ CONFIG_BOOT_UPGRADE_ONLY=y      ← 这是 MCUboot 的! │
│  ❌ CONFIG_BOOT_VALIDATE_SLOT0=y    ← 这是 MCUboot 的! │
│  ❌ CONFIG_BOOT_SWAP_USING_SCRATCH  ← 这是 MCUboot 的! │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  MCUboot 的配置 (构建时 -D 或 boards/<board>.conf)      │
│                                                         │
│  CONFIG_BOOT_UPGRADE_ONLY=y         ← Overwrite 模式   │
│  CONFIG_BOOT_VALIDATE_SLOT0=y       ← 每次启动验证      │
│  CONFIG_BOOT_SWAP_USING_SCRATCH=y   ← Swap 模式        │
│  CONFIG_BOOT_SIGNATURE_KEY_FILE     ← 验证用的公钥      │
└─────────────────────────────────────────────────────────┘
```

**规则**：`CONFIG_BOOT_*` 属于 MCUboot，`CONFIG_BOOTLOADER_MCUBOOT` / `CONFIG_MCUBOOT_*` 属于 App。
把 MCUboot 的配置写在 App 的 prj.conf 中会导致 Kconfig 报错 "undefined symbol"。

---

## 3. Flash 分区布局设计

### 3.1 关键约束

设计分区布局时，必须遵守以下约束：

1. **分区边界必须对齐到扇区边界**（STM32F407 扇区大小不均匀，需特别注意）
2. **Boot 分区必须从 Flash 起始地址开始**（0x08000000）
3. **Swap 模式**：slot0 和 slot1 大小必须相同，scratch >= 最大擦除块
4. **Overwrite 模式**：slot 大小不要求相等，slot1 >= slot0 即可，无需 scratch

### 3.2 STM32F407VET6 Flash 参数

| 参数 | 值 |
|------|-----|
| 总容量 | 512 KB |
| 基地址 | 0x08000000 |
| 写入块大小 | 1 字节 |
| 最小擦除单位 | 整个扇区（大小不均匀！） |

**扇区布局**（这是 STM32F4 系列的特殊之处，扇区大小不均匀）：

| 扇区 | 起始地址 | 大小 | 累计 |
|------|---------|------|------|
| Sector 0 | 0x00000000 | 16KB | 16KB |
| Sector 1 | 0x00004000 | 16KB | 32KB |
| Sector 2 | 0x00008000 | 16KB | 48KB |
| Sector 3 | 0x0000C000 | 16KB | 64KB |
| Sector 4 | 0x00010000 | 64KB | 128KB |
| Sector 5 | 0x00020000 | 128KB | 256KB |
| Sector 6 | 0x00040000 | 128KB | 384KB |
| Sector 7 | 0x00060000 | 128KB | 512KB |

> **与 STM32H743 的区别**：H743 的 16 个扇区都是 128KB，分区对齐很简单。
> F407 的扇区大小不均匀（16K/64K/128K），分区设计必须按扇区边界切分。

### 3.3 分区布局方案（Overwrite 模式）

512KB Flash 空间紧张，选择 Overwrite 模式省去 scratch 分区：

```
Flash 地址空间 (0x0800_0000 ~ 0x0808_0000, 共 512KB)

  0x0800_0000 ┌─────────────────────┐
              │   mcuboot (48KB)    │  Sector 0~2 — Bootloader
  0x0800_C000 ├─────────────────────┤
              │                     │
              │   image-0 (208KB)   │  Sector 3~5 — 主槽 (当前App)
              │                     │
  0x0804_0000 ├─────────────────────┤
              │                     │
              │   image-1 (256KB)   │  Sector 6~7 — 次槽 (升级App)
              │                     │
  0x0808_0000 └─────────────────────┘
```

**地址计算表**（DTS 中地址是相对于 Flash 基地址的偏移）：

| 分区 | DTS 偏移地址 | 绝对地址 | 大小 | 扇区 |
|------|-------------|---------|------|------|
| mcuboot | 0x000000 | 0x08000000 | 48KB | 0~2 |
| image-0 | 0x00C000 | 0x0800C000 | 208KB | 3~5 |
| image-1 | 0x040000 | 0x08040000 | 256KB | 6~7 |

**为什么 mcuboot 是 48KB 而不是 32KB？**

MCUboot 默认配置编译后约 37KB，32KB 放不下。48KB = 3个16KB扇区（Sector 0+1+2），
是最小的能容纳 MCUboot 的扇区组合。如果后续 MCUboot 启用更多功能（如串口恢复模式），
可能需要进一步增大。

**为什么 slot0(208KB) 和 slot1(256KB) 大小不同？**

Overwrite 模式下 slot 大小不要求相等，只要 slot1 >= slot0 即可。
这样 image-1 可以容纳比 image-0 更大的固件。如果使用 Swap 模式，
则 slot0 和 slot1 必须等大，512KB 下空间会更加紧张。

### 3.4 如果使用 Swap 模式（对比参考）

```
512KB Swap 模式布局（不推荐，空间太紧张）:

  mcuboot  : 48KB   (Sector 0~2)
  image-0  : 128KB  (Sector 3~4)     ← 必须等大
  image-1  : 128KB  (Sector 5)       ← 必须等大
  scratch  : 128KB  (Sector 6)       ← 必需
  storage  : 128KB  (Sector 7)       ← 可选

  应用空间仅 128KB，比 Overwrite 模式少了 38%
```

---

## 4. 设备树配置

### 4.1 板级 DTS 分区定义

```dts
&flash0 {
	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		boot_partition: partition@0 {
			label = "mcuboot";
			reg = <0x00000000 DT_SIZE_K(48)>;
			read-only;
		};
		slot0_partition: partition@c000 {
			label = "image-0";
			reg = <0x0000C000 DT_SIZE_K(208)>;
		};
		slot1_partition: partition@40000 {
			label = "image-1";
			reg = <0x00040000 DT_SIZE_K(256)>;
		};
	};
};
```

**关键规则**：
- 分区标签必须是 `mcuboot`、`image-0`、`image-1`、`image-scratch`（MCUboot 按标签查找）
- `partition@` 后的地址必须与 `reg` 中的起始地址一致
- `boot_partition` 标记为 `read-only`，防止 App 意外擦除 Bootloader
- 分区边界必须对齐到扇区边界（不能在扇区中间切分）

### 4.2 chosen 节点配置

MCUboot 和 App 使用**不同的** `zephyr,code-partition`：

**App 的 DTS**（板级 DTS）：
```dts
/ {
	chosen {
		zephyr,console = &usart1;
		zephyr,shell-uart = &usart1;
		zephyr,sram = &sram0;
		zephyr,flash = &flash0;
		zephyr,code-partition = &slot0_partition;  /* App 代码在 slot0 */
	};
};
```

**MCUboot 的 DTS**（通过 app.overlay 自动覆盖）：
```dts
/* bootloader/mcuboot/boot/zephyr/app.overlay — 自动应用 */
/ {
	chosen {
		zephyr,code-partition = &boot_partition;  /* MCUboot 代码在 boot 分区 */
	};
};
```

这个 overlay 会在编译 MCUboot 时自动应用，将代码分区指向 boot_partition。

### 4.3 `zephyr,code-partition` 的作用

`zephyr,code-partition` 决定了：
- 代码被链接到哪个 Flash 地址（`FLASH_LOAD_OFFSET`）
- 向量表放在哪里

```
App:     zephyr,code-partition = &slot0_partition
         → FLASH_LOAD_OFFSET = 0xC000 (slot0 起始偏移)
         → 向量表在 0x0800C000 + 0x400 = 0x0800C400

MCUboot: zephyr,code-partition = &boot_partition
         → FLASH_LOAD_OFFSET = 0x0 (boot 分区起始偏移)
         → 向量表在 0x08000000
```

---

## 5. 编译优化与空间规划

### 5.1 优化等级对 Flash 占用的影响

在 512KB Flash 上，编译优化等级直接影响应用能否放入分区：

| Kconfig | GCC 标志 | 本项目 FLASH 占用 | slot0 使用率 | GDB 体验 |
|---------|---------|------------------|-------------|----------|
| `CONFIG_NO_OPTIMIZATIONS=y` | -O0 | ~235KB | 95.6% | 完美，但几乎放不下 |
| `CONFIG_DEBUG_OPTIMIZATIONS=y` | -Og | ~122KB | ~57% | 良好（甜点） |
| （都不设，默认） | -O2 | ~110KB | ~52% | 变量常 optimized out |
| `CONFIG_SIZE_OPTIMIZATIONS=y` | -Os | ~100KB | ~47% | 较差 |

### 5.2 甜点推荐

```
日常开发: CONFIG_DEBUG_OPTIMIZATIONS=y  (-Og)
  → GDB 基本可用，体积比 -O0 小 47%，空间充裕

发布版本: CONFIG_SIZE_OPTIMIZATIONS=y  (-Os)
  → 体积最小，GDB 体验差但发布版不需要调试
```

### 5.3 prj.conf 优化配置

```ini
# 编译优化等级（四选一，互斥）:
#   CONFIG_NO_OPTIMIZATIONS=y   → -O0  零优化，GDB完美调试，代码体积最大
#   CONFIG_DEBUG_OPTIMIZATIONS=y → -Og  调试优化甜点，GDB基本可用，体积比-O0小30-40%
#   （都不设，默认）             → -O2  速度优化，GDB变量常"optimized out"
#   CONFIG_SIZE_OPTIMIZATIONS=y → -Os  体积最小，GDB体验差
# 推荐：日常开发用 -Og（调试与空间的甜点），发布用 -Os
CONFIG_DEBUG_OPTIMIZATIONS=y
```

---

## 6. 编译 MCUboot Bootloader

### 6.1 MCUboot 源码位置

MCUboot 作为 Zephyr 的 west 模块，位于：
```
<zephyrproject>/bootloader/mcuboot/
```

其 Zephyr 应用入口在：
```
<zephyrproject>/bootloader/mcuboot/boot/zephyr/
```

### 6.2 编译命令

```bash
# 在 zephyrproject 根目录下执行

# 首次编译（pristine 构建）+ Overwrite 模式
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
    -d build_mcuboot -p -- -DCONFIG_BOOT_UPGRADE_ONLY=y

# 增量编译
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
    -d build_mcuboot
```

**参数说明**：
- `-d build_mcuboot`：指定独立的构建目录，避免与 App 构建冲突
- `-p`：pristine 构建，清理旧产物
- `-- -DCONFIG_BOOT_UPGRADE_ONLY=y`：传递 CMake 变量，启用 Overwrite 模式

### 6.3 自定义 MCUboot 配置（推荐方式）

**方式一：板级配置文件**（推荐，持久化）

创建文件 `bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf`：

```ini
# MCUboot 板级配置 — STM32F407VET6 DevEBox
# Overwrite 模式（512KB Flash 空间紧张，不需要 scratch 分区）
CONFIG_BOOT_UPGRADE_ONLY=y

# 如需其他配置可在此添加:
# CONFIG_MCUBOOT_SERIAL=y           # 串口恢复模式
# CONFIG_MCUBOOT_SERIAL_RECOVERY=y  # 串口固件恢复
# CONFIG_MCUBOOT_LOG_LEVEL_OFF=y    # 关闭日志（减小体积）
```

创建后，编译命令简化为：
```bash
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr -d build_mcuboot -p
```

**方式二：编译时 -D 传入**（临时，每次需手动指定）

```bash
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
    -d build_mcuboot -p -- -DCONFIG_BOOT_UPGRADE_ONLY=y
```

### 6.4 编译输出

编译成功后，MCUboot 固件位于：
```
build_mcuboot/zephyr/
├── zephyr.bin         ← 烧录此文件到 0x08000000（未签名，MCUboot 自身不需要签名）
├── zephyr.hex
└── zephyr.elf
```

**验证大小**：
```bash
arm-zephyr-eabi-size build_mcuboot/zephyr/zephyr.elf
#    text    data     bss     dec     hex filename
#   40676     276   32431   73383   11ea7 zephyr.elf
# text+data = 40952 bytes ≈ 40KB，在 48KB 分区内 ✓
```

---

## 7. 编译 App（应用程序）

### 7.1 prj.conf 配置

App 必须告知 Zephyr 它是在 MCUboot 之下运行的：

```ini
# 启动MCUboot
CONFIG_BOOTLOADER_MCUBOOT=y

# 签名密钥文件路径（相对于 west 工作区根目录）
# 开发阶段使用 MCUboot 自带的默认密钥
# 生产环境必须替换为自己的密钥
CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="bootloader/mcuboot/root-rsa-2048.pem"

# Overwrite 模式配置说明:
#   CONFIG_BOOT_UPGRADE_ONLY 是 MCUboot 自身的 Kconfig，不属于应用配置
#   需在构建 MCUboot 时通过 -D 传入:
#     west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
#       -d build_mcuboot -- -DCONFIG_BOOT_UPGRADE_ONLY=y
#   或创建 MCUboot 板级配置文件:
#     bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf
```

### 7.2 `CONFIG_BOOTLOADER_MCUBOOT=y` 做了什么

启用此选项后，Zephyr 构建系统会自动：

1. 设置 `USE_DT_CODE_PARTITION=y` → 使用 DTS 中 `zephyr,code-partition` 指向的分区
2. 设置 `FLASH_LOAD_OFFSET` = slot0 分区的起始偏移（如 0xC000）
3. 设置 `ROM_START_OFFSET=0x400` → 向量表偏移 0x400 字节（MCUboot 镜像头大小）
4. 编译完成后自动调用 `imgtool sign` 对 `zephyr.bin` 签名
5. 生成 `zephyr.signed.bin` 和 `zephyr.signed.hex`

### 7.3 编译命令

```bash
# 首次编译（pristine 构建）
west build -b stm32f4_devebox code/blinky_test -p

# 增量编译
west build -b stm32f4_devebox code/blinky_test

# 如果签名步骤未自动执行，手动签名：
west build -t sign_app
```

### 7.4 编译输出

```
build/zephyr/
├── zephyr.bin           ← 原始未签名镜像（不要直接烧录！）
├── zephyr.signed.bin    ← 签名后的镜像（烧录这个！）
├── zephyr.signed.hex    ← 签名后的 HEX 格式
└── zephyr.elf           ← 调试用 ELF 文件
```

---

## 8. 签名与烧录

### 8.1 签名原理

```
                    编译时
  ┌──────────┐    ┌──────────┐    ┌──────────────────┐
  │ zephyr.  │───→│ imgtool  │───→│ zephyr.signed.   │
  │ bin      │    │ sign     │    │ bin               │
  └──────────┘    └────┬─────┘    └──────────────────┘
                      │
                      │ 使用私钥签名
                      ▼
               ┌──────────────┐
               │ root-rsa-    │
               │ 2048.pem     │  ← 私钥（仅编译时使用）
               └──────────────┘

                    启动时
  ┌──────────────────┐    ┌──────────────┐
  │ zephyr.signed.   │───→│   MCUboot    │
  │ bin (在Flash中)  │    │   验证签名    │
  └──────────────────┘    └──────┬───────┘
                                │
                                │ 使用嵌入的公钥验证
                                ▼
                         ┌──────────────┐
                         │ 公钥（编译时  │
                         │ 嵌入MCUboot）│
                         └──────────────┘
```

### 8.2 手动签名（了解原理）

如果需要手动签名（不通过 Zephyr 构建系统）：

```bash
imgtool sign \
    --key bootloader/mcuboot/root-rsa-2048.pem \
    --header-size 0x400 \
    --align 1 \
    --version 1.0.0 \
    --slot-size 0x34000 \
    build/zephyr/zephyr.bin \
    build/zephyr/zephyr.signed.bin
```

参数说明：
- `--key`：签名私钥路径
- `--header-size`：镜像头大小（Zephyr 默认 0x400）
- `--align`：对齐（STM32F407 写入块 1 字节）
- `--version`：镜像版本号
- `--slot-size`：slot0 分区大小（208KB = 0x34000）

### 8.3 烧录步骤

**首次部署（全 Flash 烧录）**：

```
步骤 1: 烧录 MCUboot 到 Flash 起始地址
  地址: 0x08000000
  文件: build_mcuboot/zephyr/zephyr.bin

步骤 2: 烧录签名后的 App 到 slot0 地址
  地址: 0x0800C000 (= 0x08000000 + 0xC000)
  文件: build/zephyr/zephyr.signed.bin
```

**使用 west flash 烧录**：

```bash
# 烧录 MCUboot
west flash --build-dir build_mcuboot

# 烧录 App（构建系统自动计算 slot0 偏移地址）
west flash
```

### 8.4 烧录地址速查

| 固件 | 烧录地址 | 文件 |
|------|---------|------|
| MCUboot | 0x08000000 | zephyr.bin（未签名） |
| App → slot0 | 0x0800C000 | zephyr.signed.bin（已签名） |
| App → slot1（OTA测试） | 0x08040000 | zephyr.signed.bin（已签名） |

---

## 9. 升级模式详解

MCUboot 支持多种升级模式，通过 MCUboot 侧的 Kconfig 选择：

### 9.1 Overwrite Only（当前使用）

```
Kconfig: CONFIG_BOOT_UPGRADE_ONLY=y

升级过程：
  直接将 slot1 内容复制到 slot0（覆盖）

  升级前:  slot0 = App v1    slot1 = App v2
  升级后:  slot0 = App v2    slot1 = (被擦除)

优点：简单快速，不需要 scratch 分区，最省空间
缺点：不支持回滚，升级中断可能变砖
要求：slot1 >= slot0
适用：Flash 空间紧张的设备（如 512KB 的 STM32F407）
```

### 9.2 Swap using Scratch

```
Kconfig: CONFIG_BOOT_SWAP_USING_SCRATCH=y

升级过程：
  slot0(A) + slot1(B) + scratch
       ↓         ↓          ↓
  1. 将 slot0 的最后一个扇区移到 scratch
  2. 将 slot1 的最后一个扇区移到 slot0
  3. 将 scratch 中的扇区移到 slot1
  4. 重复，直到所有扇区交换完成

优点：支持自动回滚
缺点：需要 scratch 分区，交换过程较慢
要求：slot0 == slot1 大小，scratch >= 最大擦除块
适用：Flash 充裕、安全性要求高的设备
```

### 9.3 Direct XIP

```
Kconfig: CONFIG_BOOT_DIRECT_XIP=y

升级过程：
  不复制，直接在 slot1 中执行新固件

优点：最快，无需复制
缺点：slot0 和 slot1 必须有相同的地址映射，不支持回滚
适用：对启动速度要求极高的场景
```

### 9.4 模式选择决策树

```
Flash 空间是否充裕？
  ├─ 否 → Overwrite Only（省去 scratch，slot 大小灵活）
  └─ 是 → 是否需要回滚能力？
       ├─ 是 → Swap using Scratch
       └─ 否 → Overwrite Only（更简单快速）
```

---

## 10. 外部 Flash 作为 slot1 的可行性分析

本项目使用外部 W25Q16 (2MB) 通过 SPI 连接，一个自然的想法是：能否将 slot1 放在外部 Flash 上，从而释放更多内部 Flash 给应用？

### 10.1 现有成功案例

Zephyr 生态中确实有外部 Flash slot1 的成功案例：

| 开发板 | 外部 Flash | 连接方式 | 关键配置 |
|--------|-----------|---------|---------|
| disco_l475_iot1 | MX25R6435F | QSPI | `CONFIG_STM32_MEMMAP=y` |
| nrf52840dk | MX25R64 | QSPI | `CONFIG_NORDIC_QSPI_NOR=y` |
| stm32h750b_dk | MT25QL512AB | QSPI | `CONFIG_STM32_MEMMAP=y` |

**共同点**：全部使用 **QSPI Memory-Mapped 模式**。

### 10.2 为什么 STM32F407 + 普通 SPI 不可行

```
QSPI Memory-Mapped 模式（可行）:
  CPU 直接通过地址总线访问外部 Flash
  0x90000000 地址 → QSPI 控制器 → 外部 Flash
  MCUboot 可以像访问内部 Flash 一样访问外部 Flash

普通 SPI 模式（本项目）:
  CPU 通过 SPI 外设寄存器间接访问
  需要先发 SPI 命令 → 等待 → 读取数据
  MCUboot 无法像访问内部 Flash 一样直接寻址
```

**具体障碍**：

1. **`flash_area_get_device_id()` 只返回单一设备 ID**
   - MCUboot 的 `flash_map_extended.c` 中，此函数忽略参数，始终返回 `FLASH_DEVICE_ID`
   - slot0 在内部 Flash（ID=0），slot1 在外部 Flash（ID=1），无法区分

2. **普通 SPI 驱动初始化时序问题**
   - QSPI Memory-Mapped 模式下，外部 Flash 在启动早期就可访问
   - 普通 SPI 需要先初始化 SPI 控制器、GPIO 等，MCUboot 启动阶段可能来不及

3. **无现成参考实现**
   - Zephyr 代码库中没有 STM32F4 + 普通 SPI Flash 的 MCUboot 方案

### 10.3 如果一定要用外部 Flash slot1

需要自定义以下代码（复杂度较高）：

1. 修改 `flash_area_get_device_id()` 根据 `fa` 参数返回正确的设备 ID
2. 确保 SPI 驱动在 MCUboot 启动早期完成初始化
3. 使用 `compatible = "zephyr,mapped-partition"` 处理跨设备分区
4. 只能使用 Overwrite 模式（Swap 模式跨设备更复杂）

> **建议**：学习阶段使用内部 Flash Overwrite 模式即可。如需更大应用空间，
> 可考虑升级到带 QSPI 控制器的 MCU（如 STM32F412/STM32H7 系列）。

---

## 11. 常见问题排查

### 11.1 `Image in the primary slot is not valid!`

**原因**：烧录了未签名的 `zephyr.bin`，MCUboot 验证签名失败。

**解决**：
1. 确认 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 已设置
2. 烧录 `zephyr.signed.bin` 而非 `zephyr.bin`
3. 如果签名步骤未自动执行，手动运行：`west build -t sign_app`

### 11.2 `region FLASH overflowed by N bytes`（MCUboot 编译溢出）

**原因**：MCUboot 编译后体积超过了 boot 分区大小。

**解决**：
1. 增大 boot 分区（如 32KB → 48KB）
2. 注意分区边界必须对齐扇区：48KB = Sector 0+1+2（3个16KB扇区）
3. 调整 boot 分区后，slot0 的起始偏移和大小也要相应调整

### 11.3 Kconfig 报错 "attempt to assign the value 'y' to the undefined symbol"

**现象**：
```
prj.conf:19: warning: attempt to assign the value 'y' to the undefined symbol BOOT_UPGRADE_ONLY
error: Aborting due to Kconfig warnings
```

**原因**：把 MCUboot 的 Kconfig 配置写在了 App 的 prj.conf 中。

**解决**：`CONFIG_BOOT_UPGRADE_ONLY` 等属于 MCUboot，需在构建 MCUboot 时指定：
- 方式一：`-DCONFIG_BOOT_UPGRADE_ONLY=y`
- 方式二：创建 `bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf`

### 11.4 App 启动后 Hard Fault

**原因**：向量表地址不正确。

**检查**：
1. `CONFIG_BOOTLOADER_MCUBOOT=y` 是否启用
2. `ROM_START_OFFSET` 是否为 0x400
3. `FLASH_LOAD_OFFSET` 是否等于 slot0 起始偏移（0xC000）

### 11.5 编译后没有 zephyr.signed.bin

**原因**：签名密钥路径为空或错误。

**解决**：
1. 检查 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 是否设置
2. 确认密钥文件路径相对于 west 工作区根目录正确
3. 手动签名：`west build -t sign_app`

### 11.6 DTS 分区改动后编译报奇怪的错误

**原因**：旧的 `.config` 缓存与新的 DTS 不一致。

**解决**：使用 pristine 构建 `west build -p`，或手动删除 build 目录。

---

## 12. 生产环境注意事项

### 12.1 生成自己的密钥对

```bash
# 生成 RSA-2048 密钥对
imgtool keygen -k my-key.pem -t rsa-2048

# 提取公钥（嵌入 MCUboot）
imgtool getpub -k my-key.pem -o my-key.pub

# 生成 ECDSA-P256 密钥对（更小更快）
imgtool keygen -k my-key.pem -t ecdsa-p256
```

### 12.2 MCUboot 使用自定义密钥

**方式一**：板级覆盖文件
```ini
# bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf
CONFIG_BOOT_SIGNATURE_KEY_FILE="my-key.pem"
```

**方式二**：编译时指定
```bash
west build ... -- -DCONFIG_BOOT_SIGNATURE_KEY_FILE="my-key.pem"
```

### 12.3 安全清单

- [ ] 替换默认签名密钥 `root-rsa-2048.pem`
- [ ] 私钥文件不要放入版本控制（加入 .gitignore）
- [ ] 考虑启用镜像加密（`CONFIG_BOOT_ENCRYPT_IMAGE=y`）
- [ ] 考虑启用降级防护（`CONFIG_MCUBOOT_DOWNGRADE_PREVENTION=y`）
- [ ] MCUboot 日志级别设为 OFF（`CONFIG_MCUBOOT_LOG_LEVEL_OFF=y`）
- [ ] 禁用串口恢复模式（不定义 `CONFIG_MCUBOOT_SERIAL`）

---

## 13. 实战验证记录

本章记录在 STM32F407VET6 DevEBox 上从零配置 MCUboot 的完整过程，包括遇到的问题和解决方案。

### 13.1 初始配置

**DTS 分区布局**（[stm32f4_devebox.dts](file:///f:/dzd/zephyr/zephyrproject/zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts#L146-L203)）：

```dts
&flash0 {
	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		boot_partition: partition@0 {
			label = "mcuboot";
			reg = <0x00000000 DT_SIZE_K(48)>;
			read-only;
		};
		slot0_partition: partition@c000 {
			label = "image-0";
			reg = <0x0000C000 DT_SIZE_K(208)>;
		};
		slot1_partition: partition@40000 {
			label = "image-1";
			reg = <0x00040000 DT_SIZE_K(256)>;
		};
	};
};
```

**App prj.conf**（[prj.conf](file:///f:/dzd/zephyr/zephyrproject/code/blinky_test/prj.conf#L13-L24)）：

```ini
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="bootloader/mcuboot/root-rsa-2048.pem"
```

**chosen 节点**（[stm32f4_devebox.dts](file:///f:/dzd/zephyr/zephyrproject/zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts#L38-L44)）：

```dts
chosen {
	zephyr,console = &usart1;
	zephyr,shell-uart = &usart1;
	zephyr,sram = &sram0;
	zephyr,flash = &flash0;
	zephyr,code-partition = &slot0_partition;
};
```

### 13.2 编译 MCUboot

```bash
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
    -d build_mcuboot -p -- -DCONFIG_BOOT_UPGRADE_ONLY=y
```

输出产物：
```
build_mcuboot/zephyr/
├── zephyr.bin    ← 烧录到 0x08000000 (约 40KB)
├── zephyr.hex
└── zephyr.elf
```

### 13.3 编译 App

```bash
west build -b stm32f4_devebox code/blinky_test -p
```

输出产物：
```
build/zephyr/
├── zephyr.bin           ← 未签名，不要烧录
├── zephyr.signed.bin    ← 已签名，烧录到 0x0800C000
├── zephyr.signed.hex
└── zephyr.elf
```

### 13.4 构建结果

| 组件 | 分区大小 | 实际占用 | 使用率 |
|------|---------|---------|--------|
| MCUboot | 48KB | ~40KB | 83% |
| App (-Og) | 208KB (slot0) | ~122KB | 57% |

### 13.5 遇到的问题与解决

#### 问题 1：MCUboot 编译溢出 32KB 分区

**现象**：
```
region `FLASH' overflowed by 4668 bytes
```

**原因**：MCUboot 默认配置编译后约 37KB，初始分区仅 32KB（2个16KB扇区）。

**解决**：将 boot 分区从 32KB 增大到 48KB（3个16KB扇区），同时调整 slot0 起始偏移。

**教训**：STM32F4 的扇区大小不均匀（16K/64K/128K），分区调整不是简单的倍数关系，
每次调整都需要重新计算扇区边界。

#### 问题 2：CONFIG_BOOT_UPGRADE_ONLY 写在 App prj.conf 中导致编译失败

**现象**：
```
prj.conf:19: warning: attempt to assign the value 'y' to the undefined symbol BOOT_UPGRADE_ONLY
error: Aborting due to Kconfig warnings
```

**原因**：`CONFIG_BOOT_UPGRADE_ONLY` 是 MCUboot 的 Kconfig 符号，在 App 的配置空间中不存在。

**解决**：从 App 的 prj.conf 中移除该配置，改为在构建 MCUboot 时通过 `-D` 传入或创建板级配置文件。

**教训**：App 和 MCUboot 有各自独立的 Kconfig 空间，不能混用。简单判断规则：
- `CONFIG_BOOT_*` → 属于 MCUboot
- `CONFIG_BOOTLOADER_MCUBOOT` / `CONFIG_MCUBOOT_*` → 属于 App

#### 问题 3：初始分区方案超出 512KB

**现象**：最初的分区方案（mcuboot 128KB + slot0 640KB + slot1 640KB + scratch 128KB + storage 256KB）
总计远超 512KB。

**原因**：直接复制了 STM32H743 (2MB Flash) 的分区方案，未根据实际 Flash 大小调整。

**解决**：根据 512KB 限制重新规划，选择 Overwrite 模式省去 scratch 分区，
mcuboot 缩减到 48KB，slot0 208KB，slot1 256KB。

**教训**：分区方案必须根据芯片实际 Flash 大小定制，不能直接照搬其他板子的配置。

### 13.6 关键经验总结

1. **分区边界必须对齐扇区边界**，STM32F4 扇区大小不均匀，需格外注意
2. **MCUboot 体积约 37-40KB**，boot 分区至少 48KB（3个16KB扇区）
3. **必须烧录签名镜像** `zephyr.signed.bin`，不是 `zephyr.bin`
4. **App 和 MCUboot 的 Kconfig 空间独立**，不能把 MCUboot 配置写在 App 的 prj.conf 中
5. **编译优化等级对 Flash 占用影响巨大**：-O0 约 235KB，-Og 约 122KB，-Os 约 100KB
6. **512KB Flash 推荐使用 Overwrite 模式**，省去 scratch 分区，slot 大小灵活
7. **普通 SPI 外部 Flash 不适合作为 MCUboot slot1**，需要 QSPI Memory-Mapped 模式
8. DTS 改动后需要 **pristine 构建**，否则旧的 `.config` 缓存会导致奇怪的错误
9. **签名密钥路径是相对于 west 工作区根目录**（即 `zephyrproject/` 目录）

---

## 附录 A：快速参考卡

### 编译命令

```bash
# 编译 MCUboot（Overwrite 模式）
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr \
    -d build_mcuboot -p -- -DCONFIG_BOOT_UPGRADE_ONLY=y

# 编译 App（自动签名）
west build -b stm32f4_devebox code/blinky_test -p

# 手动签名
west build -t sign_app
```

### 烧录地址

| 固件 | 地址 | 文件 |
|------|------|------|
| MCUboot | 0x08000000 | zephyr.bin（未签名） |
| App (slot0) | 0x0800C000 | zephyr.signed.bin（已签名） |
| App (slot1) | 0x08040000 | zephyr.signed.bin（已签名） |

### 分区布局速查

| 分区 | 偏移 | 大小 | 扇区 |
|------|------|------|------|
| mcuboot | 0x000000 | 48KB | 0~2 |
| image-0 | 0x00C000 | 208KB | 3~5 |
| image-1 | 0x040000 | 256KB | 6~7 |

### 关键 Kconfig 速查

| 配置 | 用途 | 用在 |
|------|------|------|
| `CONFIG_BOOTLOADER_MCUBOOT=y` | 告知 App 运行在 MCUboot 下 | App |
| `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` | App 签名密钥路径 | App |
| `CONFIG_BOOT_UPGRADE_ONLY=y` | Overwrite 升级模式 | MCUboot |
| `CONFIG_BOOT_VALIDATE_SLOT0=y` | 每次启动验证 slot0 | MCUboot |
| `CONFIG_BOOT_SWAP_USING_SCRATCH=y` | Swap 升级模式 | MCUboot |
| `CONFIG_DEBUG_OPTIMIZATIONS=y` | -Og 调试优化甜点 | App |
| `CONFIG_SIZE_OPTIMIZATIONS=y` | -Os 体积优化 | App |

### MCUmgr CLI 命令（OTA 升级）

```bash
# 安装 mcumgr CLI 工具
pip install mcumgr

# 查询镜像状态
mcumgr --conntype serial --conndev <port> image list

# 上传新固件到 slot1
mcumgr --conntype serial --conndev <port> image upload zephyr.signed.bin

# 标记待升级
mcumgr --conntype serial --conndev <port> image test <hash>

# 确认当前固件（取消回滚）
mcumgr --conntype serial --conndev <port> image confirm

# 重启设备
mcumgr --conntype serial --conndev <port> reset
```
