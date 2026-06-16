# MCUboot 完整教程 — STM32F407VET6 + W25Q16 外部 Flash + MCUmgr OTA

> **验证状态**: 所有配置已在 STM32F407VET6 DevEBox 上完整验证通过（包括 OTA 升级）。
> **环境**: Zephyr 4.4.1-rc1 + MCUboot v2.4.0
> **芯片**: STM32F407VET6 (512KB Flash, 非均匀扇区)
> **外部 Flash**: W25Q16 (2MB SPI NOR)
> **项目路径**: `code/blinky_test` (App), `bootloader/mcuboot/boot/zephyr` (MCUboot)
> **板级文件**: `zephyr/boards/st/stm32f407vet6_devebox/`

---

## 目录

1. [MCUboot 是什么](#1-mcuboot-是什么)
2. [核心概念](#2-核心概念)
3. [硬件资源与分区规划](#3-硬件资源与分区规划)
4. [Kconfig 配置边界](#4-kconfig-配置边界)
5. [设备树 DTS 配置](#5-设备树-dts-配置)
6. [MCUboot 编译配置](#6-mcuboot-编译配置)
7. [App 编译配置](#7-app-编译配置)
8. [MCUmgr OTA 升级配置](#8-mcumgr-ota-升级配置)
9. [跨设备 Flash 原理](#9-跨设备-flash-原理)
10. [编译、签名与烧录](#10-编译签名与烧录)
11. [OTA 升级流程](#11-ota-升级流程)
12. [踩坑记录与排查](#12-踩坑记录与排查)
13. [附录：快速参考](#13-附录快速参考)

---

## 1. MCUboot 是什么

MCUboot 是面向微控制器的开源安全 bootloader，核心职责：

```
┌─────────────────────────────────────────────────┐
│                   MCUboot                        │
│                                                  │
│  1. 验证 App 镜像签名（防篡改）                   │
│  2. 管理固件升级（A/B 槽位交换/覆写）              │
│  3. 升级失败自动回滚（防变砖，Swap 模式）          │
│  4. 支持多种签名算法（RSA-2048/ECDSA/Ed25519）    │
│  5. 支持镜像加密（AES-128/256）                   │
└─────────────────────────────────────────────────┘
```

### MCUboot 与 MCUmgr 的分工

| 组件 | 角色 | 工作量 |
|------|------|--------|
| **MCUboot** | Bootloader，启机时验证签名 → 跳转/升级 → 再度启动 | "门卫" |
| **MCUmgr** | 运行时固件传输协议 + 固件写入 Flash | "快递员" |

MCUboot 本身不传输固件。OTA 流程：App 中的 MCUmgr 接收新固件并写入 slot1 → 标记升级 → 复位 → MCUboot 接管升级。

---

## 2. 核心概念

### 2.1 Flash 分区

| 分区标签 | 用途 | 是否必需 |
|----------|------|----------|
| `mcuboot` | MCUboot 自身代码 | 必需 |
| `image-0` | slot0，当前运行的 App（主槽） | 必需 |
| `image-1` | slot1，升级新 App 暂存（次槽） | 必需 |
| `image-scratch` | Swap 模式临时交换区 | 仅 Swap 模式 |

**Overwrite 模式**: slot0 和 slot1 大小不要求相等，slot1 >= slot0 即可，无需 scratch 分区。

### 2.2 签名与镜像头

```
编译时签名:
  zephyr.bin → imgtool 添加头部(0x400) + 签名 → zephyr.signed.bin

启机时验证:
  zephyr.signed.bin → MCUboot 用嵌入的公钥验证 → 通过则启动

镜像结构:
┌─────────────────────────┐
│  Image Header (0x400)   │ ← magic=0x96f3b83d
│  - 魔数 + 版本 + 大小    │
├─────────────────────────┤
│  App 代码                │
│  ...                     │
├─────────────────────────┤
│  TLV 区域                │ ← 签名/Hash/Key 信息
│  - SHA256 Hash           │
│  - RSA-2048 签名         │
└─────────────────────────┘
```

### 2.3 升级模式对比

| 模式 | MCUboot Kconfig | 特点 | 适用场景 |
|------|----------------|------|---------|
| **Overwrite Only** | `BOOT_UPGRADE_ONLY=y` | slot1 直接覆写 slot0，不需 scratch，不支持回滚 | Flash 空间紧张 |
| Swap using Scratch | `BOOT_SWAP_USING_SCRATCH=y` | 逐扇区交换，支持回滚，需 scratch 分区 | 安全要求高 |
| Swap using Move | `BOOT_SWAP_USING_MOVE=y` | 不需 scratch，但 slot0 == slot1 | 中等空间 |

---

## 3. 硬件资源与分区规划

### 3.1 STM32F407VET6 内部 Flash

| 参数 | 值 |
|------|-----|
| 总容量 | 512 KB |
| 写入块大小 | 1 字节 |
| 最小擦除单位 | 整个扇区（大小不均匀） |

**扇区布局**（大小不均匀，分区必须对齐扇区边界）：

| 扇区 | 起始偏移 | 大小 |
|------|---------|------|
| Sector 0 | 0x00000 | 16KB |
| Sector 1 | 0x04000 | 16KB |
| Sector 2 | 0x08000 | 16KB |
| Sector 3 | 0x0C000 | 16KB |
| Sector 4 | 0x10000 | 64KB |
| Sector 5 | 0x20000 | 128KB |
| Sector 6 | 0x40000 | 128KB |
| Sector 7 | 0x60000 | 128KB |

### 3.2 W25Q16 外部 SPI Flash

| 参数 | 值 |
|------|-----|
| 总容量 | 2 MB (16 Mbit) |
| 最小擦除单位 | 4 KB (Sector Erase) |
| 接口 | SPI (非 QSPI) |
| DTS 节点 | `w25qxx@0` (compatible: `jedec,spi-nor`) |

### 3.3 最终分区方案（已验证通过）

```
内部 Flash (512KB)
0x00000000 ┌────────────────────┐
          │  mcuboot (48KB)     │ Sector 0~2 — Bootloader
0x0000C000├────────────────────┤
          │                    │
          │  slot0 / image-0   │ Sector 3~7 — 主槽 (当前 App)
          │       (464KB)      │
          │                    │
0x00080000└────────────────────┘

外部 W25Q16 (2MB)
0x00000000 ┌────────────────────┐
          │  lfs (1MB)         │ LittleFS 文件系统
0x00100000├────────────────────┤
          │  slot1 / image-1   │ 升级固件暂存 (464KB)
          │       (464KB)      │
0x00174000├────────────────────┤
          │  storage (560KB)   │ 预留
0x00200000└────────────────────┘
```

**分区地址速查**：

| 分区 | Flash 设备 | 偏移 | 大小 | 扇区范围 |
|------|-----------|------|------|---------|
| mcuboot | 内部 Flash | 0x000000 | 48KB | Sector 0~2 |
| slot0 (image-0) | 内部 Flash | 0x00C000 | 464KB | Sector 3~7 |
| lfs | W25Q16 | 0x000000 | 1MB | — |
| **slot1 (image-1)** | **W25Q16** | **0x100000** | **464KB** | — |
| storage | W25Q16 | 0x174000 | 560KB | — |

### 3.4 为什么选择这个方案

1. **slot1 放在外部 Flash**：释放内部 Flash 全部给 slot0（464KB vs 旧方案的 208KB），应用空间翻倍
2. **Overwrite 模式**：不需要 scratch 分区，进一步节约空间
3. **LittleFS 在 W25Q16**：利用外部 Flash 余量做文件系统，存放配置参数

---

## 4. Kconfig 配置边界

> **这是最容易被忽略但最容易踩坑的概念**。App 和 MCUboot 有各自独立的 Kconfig 命名空间。

### 4.1 App 的 Kconfig（prj.conf）

App 中可用的 Kconfig 前缀：

```
CONFIG_BOOTLOADER_MCUBOOT     ← 告知 App 运行在 MCUboot 下
CONFIG_MCUBOOT_*              ← App 端 MCUboot 相关配置
CONFIG_MCUMGR_*               ← MCUmgr 配置
CONFIG_IMG_MANAGER            ← DFU 镜像管理器
CONFIG_STREAM_FLASH            ← 流式 Flash 写入
```

### 4.2 MCUboot 的 Kconfig（构建时指定）

MCUboot 中可用的 Kconfig 前缀：

```
CONFIG_BOOT_UPGRADE_ONLY       ← Overwrite 模式
CONFIG_BOOT_SWAP_USING_SCRATCH ← Swap 模式
CONFIG_BOOT_VALIDATE_SLOT0     ← 每次启动验证 slot0
CONFIG_BOOT_SIGNATURE_KEY_FILE ← 验证密钥
CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE ← SPI NOR 页大小
CONFIG_BOOT_MAX_IMG_SECTORS    ← 镜像最大扇区数
```

### 4.3 绝对不能做的事

```ini
# ❌ 错误：把 MCUboot 的配置写在 App 的 prj.conf 中
CONFIG_BOOT_UPGRADE_ONLY=y      # Kconfig 报错 "undefined symbol BOOT_UPGRADE_ONLY"

# ❌ 错误：把 App 的配置写在 MCUboot 配置中
CONFIG_BOOTLOADER_MCUBOOT=y     # 在 MCUboot 编译中无意义
```

### 4.4 【关键】`MCUBOOT_BOOTLOADER_MODE_*` 必须与 MCUboot 一致

这是本项目中发现的**第一个重大踩坑**。Zephyr 为 App 提供了 `choice MCUBOOT_BOOTLOADER_MODE` 来告知 App MCUboot 使用的是什么升级模式。**默认值是 `SWAP_USING_OFFSET`**。

如果 App 不显式设置此选项，且 MCUboot 配置的是 Overwrite 模式，则会导致：

| 配置 | MCUboot | App (默认) |
|------|---------|------------|
| 模式 | Overwrite | SWAP_USING_OFFSET |
| 写入 slot1 起始偏移 | — | fa_off + 0x1000（跳过一个扇区） |
| 读取 slot1 起始偏移 | fa_off（slot1 起始处） | — |
| **结果** | **MCUboot 在 offset 0 读到全 0xFF** | **MCUmgr 写到 offset 0x1000** |

**解决**：App 必须显式设置与 MCUboot 相同的模式：

```ini
# App prj.conf — 与 MCUboot 的 CONFIG_BOOT_UPGRADE_ONLY=y 必须一致
CONFIG_MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY=y
```

详细源码路径：`zephyr/subsys/dfu/img_util/flash_img.c` 的 `flash_img_init_id()`：

```c
#if defined(CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET)
    // Swap using offset 模式：跳过第一个扇区
    return stream_flash_init(..., fa_off + sector_data.fs_size, ...);
#else
    // Overwrite 等其他模式：从 slot1 起始处开始
    return stream_flash_init(..., fa_off, ...);
#endif
```

---

## 5. 设备树 DTS 配置

### 5.1 内部 Flash 分区

文件：`zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts`

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        boot_partition: partition@0 {
            label = "mcuboot";
            reg = <0x00000000 DT_SIZE_K(48)>;
            read-only;   /* 防止 App 意外擦除 bootloader */
        };
        slot0_partition: partition@c000 {
            label = "image-0";
            reg = <0x0000C000 DT_SIZE_K(464)>;
        };
    };
};
```

**关键规则**：
- 分区标签必须是 `mcuboot`、`image-0`、`image-1`（MCUboot 按标签查找）
- 分区边界必须对齐到扇区边界（48KB = 3 × 16KB）
- `boot_partition` 标记 `read-only`
- 编译优化等级 `-Og` 下 App 约 122KB，slot0 有 464KB，空间充裕

### 5.2 外部 Flash (W25Q16) 分区

```dts
&spi1 {
    status = "okay";
    pinctrl-0 = <&spi1_sck_pb3 &spi1_miso_pb4 &spi1_mosi_pb5>;
    pinctrl-names = "default";
    cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>;

    w25qxx: w25qxx@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <80000000>;
        jedec-id = [ef 40 15];
        size = <16777216>;   /* 16 Mbit = 2 MB */

        partitions {
            compatible = "fixed-partitions";
            #address-cells = <1>;
            #size-cells = <1>;

            lfs_partition: partition@0 {
                label = "lfs";
                reg = <0x00000000 0x00100000>;   /* 前 1MB → LittleFS */
            };

            slot1_partition: partition@100000 {
                label = "image-1";
                reg = <0x00100000 0x00074000>;   /* 464KB → MCUboot slot1 */
            };

            storage_partition: partition@174000 {
                label = "storage";
                reg = <0x00174000 0x0008c000>;   /* 后 560KB → 预留 */
            };
        };
    };
};
```

**关键点**：
- `slot1_partition` 的标签必须是 `image-1`
- `slot1_partition` 在 W25Q16 上的起始位置是 0x100000（1MB 偏移），大小 464KB（0x74000），与 slot0 等大
- LittleFS 占用前 1MB，slot1 在 1MB 之后

### 5.3 chosen 节点

```dts
chosen {
    zephyr,code-partition = &slot0_partition;  /* App 代码链接到 slot0 */
};
```

MCUboot 编译时通过 `bootloader/mcuboot/boot/zephyr/app.overlay` 自动覆盖为 `zephyr,code-partition = &boot_partition`。

---

## 6. MCUboot 编译配置

### 6.1 板级配置文件

文件：`bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf`

```ini
# Overwrite 模式：升级时将 slot1 覆写到 slot0
CONFIG_BOOT_UPGRADE_ONLY=y

# SPI NOR Flash 页大小（最小擦除单位）
# W25Q16 最小擦除单位是 4KB，默认 64KB 会导致 boot_slots_compatible() 检查失败
CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096

# 镜像最大扇区数
# slot1 在 W25Q16 上: 464KB / 4KB ≈ 116 个扇区
# 默认值 128 可能不够（需额外 trailer 等），设为 256 留余量
# 必须禁用 AUTO 模式，否则显式值不生效
CONFIG_BOOT_MAX_IMG_SECTORS_AUTO=n
CONFIG_BOOT_MAX_IMG_SECTORS=256

# 调试日志（问题确认后建议改回 INF）
CONFIG_MCUBOOT_LOG_LEVEL_DBG=y
CONFIG_MCUBOOT_UTIL_LOG_LEVEL_DBG=y
```

### 6.2 `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` 为什么是 4096

这是**第二个关键踩坑点**。

W25Q16 的最小擦除单位是 4KB。Zephyr SPI NOR 驱动默认 `SPI_NOR_FLASH_LAYOUT_PAGE_SIZE = 65536`（64KB）。如果使用默认值：

1. MCUboot 的 `boot_slots_compatible()` 会用 64KB 作为 slot1 的"扇区大小"
2. 内部 Flash 的 slot0 最小扇区是 16KB
3. 64KB 不能整除 16KB → 兼容性检查失败 → MCUboot 拒绝升级

**正确做法**：设为 4096（W25Q16 的真实最小擦除单位），这样 4KB 能整除 16KB/64KB/128KB。

### 6.3 `CONFIG_BOOT_MAX_IMG_SECTORS` 为什么是 256

- slot1 在 W25Q16 上的扇区数 = 464KB / 4KB = 116 个
- 默认值 128 理论上够，但 MCUboot 内部需要额外扇区用于 trailer 等标记
- 设为 256 留足余量，且必须设置 `AUTO=n` 才能使显式值生效

### 6.4 MCUboot 主配置文件

文件：`bootloader/mcuboot/boot/zephyr/prj.conf`

```ini
CONFIG_PM=n
CONFIG_MAIN_STACK_SIZE=10240
CONFIG_BOOT_SWAP_SAVE_ENCTLV=n
CONFIG_BOOT_ENCRYPT_IMAGE=n
CONFIG_BOOT_UPGRADE_ONLY=n       # 默认值，由板级文件覆盖为 y
CONFIG_BOOT_BOOTSTRAP=n
CONFIG_FLASH=y
CONFIG_LOG=y
CONFIG_LOG_MODE_MINIMAL=y
CONFIG_MCUBOOT_LOG_LEVEL_INF=y
CONFIG_CBPRINTF_NANO=y           # 比 CBPRINTF_COMPLETE 小约 4KB
CONFIG_PICOLIBC=y                # 使用 picolibc 减小体积
CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0
```

### 6.5 编译命令

```bash
# 在 zephyrproject 根目录执行

# 编译 MCUboot（板级配置自动加载 stm32f4_devebox.conf）
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr -d build_mcuboot -p

# 输出: build_mcuboot/zephyr/zephyr.bin（约 40KB）
```

**编译后大小验证**：

| 组件 | 分区大小 | 实际占用 | 使用率 |
|------|---------|---------|--------|
| MCUboot | 48KB | ~40KB | 83% |

---

## 7. App 编译配置

文件：`code/blinky_test/prj.conf`（MCUboot 相关部分）

```ini
# =============================================================================
# MCUboot 配置（App 端）
# =============================================================================

# 启动 MCUboot 支持
CONFIG_BOOTLOADER_MCUBOOT=y

# 签名密钥 — 使用 MCUboot 自带开发密钥（生产环境必须替换）
CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="bootloader/mcuboot/root-rsa-2048.pem"

# 【关键】必须与 MCUboot 的 BOOT_UPGRADE_ONLY=y 一致！
# 否则 flash_img 写入 slot1 的偏移与 MCUboot 读取偏移不匹配
CONFIG_MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY=y


# =============================================================================
# MCUmgr DFU 升级配置
# =============================================================================

# MCUmgr 依赖项（Kconfig 不会自动启用，需显式声明）
CONFIG_ZCBOR=y
CONFIG_NET_BUF=y
CONFIG_BASE64=y
CONFIG_MCUMGR=y
CONFIG_MCUMGR_GRP_IMG=y              # 镜像管理组
CONFIG_IMG_MANAGER=y                 # DFU 镜像管理器
CONFIG_MCUMGR_GRP_OS=y               # OS 管理组（远程复位）
CONFIG_STREAM_FLASH=y                # 流式 Flash 写入
CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD=y
CONFIG_MCUMGR_TRANSPORT_SHELL=y      # Shell 传输
CONFIG_MCUMGR_TRANSPORT_SHELL_MTU=512
CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=2048


# =============================================================================
# SPI / Flash 配置
# =============================================================================

CONFIG_SPI=y
CONFIG_FLASH=y
CONFIG_FLASH_PAGE_LAYOUT=y

# W25Q16 最小擦除单位 4KB — 必须与 MCUboot 保持一致
CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096
```

### 7.1 `CONFIG_BOOTLOADER_MCUBOOT=y` 的效果

启用后构建系统自动：

1. 设置 `USE_DT_CODE_PARTITION=y`
2. `FLASH_LOAD_OFFSET` = slot0 起始偏移（0xC000）
3. `ROM_START_OFFSET = 0x400`（MCUboot 镜像头大小）
4. 编译后自动 `imgtool sign` 签名
5. 生成 `zephyr.signed.bin`

### 7.2 编译优化

```ini
# -Og：调试与空间的甜点，GDB 基本可用，体积比 -O0 小 ~47%
CONFIG_DEBUG_OPTIMIZATIONS=y
```

| 优化等级 | FLASH 占用 | slot0 使用率 | 调试体验 |
|----------|-----------|-------------|----------|
| -O0 | ~235KB | 50.6% | 完美，但体积大 |
| **-Og** | **~122KB** | **26.3%** | 良好 |
| -O2 | ~110KB | 23.7% | 变量常 optimized out |
| -Os | ~100KB | 21.6% | 较差 |

### 7.3 编译命令

```bash
# 在 zephyrproject 根目录执行

# 编译 App（自动签名）
west build -b stm32f4_devebox code/blinky_test -p

# 输出:
#   build/zephyr/zephyr.bin           ← 未签名
#   build/zephyr/zephyr.signed.bin    ← 已签名（OTA 上传用这个）
#   build/zephyr/zephyr.signed.hex
#   build/zephyr/zephyr.elf
```

---

## 8. MCUmgr OTA 升级配置

### 8.1 各配置项说明

| 配置 | 作用 |
|------|------|
| `CONFIG_MCUMGR=y` | MCUmgr 核心 |
| `CONFIG_MCUMGR_GRP_IMG=y` | 镜像管理命令组（upload/state-read/state-write/erase） |
| `CONFIG_MCUMGR_GRP_OS=y` | OS 命令组（reset/echo） |
| `CONFIG_MCUMGR_TRANSPORT_SHELL=y` | 复用 Shell 串口通信（好处：一个 UART 同时做 Shell + DFU） |
| `CONFIG_MCUMGR_TRANSPORT_SHELL_MTU=512` | 增大 MTU 提升上传速度 |
| `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=2048` | DFU 场景需要较大缓冲区 |
| `CONFIG_STREAM_FLASH=y` | 流式写入 Flash（边收边写，不占用大块 RAM） |
| `CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD=y` | 允许直接上传到 slot1 |
| `CONFIG_IMG_MANAGER=y` | DFU 镜像管理器（`MCUMGR_GRP_IMG` 的依赖） |

### 8.2 上传 slot 选择机制

MCUmgr 上传时调用 `flash_img_init()` → `flash_img_init_id(UPLOAD_FLASH_AREA_ID)`。

`UPLOAD_FLASH_AREA_ID` 的定义逻辑（`zephyr/subsys/dfu/img_util/flash_img.c`）：

```c
// 如果 slot1_partition 存在 且 slot0_partition 是运行中的 App 分区
// → 上传到 slot1_partition
#if PARTITION_EXISTS(slot1_partition) && PARTITION_IS_RUNNING_APP_PARTITION(slot0_partition)
#define UPLOAD_FLASH_AREA_LABEL slot1_partition
#else
#define UPLOAD_FLASH_AREA_LABEL slot0_partition
#endif

#define UPLOAD_FLASH_AREA_ID PARTITION_ID(UPLOAD_FLASH_AREA_LABEL)
```

所以正常配置下（slot1 在 DTS 中定义），MCUmgr 自动将固件上传到**外部 W25Q16 上的 slot1_partition**。

### 8.3 smpmgr CLI 使用

```bash
# 安装
pip install smpmgr

# 查看镜像状态
smpmgr --port COM7 image state-read

# 上传新固件
smpmgr --port COM7 image upload ./zephyr.signed.bin

# 标记待升级（填入 slot1 的 hash）
smpmgr --port COM7 image state-write <hash>

# 确认当前固件（取消回滚标记）
smpmgr --port COM7 image confirm <hash>

# 重启设备进入升级
smpmgr --port COM7 reset
```

---

## 9. 跨设备 Flash 原理

### 9.1 flash_map 自动路由

这是 Zephyr flash_map 的核心能力——**同一个应用代码可以透明地操作不同物理 Flash 设备**。

```
flash_map 自动解析 DTS 分区 → flash_area 结构体:
  每个 flash_area 包含:
    - fa_dev    → 指向对应的 Flash 驱动设备
    - fa_off    → 分区在 Flash 上的起始偏移
    - fa_size   → 分区大小

  slot0 (内部 Flash):
    fa_dev  → SOC_FLASH_STM32 驱动（通过 &flash0 解析）

  slot1 (W25Q16):
    fa_dev  → SPI_NOR 驱动（通过 w25qxx@0 节点自动解析）

  MCUboot 调用 flash_area_read(slot1) 时:
    → fa_dev 自动路由到 SPI NOR 驱动 → 通过 SPI 读取 W25Q16
```

**DTS 解析链路**：

```
slot1_partition → fixed-partitions → w25qxx (jedec,spi-nor) → SPI NOR 驱动
                                                        ↑
                                            DT_MTD_FROM_PARTITION() 宏自动解析
```

### 9.2 Overwrite 模式升级路径（跨设备）

```
1. boot_read_image_headers()
   → flash_area_read(slot1) → SPI NOR 驱动 → 读取 W25Q16
   → 验证 slot1 的 magic=0x96f3b83d

2. bootutil_img_validate()
   → flash_area_read(slot1) → SPI NOR 驱动
   → 验证 SHA256 + RSA 签名

3. boot_copy_region()
   → read(slot1) + write(slot0) → 跨设备复制
     读: slot1 (W25Q16, SPI) → 写: slot0 (内部 Flash, 寄存器)

4. do_boot()
   → flash_device_base(slot0) → 内部 Flash 基址
   → 跳转到 slot0 运行新 App
```

### 9.3 为什么普通 SPI 也可以实现跨设备升级

以前很多资料说"外部 Flash 做 slot1 需要 QSPI Memory-Mapped 模式"，这个说法**在较老版本的 MCUboot/Zephyr 中成立**，但在当前版本（Zephyr 4.x + MCUboot 2.4）中，flash_map 的 `fa_dev` 自动路由机制已经完美支持普通 SPI Flash。

**关键条件**：
1. DTS 中正确配置 `slot1_partition` 在意外的 Flash 设备的子分区中
2. `CONFIG_FLASH_MAP=y`（App + MCUboot 都需要）
3. SPI 驱动在 MCUboot 启动早期完成初始化（Zephyr 的 `SYS_INIT` 机制自动处理）

**仍然存在的限制**：
- slot1 位于外部 Flash，MCUboot 只能使用 Overwrite 模式（Swap 模式的跨设备实现更复杂）
- 必须确保 MCUboot 启动时 SPI 外设已初始化

---

## 10. 编译、签名与烧录

### 10.1 编译顺序

```bash
# 1. 编译 MCUboot
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr -d build_mcuboot -p

# 2. 编译 App（自动签名）
west build -b stm32f4_devebox code/blinky_test -p
```

### 10.2 烧录

```bash
# 烧录 MCUboot 到 0x08000000
west flash --build-dir build_mcuboot

# 烧录 App 到 slot0 (0x0800C000) — 构建系统自动计算偏移
west flash
```

### 10.3 烧录地址速查

| 固件 | 烧录地址 | 文件 |
|------|---------|------|
| MCUboot | 0x08000000 | `build_mcuboot/zephyr/zephyr.bin`（未签名） |
| App (slot0) | 0x0800C000 | `build/zephyr/zephyr.signed.bin`（已签名） |

### 10.4 签名参数

```bash
# Zephyr 构建系统自动调用 imgtool，等效命令：
imgtool sign \
    --key bootloader/mcuboot/root-rsa-2048.pem \
    --header-size 0x400 \
    --align 1 \
    --version 0.0.0 \
    --slot-size 0x74000 \           # = 464KB
    --overwrite-only \              # 因为 MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY=y
    build/zephyr/zephyr.bin \
    build/zephyr/zephyr.signed.bin
```

---

## 11. OTA 升级流程

### 11.1 完整步骤

```
步骤 1: 编译新版本 App
  → west build -b stm32f4_devebox code/blinky_test

步骤 2: 通过 smpmgr 上传到 slot1（W25Q16）
  → smpmgr --port COM7 image upload ./zephyr.signed.bin
  → MCUmgr 通过 stream_flash 写入 W25Q16 offset 0x100000

步骤 3: 查看状态，获取 slot1 的 hash
  → smpmgr --port COM7 image state-read
  → 输出:
      slot=1 hash=AE0C8D426DC1F5213266576363FB9704...
               bootable=True pending=False

步骤 4: 标记 slot1 为待升级
  → smpmgr --port COM7 image state-write <hash>
  → slot1 pending 变为 True

步骤 5: 重启设备
  → smpmgr --port COM7 reset
  → 设备复位 → MCUboot 检测到 slot1 pending
  → 将 slot1 (W25Q16) 内容覆写到 slot0 (内部 Flash)
  → 跳转执行新 App
```

### 11.2 MCUboot 启机日志解读（升级成功时）

```
*** Booting MCUboot v2.4.0 ***
I: Starting bootloader
I: Image index: 0, Swap type: test          ← Overwrite 模式
D: boot_read_image_hdr: slot=1 magic=0x96f3b83d  ← slot1 头部有效！
D: bootutil_img_validate: 签名验证通过
D: Left boot_go with success == 1
I: Bootloader chainload address offset: 0xc000
I: Jumping to the first image slot
```

### 11.3 MCUboot 启机日志解读（升级失败，旧配置）

```
*** Booting MCUboot v2.4.0 ***
D: boot_read_image_hdr: slot=1 magic=0xffffffff  ← slot1 offset 0 全 0xFF
I: Image index: 0, Swap type: test              ← 没检测到升级
D: Left boot_go with success == 1                ← 直接启动旧 App
```

---

## 12. 踩坑记录与排查

### 坑 1：【致命】App 的 `MCUBOOT_BOOTLOADER_MODE` 与 MCUboot 不一致

**现象**：MCUmgr 上传成功，`slot1.hash` 显示正确，但重启后不升级。

**根因**：Zephyr Kconfig 默认 `MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET=y`，而 MCUboot 配置的是 `BOOT_UPGRADE_ONLY=y`（Overwrite）。

- SWAP_USING_OFFSET 模式下 `flash_img_init_id()` 从 slot1 的 **offset 0x1000** 开始写入
- MCUboot Overwrite 模式从 slot1 的 **offset 0** 开始读取
- 结果：MCUboot 在 offset 0 读到全 0xFF → 认为 slot1 为空 → 不升级

**排查方法**：App 中用 `flash_area_read(slot1, 0, buf, 32)` 读 slot1 offset 0，发现全 0xFF；但扫描到 0x10000 处发现有效数据。

**解决**：App 的 prj.conf 中添加：
```ini
CONFIG_MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY=y
```

### 坑 2：`CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE` 默认值 64KB

**现象**：MCUboot 的 `boot_slots_compatible()` 检查失败，无法升级。

**根因**：SPI NOR 驱动默认页大小为 65536（64KB），而 W25Q16 实际最小擦除单位是 4KB。64KB 无法整除内部 Flash 的 16KB 扇区。

**解决**：在 App 和 MCUboot 中都设置：
```ini
CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096
```

### 坑 3：`CONFIG_BOOT_MAX_IMG_SECTORS` 默认值不够

**现象**：MCUboot 编译或运行时报扇区数不足。

**根因**：slot1 在 W25Q16 上有 464KB/4KB = 116 个扇区，默认 128 个刚好可能不够（需额外扇区用于 trailer 等元数据）。

**解决**：
```ini
CONFIG_BOOT_MAX_IMG_SECTORS_AUTO=n
CONFIG_BOOT_MAX_IMG_SECTORS=256
```

### 坑 4：Kconfig 命名空间混淆

**现象**：在 App prj.conf 中写 `CONFIG_BOOT_UPGRADE_ONLY=y`，Kconfig 报错 "undefined symbol"。

**根因**：`CONFIG_BOOT_*` 属于 MCUboot 的 Kconfig，App 的 Kconfig 空间中不存在。

**解决**：MCUboot 的配置放在 `bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf` 中。

**Kconfig 归属速查**：
- `CONFIG_BOOT_*` → 属于 MCUboot
- `CONFIG_BOOTLOADER_MCUBOOT` / `CONFIG_MCUBOOT_*` → 属于 App
- `CONFIG_MCUMGR_*` / `CONFIG_IMG_MANAGER` / `CONFIG_STREAM_FLASH` → 属于 App

### 坑 5：DTS 改动后需要 pristine 构建

**现象**：修改 DTS 分区后编译报奇怪的错误。

**根因**：旧的 `.config` 缓存与新 DTS 不一致。

**解决**：`west build -p`（pristine 构建）或手动删除 build 目录。

### 坑 6：签名密钥路径问题

**现象**：编译后没有 `zephyr.signed.bin`。

**解决**：
1. 确认 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 已设置
2. 确认路径相对于 west 工作区根目录（即 `zephyrproject/`）
3. 手动签名：`west build -t sign_app`

### 坑 7：烧录未签名镜像

**现象**：MCUboot 报 `Image in the primary slot is not valid!`。

**根因**：烧录了 `zephyr.bin` 而不是 `zephyr.signed.bin`。

**解决**：始终烧录 `zephyr.signed.bin`。

### 坑 8：smpmgr 连接 timeout

**现象**：`smpmgr --port COM7 image state-read` 报 "connection timeout"。

**常见原因**：
1. COM 口被其他程序占用（如串口调试助手）
2. 设备刚复位，MCUmgr 尚未初始化完成（等几秒再试）
3. 波特率不匹配

---

## 13. 附录：快速参考

### 13.1 文件索引

| 文件 | 作用 |
|------|------|
| `code/blinky_test/prj.conf` | App 配置（MCUboot + MCUmgr + 外设） |
| `code/blinky_test/src/main.c` | App 入口代码 |
| `bootloader/mcuboot/boot/zephyr/prj.conf` | MCUboot 主配置 |
| `bootloader/mcuboot/boot/zephyr/boards/stm32f4_devebox.conf` | MCUboot 板级配置 |
| `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts` | 板级 DTS（分区定义） |
| `zephyr/modules/Kconfig.mcuboot` | App 端 MCUboot Kconfig 定义 |
| `zephyr/subsys/dfu/img_util/flash_img.c` | flash_img 实现（包含写入偏移逻辑） |

### 13.2 命令速查

```bash
# === 编译 ===
# 编译 MCUboot
west build -b stm32f4_devebox bootloader/mcuboot/boot/zephyr -d build_mcuboot -p

# 编译 App（自动签名）
west build -b stm32f4_devebox code/blinky_test -p

# 手动签名
west build -t sign_app

# === 烧录 ===
# 烧录 MCUboot
west flash --build-dir build_mcuboot

# 烧录 App
west flash

# === OTA 升级 ===
smpmgr --port COM7 image upload ./zephyr.signed.bin
smpmgr --port COM7 image state-read
smpmgr --port COM7 image state-write <hash>
smpmgr --port COM7 reset

# === 调试 ===
# 查看 App 实际使用的 .config
cat build/zephyr/.config | grep MCUBOOT_BOOTLOADER_MODE
```

### 13.3 关键配置对照表

| 配置项 | App 端 | MCUboot 端 | 说明 |
|--------|--------|-----------|------|
| 升级模式 | `CONFIG_MCUBOOT_BOOTLOADER_MODE_OVERWRITE_ONLY=y` | `CONFIG_BOOT_UPGRADE_ONLY=y` | **两端必须一致** |
| SPI NOR 页大小 | `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096` | `CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096` | **两端必须一致** |
| 签名密钥 | `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` | `CONFIG_BOOT_SIGNATURE_KEY_FILE` | 同一对密钥 |
| 最大扇区数 | — | `CONFIG_BOOT_MAX_IMG_SECTORS=256` | 仅 MCUboot |

### 13.4 启动日志关键判别

```
# 升级成功
magic=0x96f3b83d  ← slot1 头部有效
Left boot_go with success == 1
Jumping to the first image slot

# 升级失败（模式不匹配 / slot1 为空）
magic=0xffffffff  ← slot1 头部读到全 0xFF
Swap type: test    ← 没有执行升级
```
