# Zephyr SPI + W25Q16 + LittleFS 文件系统实战指南

> 基于 STM32F407VET6 (stm32f4_devebox) 平台的完整学习记录，从 SPI 配置、
> W25Q16 Flash 驱动、Flash 分区到 LittleFS 文件系统挂载与使用。
> 适用于未来移植板卡和快速参考。

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [SPI 子系统配置](#2-spi-子系统配置)
3. [W25Q16 SPI NOR Flash 驱动](#3-w25q16-spi-nor-flash-驱动)
4. [Flash 分区表](#4-flash-分区表)
5. [LittleFS 文件系统](#5-littlefs-文件系统)
6. [DTS fstab 自动挂载](#6-dts-fstab-自动挂载)
7. [Kconfig 配置汇总](#7-kconfig-配置汇总)
8. [文件系统 API 速查](#8-文件系统-api-速查)
9. [完整 DTS 配置参考](#9-完整-dts-配置参考)
10. [应用代码示例](#10-应用代码示例)
11. [踩坑记录](#11-踩坑记录)
12. [文件索引](#12-文件索引)

---

## 1. 整体架构概览

### 1.1 分层架构

从硬件到应用，整个存储栈的分层如下：

```
应用代码
  │
  ├── 直接操作 Flash：w25qxx_erase / w25qxx_write / w25qxx_read
  │     ↓
  │   Flash API (flash_erase / flash_write / flash_read)
  │     ↓
  │   jedec,spi-nor 驱动 (spi_nor.c)
  │     ↓
  │   SPI 控制器驱动 (stm32 SPI)
  │     ↓
  │   GPIO (CS 片选)
  │
  └── 通过文件系统：fs_storage_write_file / fs_storage_read_file
        ↓
      VFS 统一 API (fs_open / fs_write / fs_read / fs_mount)
        ↓
      LittleFS 驱动 (littlefs_fs.c)
        ↓
      Flash Map API (flash_area_read / flash_area_write / flash_area_erase)
        ↓
      jedec,spi-nor 驱动 → SPI 控制器 → GPIO
```

### 1.2 两种使用方式

| 方式 | API | 适用场景 | 特点 |
|------|-----|---------|------|
| Flash API | `flash_erase` / `flash_write` / `flash_read` | 裸数据存储、Bootloader | 需手动管理擦除/写入/对齐 |
| 文件系统 | `fs_open` / `fs_write` / `fs_read` | 参数保存、日志记录 | 自动管理擦除、磨损均衡、掉电安全 |

### 1.3 关键概念映射

| 概念 | 说明 |
|------|------|
| SPI 控制器 | STM32 内置 SPI 外设，负责串行数据收发 |
| jedec,spi-nor | Zephyr 通用 SPI NOR Flash 驱动，通过 JEDEC ID 识别芯片 |
| Flash Map | 将 Flash 分区抽象为 `flash_area`，提供分区级读写接口 |
| fixed-partitions | DTS 中的分区表定义，将 Flash 划分为多个逻辑区域 |
| LittleFS | 专为嵌入式设计的文件系统，掉电安全 + 磨损均衡 |
| fstab | DTS 中的文件系统挂载表，声明挂载参数，构建系统自动生成挂载结构体 |

---

## 2. SPI 子系统配置

### 2.1 SPI 基础概念

SPI（Serial Peripheral Interface）是一种同步串行通信协议：

- **四线制**：SCK（时钟）、MOSI（主出从入）、MISO（主入从出）、CS（片选）
- **全双工**：数据同时收发
- **一主多从**：通过 CS 选择不同从设备
- **CPOL/CPHA**：时钟极性和相位决定四种工作模式，W25Q16 使用 Mode 0（CPOL=0, CPHA=0）

### 2.2 DTS 中 SPI 控制器配置

```dts
&spi1 {
    status = "okay";
    /* 引脚复用：使用第二组引脚（PA5/6/7 被 LED 占用） */
    pinctrl-0 = <&spi1_sck_pb3 &spi1_miso_pb4 &spi1_mosi_pb5>;
    pinctrl-names = "default";
    /* SPI1 外设时钟频率（APB2 经分频后的输入时钟） */
    clock-frequency = <16000000>;
    /* CS 片选 GPIO 列表 */
    cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>;
};
```

### 2.3 cs-gpios 详解

**为什么 cs-gpios 在 SPI 控制器节点而不是子设备节点？**

因为 CS 线是 SPI 控制器（主机）的资源。SPI 总线上 SCK/MISO/MOSI 共享，只有 CS 是每个从设备独占的。控制器需要知道它手上有哪些 CS GPIO 可用，才能在通信时正确选中目标设备。

**`reg` 与 `cs-gpios` 的映射关系**：

```dts
cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>,   /* 索引 0 → PB0 */
           <&gpioa 15 GPIO_ACTIVE_LOW>;   /* 索引 1 → PA15 */

device@0 { reg = <0>; };  /* reg=0 → 使用 cs-gpios[0] = PB0 */
device@1 { reg = <1>; };  /* reg=1 → 使用 cs-gpios[1] = PA15 */
```

**重要**：CS 必须连接到 GPIO，不能直接接 GND 或 VCC。`jedec,spi-nor` 驱动需要在每次 SPI 事务之间拉高 CS 来分隔命令帧，CS 始终为低会导致通信失败。

### 2.4 STM32F407 SPI1 可用引脚

| 功能 | 第一组引脚 | 第二组引脚 | 本项目使用 |
|------|-----------|-----------|-----------|
| SCK  | PA5 (AF5) | PB3 (AF5) | PB3 |
| MISO | PA6 (AF5) | PB4 (AF5) | PB4 |
| MOSI | PA7 (AF5) | PB5 (AF5) | PB5 |
| NSS  | PA4 (AF5) | PA15 (AF5) | — |

### 2.5 Kconfig

```ini
CONFIG_SPI=y    # 启用 SPI 子系统
```

---

## 3. W25Q16 SPI NOR Flash 驱动

### 3.1 Zephyr 的 SPI NOR 驱动机制

Zephyr **没有专门的 W25Q16 驱动**，但有通用的 `jedec,spi-nor` 驱动，它通过 JEDEC ID 识别芯片，支持所有符合 JEDEC 标准的 SPI NOR Flash。

**驱动源码**：`zephyr/drivers/flash/spi_nor.c`
**兼容字符串**：`"jedec,spi-nor"`
**Kconfig**：`CONFIG_SPI_NOR`（当 DTS 中存在 `jedec,spi-nor` 节点时自动启用）

### 3.2 W25Q16 关键参数

| 参数 | 值 | 说明 |
|------|---|------|
| 容量 | 16 Mbit = 2 MB | W25Q16 的 "16" 指 16 Mbit |
| 扇区大小 | 4 KB | 擦除的最小单位 |
| 页大小 | 256 字节 | 单次写入的最大对齐单位 |
| 地址空间 | 0x000000 ~ 0x1FFFFF | 24 位地址 |
| JEDEC ID | `[ef 40 15]` | ef=Winbond, 40=类型, 15=16Mbit |
| SPI 模式 | Mode 0 (CPOL=0, CPHA=0) | 标准读最高 104 MHz |

### 3.3 DTS 中 W25Q16 配置

```dts
w25qxx: w25qxx@0 {
    compatible = "jedec,spi-nor";
    reg = <0>;                         /* 对应 cs-gpios[0] */
    spi-max-frequency = <80000000>;    /* 80 MHz，W25Q16 最高 104 MHz */
    label = "W25Q16";
    jedec-id = [ef 40 15];            /* Winbond W25Q16 的 JEDEC ID */
    size = <16777216>;                 /* 16 Mbit，单位为 bit */
};
```

**属性说明**：

| 属性 | 说明 |
|------|------|
| `compatible` | 必须为 `"jedec,spi-nor"`，匹配通用驱动 |
| `reg` | SPI 设备索引，对应 cs-gpios 数组的下标 |
| `spi-max-frequency` | SPI 最大时钟频率，不能超过芯片规格 |
| `jedec-id` | 3 字节 JEDEC ID，驱动初始化时与芯片实际 ID 比对校验 |
| `size` | Flash 容量，**单位为 bit**，16 Mbit = 16777216 |

### 3.4 Flash API 使用

Zephyr 的 Flash API（`#include <zephyr/drivers/flash.h>`）提供三个核心操作：

#### 擦除 — flash_erase()

```c
int flash_erase(const struct device *dev, off_t offset, size_t size);
```

- `offset` 必须扇区对齐（4KB 对齐）
- `size` 必须为扇区大小的整数倍
- 擦除后所有字节变为 0xFF
- **写入前必须先擦除**

W25Q16 支持的擦除粒度：

| 擦除类型 | 大小 | 命令 | 耗时 |
|---------|------|------|------|
| 扇区擦除 | 4 KB | 0x20 | ~100ms |
| 块擦除 | 32 KB | 0x52 | ~300ms |
| 块擦除 | 64 KB | 0xD8 | ~600ms |
| 整片擦除 | 2 MB | 0xC7 | ~15s |

Zephyr 驱动根据 `size` 自动选择合适的擦除命令。

#### 写入 — flash_write()

```c
int flash_write(const struct device *dev, off_t offset, const void *data, size_t len);
```

- 目标区域必须已擦除（0xFF）
- Flash 只能把 1 写成 0，不能把 0 写成 1
- **单次写入不能跨页边界**（每页 256 字节）
- 驱动自动发送 WREN（写使能）命令

#### 读取 — flash_read()

```c
int flash_read(const struct device *dev, off_t offset, void *data, size_t len);
```

- 无对齐限制，任意偏移、任意长度均可
- 驱动内部发送命令 0x03（标准读）+ 24 位地址

### 3.5 设备获取方式

```c
/* 方式一：通过 DTS 节点标签（推荐，精确指定） */
#define W25QXX_NODE DT_NODELABEL(w25qxx)
const struct device *dev = DEVICE_DT_GET(W25QXX_NODE);

/* 方式二：通过 compatible 匹配（不推荐，多设备时只返回第一个） */
const struct device *dev = DEVICE_DT_GET_ONE(jedec_spi_nor);
```

**`DT_NODELABEL` vs `DT_COMPAT_GET_ANY_STATUS_OKAY`**：
- `DT_NODELABEL(w25qxx)` → 通过标签精确指定，多设备不受影响
- `DT_COMPAT_GET_ANY_STATUS_OKAY(jedec_spi_nor)` → 只返回第一个匹配节点

### 3.6 初始化与就绪检查

```c
const struct device *dev = DEVICE_DT_GET(W25QXX_NODE);

/* DEVICE_DT_GET：编译时获取设备指针，不检查是否已初始化 */
/* device_is_ready：运行时检查设备是否就绪 */
if (!device_is_ready(dev)) {
    /* 可能原因：CS 未连接、SPI 配置错误、JEDEC ID 不匹配 */
    return -ENODEV;
}
```

---

## 4. Flash 分区表

### 4.1 为什么需要分区

Flash 分区将一块 Flash 划分为多个逻辑区域，每个区域可独立使用：

- **LittleFS 文件系统**需要一个分区作为存储后端
- **参数存储**可以用裸 Flash API 操作另一个分区
- **OTA 升级**需要独立的分区存放固件镜像
- 分区之间互不干扰，擦除一个分区不影响其他分区

### 4.2 DTS 分区定义

在 `jedec,spi-nor` 节点内添加 `partitions` 子节点：

```dts
w25qxx: w25qxx@0 {
    compatible = "jedec,spi-nor";
    /* ... 其他属性 ... */

    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        lfs_partition: partition@0 {
            label = "lfs";
            reg = <0x00000000 0x00100000>;  /* 前 1MB */
        };

        storage_partition: partition@100000 {
            label = "storage";
            reg = <0x00100000 0x00100000>;  /* 后 1MB */
        };
    };
};
```

**分区属性说明**：

| 属性 | 说明 |
|------|------|
| `compatible = "fixed-partitions"` | 固定分区表，必须写 |
| `#address-cells = <1>` | 地址用 1 个 cell（32 位）表示 |
| `#size-cells = <1>` | 大小用 1 个 cell（32 位）表示 |
| `partition@OFFSET` | 节点名中的 OFFSET 是分区的起始偏移（十六进制） |
| `label` | 分区的文本标签，供日志和调试使用 |
| `reg = <OFFSET SIZE>` | 分区的起始偏移和大小（字节），**不是 bit** |

### 4.3 分区规划示例

W25Q16 (2MB) 的常见分区规划：

```
0x000000 ┌──────────────────────┐
         │                      │
         │  lfs_partition       │  1 MB  → LittleFS 文件系统
         │  (参数/配置/小文件)   │
         │                      │
0x100000 ├──────────────────────┤
         │                      │
         │  storage_partition   │  1 MB  → 预留（裸 Flash / 日志 / OTA）
         │                      │
0x1FFFFF └──────────────────────┘
```

### 4.4 Flash Map API

分区定义后，通过 Flash Map API 访问分区：

```c
#include <zephyr/storage/flash_map.h>

/* 通过分区标签获取 flash_area */
const struct flash_area *fa;
flash_area_open(FIXED_PARTITION_ID(lfs_partition), &fa);

/* 分区操作 */
flash_area_read(fa, offset, buf, len);
flash_area_write(fa, offset, data, len);
flash_area_erase(fa, offset, size);
flash_area_close(fa);
```

通常不需要直接使用 Flash Map API，LittleFS 驱动内部会自动调用。

---

## 5. LittleFS 文件系统

### 5.1 为什么选择 LittleFS

| 特性 | LittleFS | FatFS | 裸 Flash |
|------|----------|-------|---------|
| 掉电安全 | 是 | 否 | 否 |
| 磨损均衡 | 是 | 否 | 否 |
| RAM 占用 | 极小 (~160B) | 中等 | 无 |
| 目录支持 | 是 | 是 | 否 |
| POSIX 兼容 | 部分 | 否 | 否 |
| 适合场景 | MCU 参数/日志 | SD 卡 | Bootloader |

**掉电安全**：写入过程中断电，文件系统不会损坏，最多丢失最后一次操作。
**磨损均衡**：自动分散写入位置，避免同一块 Flash 被反复擦写导致寿命耗尽。

### 5.2 Zephyr 文件系统架构

```
应用代码
  │
  ▼
VFS 统一 API (fs.h)                    ← fs_open / fs_read / fs_write / fs_mount
  │  根据路径前缀路由到对应文件系统
  ▼
fs_file_system_t 函数指针表             ← 接口层，每种文件系统实现一组操作
  │
  ├── LittleFS (littlefs_fs.c)          ← 掉电安全 + 磨损均衡，MCU 首选
  ├── FatFS (fat_fs.c)                  ← SD 卡 / Windows 兼容
  └── ext2                              ← Linux 兼容
  │
  ▼
Flash Map / Disk Access                 ← 存储抽象层
  │  flash_area_read/write/erase        ← SPI NOR Flash
  │  disk_access_read/write/ioctl       ← SD 卡
  ▼
硬件驱动 (jedec,spi-nor / SD 卡驱动)
```

**VFS 核心机制**：
- **文件系统注册表**：通过 `fs_register()` 注册文件系统类型（如 `FS_LITTLEFS`）
- **挂载点链表**：所有已挂载的文件系统通过双向链表管理
- **路径路由**：`fs_open("/lfs1/config.txt")` 根据前缀 `/lfs1` 找到对应的 LittleFS 挂载点

### 5.3 LittleFS 参数说明

| 参数 | 说明 | 推荐值 | 影响 |
|------|------|-------|------|
| `read-size` | 最小读取粒度 | 16 | 越大读取越快，但浪费带宽 |
| `prog-size` | 最小编程粒度 | 16 | 越大写入越快，但浪费空间 |
| `cache-size` | 读/写缓存大小 | 64 | 越大性能越好，RAM 占用越多 |
| `lookahead-size` | 前瞻缓冲区大小 | 32 (须为 8 的倍数) | 影响空闲块查找效率 |
| `block-cycles` | 磨损均衡擦除周期 | 512 | 0=禁用磨损均衡，越大写入越快 |

**RAM 占用估算**：`cache-size × 2 + lookahead-size`

---

## 6. DTS fstab 自动挂载

### 6.1 fstab 机制

fstab（File System Table）是 Zephyr 4.x 推荐的文件系统挂载方式，在 DTS 中声明挂载参数，构建系统自动生成 `fs_mount_t` 结构体。

**fstab vs 手动挂载**：

| 对比项 | fstab 方式 | 手动方式 |
|--------|-----------|---------|
| 声明位置 | DTS | C 代码 |
| 结构体生成 | 自动 | 手动声明 `fs_mount_t` |
| 挂载方式 | automount 自动 | 手动调用 `fs_mount()` |
| 参数修改 | 改 DTS | 改 C 代码重新编译 |

### 6.2 DTS fstab 配置

```dts
/ {
    fstab {
        compatible = "zephyr,fstab";

        lfs1: lfs1 {
            compatible = "zephyr,fstab,littlefs";
            mount-point = "/lfs1";              /* 挂载点路径 */
            partition = <&lfs_partition>;        /* 引用分区 */
            automount;                           /* 启动时自动挂载 */
            read-size = <16>;
            prog-size = <16>;
            cache-size = <64>;
            lookahead-size = <32>;
            block-cycles = <512>;
        };
    };
};
```

**属性说明**：

| 属性 | 说明 |
|------|------|
| `compatible = "zephyr,fstab,littlefs"` | 必须写，标识 LittleFS 挂载 |
| `mount-point` | 挂载点路径，文件操作时作为路径前缀 |
| `partition` | phandle 引用，指向 `fixed-partitions` 中定义的分区 |
| `automount` | 布尔属性，存在即启用自动挂载 |
| `no-format` | 可选，挂载失败时不格式化（默认会自动格式化） |
| `read-only` | 可选，只读挂载 |

### 6.3 C 代码中引用 fstab

```c
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

/* 声明 fstab 自动生成的 fs_mount_t 结构体 */
#define LFS_PARTITION_NODE DT_NODELABEL(lfs1)
FS_FSTAB_DECLARE_ENTRY(LFS_PARTITION_NODE);

/* 获取挂载点指针 */
struct fs_mount_t *mp = &FS_FSTAB_ENTRY(LFS_PARTITION_NODE);
```

**宏展开过程**：

```
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(lfs1))
  → extern struct fs_mount_t z_fsmp_lfs1;

FS_FSTAB_ENTRY(DT_NODELABEL(lfs1))
  → z_fsmp_lfs1
```

`z_fsmp_lfs1` 由 `littlefs_fs.c` 中的 `DEFINE_FS` 宏自动生成，包含 `.type`、`.mnt_point`、`.fs_data`、`.storage_dev`、`.flags` 等字段。

### 6.4 自动挂载流程

当 DTS 中设置了 `automount` 且启用了 `CONFIG_FS_LITTLEFS_FSTAB_AUTOMOUNT`：

1. **SYS_INIT 阶段**（`main()` 之前）：LittleFS 驱动自动调用 `fs_mount()`
2. 首次挂载时，如果分区未格式化，自动调用 `lfs_format()` 格式化后重新挂载
3. 应用代码中 `fs_statvfs("/lfs1", &stat)` 返回 0 即表示已挂载成功

### 6.5 手动挂载（不使用 automount）

```c
struct fs_mount_t *mp = &FS_FSTAB_ENTRY(LFS_PARTITION_NODE);
int rc = fs_mount(mp);
if (rc != 0) {
    /* 挂载失败 */
}
```

---

## 7. Kconfig 配置汇总

### 7.1 SPI + Flash 相关

```ini
# SPI 子系统
CONFIG_SPI=y

# Flash 子系统
CONFIG_FLASH=y

# Flash 页面布局信息（flash_get_page_info_by_offs 等 API 依赖）
CONFIG_FLASH_PAGE_LAYOUT=y

# SPI NOR 驱动（DTS 中有 jedec,spi-nor 节点时自动启用，无需手动写）
# CONFIG_SPI_NOR=y
```

### 7.2 文件系统相关

```ini
# 文件系统子系统（VFS 核心层）
CONFIG_FILE_SYSTEM=y

# LittleFS 文件系统
CONFIG_FILE_SYSTEM_LITTLEFS=y

# Flash Map API（LittleFS 通过 flash_area 读写底层 Flash 分区）
CONFIG_FLASH_MAP=y

# fstab 自动挂载支持
CONFIG_FS_LITTLEFS_FSTAB_AUTOMOUNT=y
```

### 7.3 可选配置

```ini
# LittleFS 调试日志
# CONFIG_FS_LITTLEFS_LOG_LEVEL_DBG=y

# 最大同时打开文件数（默认 4）
# CONFIG_FS_LITTLEFS_NUM_FILES=4

# 最大同时打开目录数（默认 4）
# CONFIG_FS_LITTLEFS_NUM_DIRS=4

# 文件系统 Shell 命令（可通过串口 ls/cat/rm 等操作文件）
# CONFIG_FILE_SYSTEM_SHELL=y
```

### 7.4 依赖关系图

```
CONFIG_FILE_SYSTEM=y
  └── CONFIG_FILE_SYSTEM_LITTLEFS=y
        ├── 依赖 CONFIG_FILE_SYSTEM
        ├── 依赖 LittleFS 外部模块 (ZEPHYR_LITTLEFS_MODULE)
        └── 依赖 CONFIG_FLASH_MAP=y
              └── 依赖 CONFIG_FLASH=y
                    └── 依赖 CONFIG_SPI=y (SPI NOR 场景)
```

---

## 8. 文件系统 API 速查

### 8.1 文件操作

```c
#include <zephyr/fs/fs.h>

/* 初始化文件对象（使用前必须调用） */
struct fs_file_t file;
fs_file_t_init(&file);

/* 打开文件 */
int fs_open(struct fs_file_t *zfp, const char *file_name, fs_mode_t flags);

/* 读取文件，返回实际读取字节数，0=EOF，负数=错误 */
ssize_t fs_read(struct fs_file_t *zfp, void *ptr, size_t size);

/* 写入文件，返回实际写入字节数，负数=错误 */
ssize_t fs_write(struct fs_file_t *zfp, const void *ptr, size_t size);

/* 移动文件位置，whence: FS_SEEK_SET / FS_SEEK_CUR / FS_SEEK_END */
int fs_seek(struct fs_file_t *zfp, off_t offset, int whence);

/* 获取当前文件位置 */
off_t fs_tell(struct fs_file_t *zfp);

/* 截断文件到指定长度 */
int fs_truncate(struct fs_file_t *zfp, off_t length);

/* 刷新缓存到存储 */
int fs_sync(struct fs_file_t *zfp);

/* 关闭文件 */
int fs_close(struct fs_file_t *zfp);
```

### 8.2 文件打开标志

| 标志 | 值 | 说明 |
|------|---|------|
| `FS_O_READ` | 0x01 | 只读 |
| `FS_O_WRITE` | 0x02 | 只写 |
| `FS_O_RDWR` | 0x03 | 读写 |
| `FS_O_CREATE` | 0x10 | 不存在则创建 |
| `FS_O_APPEND` | 0x20 | 追加模式 |
| `FS_O_TRUNC` | 0x40 | 截断文件 |

**常用组合**：

| 场景 | 标志 |
|------|------|
| 创建/覆盖写入 | `FS_O_CREATE \| FS_O_WRITE \| FS_O_TRUNC` |
| 读取已有文件 | `FS_O_READ` |
| 追加写入 | `FS_O_CREATE \| FS_O_WRITE \| FS_O_APPEND` |
| 读写已有文件 | `FS_O_RDWR` |

### 8.3 目录操作

```c
/* 创建目录 */
int fs_mkdir(const char *path);

/* 打开目录 */
struct fs_dir_t dir;
fs_dir_t_init(&dir);
int fs_opendir(struct fs_dir_t *zdp, const char *path);

/* 读取目录项，entry.name[0]=='\0' 表示目录末尾 */
struct fs_dirent entry;
int fs_readdir(struct fs_dir_t *zdp, struct fs_dirent *entry);

/* 关闭目录 */
int fs_closedir(struct fs_dir_t *zdp);
```

### 8.4 文件系统操作

```c
/* 获取文件/目录信息 */
struct fs_dirent entry;
int fs_stat(const char *path, struct fs_dirent *entry);
/* entry.type: FS_DIR_ENTRY_FILE 或 FS_DIR_ENTRY_DIR */
/* entry.size: 文件大小（字节） */
/* entry.name: 文件名 */

/* 获取卷统计信息 */
struct fs_statvfs stat;
int fs_statvfs(const char *path, struct fs_statvfs *stat);
/* stat.f_bsize: 块大小 */
/* stat.f_blocks: 总块数 */
/* stat.f_bfree: 空闲块数 */
/* 总容量 = f_bsize * f_blocks, 空闲容量 = f_bsize * f_bfree */

/* 删除文件/空目录 */
int fs_unlink(const char *path);

/* 重命名 */
int fs_rename(const char *from, const char *to);

/* 挂载文件系统 */
int fs_mount(struct fs_mount_t *mp);

/* 卸载文件系统 */
int fs_unmount(struct fs_mount_t *mp);
```

### 8.5 典型使用模式

#### 写入参数文件

```c
struct fs_file_t file;
fs_file_t_init(&file);

/* 创建/覆盖写入 */
int rc = fs_open(&file, "/lfs1/config.txt",
                 FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
if (rc == 0) {
    fs_write(&file, "addr=1\n", 7);
    fs_write(&file, "baud=115200\n", 12);
    fs_close(&file);
}
```

#### 读取参数文件

```c
struct fs_file_t file;
fs_file_t_init(&file);

int rc = fs_open(&file, "/lfs1/config.txt", FS_O_READ);
if (rc == 0) {
    char buf[128];
    ssize_t bytes = fs_read(&file, buf, sizeof(buf) - 1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        /* 解析 buf 中的参数 */
    }
    fs_close(&file);
}
```

#### 追加日志

```c
struct fs_file_t file;
fs_file_t_init(&file);

int rc = fs_open(&file, "/lfs1/log.txt",
                 FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
if (rc == 0) {
    char log_line[64];
    int len = snprintf(log_line, sizeof(log_line),
                       "[%u] temp=%d\n", (unsigned)k_uptime_get_32(), temp);
    fs_write(&file, log_line, len);
    fs_close(&file);
}
```

#### 遍历目录

```c
struct fs_dir_t dir;
struct fs_dirent entry;

fs_dir_t_init(&dir);
if (fs_opendir(&dir, "/lfs1") == 0) {
    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type == FS_DIR_ENTRY_FILE) {
            printk("FILE: %s (%zu bytes)\n", entry.name, (size_t)entry.size);
        } else {
            printk("DIR:  %s\n", entry.name);
        }
    }
    fs_closedir(&dir);
}
```

---

## 9. 完整 DTS 配置参考

以下为本项目的完整 SPI + W25Q16 + 分区 + fstab 配置，可直接作为移植参考：

```dts
&spi1 {
    status = "okay";
    pinctrl-0 = <&spi1_sck_pb3 &spi1_miso_pb4 &spi1_mosi_pb5>;
    pinctrl-names = "default";
    clock-frequency = <16000000>;
    cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>;

    w25qxx: w25qxx@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <80000000>;
        label = "W25Q16";
        jedec-id = [ef 40 15];
        size = <16777216>;

        partitions {
            compatible = "fixed-partitions";
            #address-cells = <1>;
            #size-cells = <1>;

            lfs_partition: partition@0 {
                label = "lfs";
                reg = <0x00000000 0x00100000>;
            };

            storage_partition: partition@100000 {
                label = "storage";
                reg = <0x00100000 0x00100000>;
            };
        };
    };
};

/ {
    fstab {
        compatible = "zephyr,fstab";

        lfs1: lfs1 {
            compatible = "zephyr,fstab,littlefs";
            mount-point = "/lfs1";
            partition = <&lfs_partition>;
            automount;
            read-size = <16>;
            prog-size = <16>;
            cache-size = <64>;
            lookahead-size = <32>;
            block-cycles = <512>;
        };
    };
};
```

### 移植到其他芯片/Flash 的修改点

| 修改项 | 说明 |
|--------|------|
| `&spi1` | 改为实际使用的 SPI 端口（spi2/spi3 等） |
| `pinctrl-0` | 改为目标芯片的 SPI 引脚配置 |
| `cs-gpios` | 改为实际的 CS GPIO |
| `jedec-id` | 改为目标 Flash 的 JEDEC ID |
| `size` | 改为目标 Flash 的容量（bit） |
| `spi-max-frequency` | 改为目标 Flash 的最大频率 |
| 分区 `reg` | 根据实际容量重新规划 |
| LittleFS 参数 | 根据实际 RAM 和性能需求调整 |

### 常见 SPI NOR Flash 的 JEDEC ID

| 芯片 | 容量 | JEDEC ID | size (bit) |
|------|------|----------|-----------|
| W25Q16 | 16 Mbit / 2 MB | `[ef 40 15]` | 16777216 |
| W25Q32 | 32 Mbit / 4 MB | `[ef 40 16]` | 33554432 |
| W25Q64 | 64 Mbit / 8 MB | `[ef 40 17]` | 67108864 |
| W25Q128 | 128 Mbit / 16 MB | `[ef 40 18]` | 134217728 |
| MX25R6435F | 64 Mbit / 8 MB | `[c2 28 17]` | 67108864 |
| SST26VF016B | 16 Mbit / 2 MB | `[bf 26 01]` | 16777216 |

---

## 10. 应用代码示例

### 10.1 W25Q16 直接操作（Flash API）

```c
#include "w25qxx.h"

/* 初始化 */
w25qxx_init();

/* 擦除 → 写入 → 读取 */
w25qxx_erase(0x1FF000, 4096);                          /* 擦除最后一个扇区 */
w25qxx_write(0x1FF000, (const uint8_t *)"Hello", 5);   /* 写入 5 字节 */
uint8_t buf[5];
w25qxx_read(0x1FF000, buf, 5);                         /* 读回 5 字节 */
```

### 10.2 文件系统操作（VFS API）

```c
#include "fs_storage.h"

/* 初始化（检查 automount 是否成功） */
fs_storage_init();

/* 写入参数文件 */
const char *param = "addr=1\nbaud=115200\nperiod=1000\n";
fs_storage_write_file("param.txt", (const uint8_t *)param, strlen(param));

/* 读取参数文件 */
uint8_t buf[128];
size_t len;
fs_storage_read_file("param.txt", buf, sizeof(buf) - 1, &len);
buf[len] = '\0';
```

### 10.3 数据流完整路径

以 `fs_storage_write_file("param.txt", data, len)` 为例：

```
fs_storage_write_file()
  → fs_open("/lfs1/param.txt", FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC)    [VFS 层]
    → VFS 根据路径前缀 "/lfs1" 找到 LittleFS 挂载点
    → littlefs_open() → lfs_file_open()                                    [LittleFS 层]
  → fs_write(&file, data, len)
    → littlefs_write() → lfs_file_write()                                  [LittleFS 层]
      → LittleFS 内部管理块分配、磨损均衡
      → lfs_api_prog() → flash_area_write()                                [Flash Map 层]
        → spi_nor_write() → 组装 SPI 命令帧 (WREN + PP + 地址 + 数据)      [SPI NOR 驱动]
          → SPI 控制器发送数据 → GPIO 控制 CS                               [硬件层]
  → fs_close(&file)
    → littlefs_close() → lfs_file_close()                                  [刷新缓存到 Flash]
```

---

## 11. 踩坑记录

### 11.1 spi-max-frequency 写错

**问题**：`spi-max-frequency = <800000000>` (800 MHz)，远超 W25Q16 的 104 MHz 规格。

**修复**：改为 `<80000000>` (80 MHz)。

**教训**：DTS 中的数值没有编译时范围检查，写错不会报错但运行时可能不稳定。务必对照数据手册确认最大频率。

### 11.2 CS 引脚未配置

**问题**：最初未在 DTS 中配置 `cs-gpios`，导致 SPI 通信失败。

**原因**：STM32 的 SPI 控制器不自动管理 CS（硬件 NSS 模式在多设备场景下不适用），需要通过 GPIO 软件 CS。

**修复**：添加 `cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>`。

**重要**：CS 不能直接接 GND（始终选中）。`jedec,spi-nor` 驱动需要在每次 SPI 事务之间拉高 CS 来分隔命令帧，CS 始终为低会导致命令帧粘连，通信失败。

### 11.3 FS_O_WRONLY 不存在

**问题**：编译报错 `'FS_O_WRONLY' undeclared`。

**原因**：Zephyr VFS 的文件打开标志命名与 POSIX 不同，只有写权限的标志叫 `FS_O_WRITE` 而非 `FS_O_WRONLY`。

**修复**：将 `FS_O_WRONLY` 改为 `FS_O_WRITE`。

**Zephyr VFS 标志 vs POSIX 标志**：

| Zephyr | POSIX | 说明 |
|--------|-------|------|
| `FS_O_READ` | `O_RDONLY` | 只读 |
| `FS_O_WRITE` | `O_WRONLY` | 只写 |
| `FS_O_RDWR` | `O_RDWR` | 读写 |
| `FS_O_CREATE` | `O_CREAT` | 创建 |
| `FS_O_APPEND` | `O_APPEND` | 追加 |
| `FS_O_TRUNC` | `O_TRUNC` | 截断 |

### 11.4 size 单位是 bit 不是 byte

**问题**：`size = <16777216>` 看起来是 16 MB，但 W25Q16 只有 2 MB。

**原因**：DTS 中 `size` 属性的单位是 **bit**，不是 byte。16777216 bit = 16 Mbit = 2 MB。

**注意**：这与分区 `reg = <OFFSET SIZE>` 不同，分区的 SIZE 单位是 **byte**。

### 11.5 DT_COMPAT_GET_ANY_STATUS_OKAY 只取第一个节点

**问题**：当 DTS 中有多个 `jedec,spi-nor` 节点时，`DT_COMPAT_GET_ANY_STATUS_OKAY` 只返回第一个。

**修复**：使用 `DT_NODELABEL(w25qxx)` 通过标签精确指定节点。

---

## 12. 文件索引

| 文件 | 说明 |
|------|------|
| `src/w25qxx.h` | W25Q16 Flash 驱动接口（擦除/写入/读取/测试） |
| `src/w25qxx.c` | W25Q16 Flash 驱动实现（基于 jedec,spi-nor + Flash API） |
| `src/fs_storage.h` | 文件系统存储接口（写入/读取/测试） |
| `src/fs_storage.c` | 文件系统存储实现（基于 VFS + LittleFS + fstab） |
| `prj.conf` | Kconfig 配置（SPI/Flash/文件系统） |
| `CMakeLists.txt` | 构建配置 |
| 板级 DTS | SPI1 + W25Q16 + 分区 + fstab 配置 |

### Zephyr 源码关键文件

| 文件 | 说明 |
|------|------|
| `zephyr/drivers/flash/spi_nor.c` | jedec,spi-nor 通用驱动实现 |
| `zephyr/subsys/fs/fs.c` | VFS 核心层实现 |
| `zephyr/subsys/fs/littlefs_fs.c` | LittleFS 适配层 + fstab 自动生成 |
| `zephyr/include/zephyr/fs/fs.h` | VFS 统一 API 声明 |
| `zephyr/include/zephyr/fs/littlefs.h` | LittleFS 相关宏和结构体 |
| `zephyr/include/zephyr/drivers/flash.h` | Flash API 声明 |
| `zephyr/include/zephyr/storage/flash_map.h` | Flash Map API 声明 |
| `zephyr/dts/bindings/mtd/jedec,spi-nor.yaml` | SPI NOR DTS 绑定 |
| `zephyr/dts/bindings/fs/zephyr,fstab,littlefs.yaml` | LittleFS fstab DTS 绑定 |
| `zephyr/samples/subsys/fs/littlefs/` | LittleFS 官方示例 |
