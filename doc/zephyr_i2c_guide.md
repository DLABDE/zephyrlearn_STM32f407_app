# Zephyr I2C 子系统教程 — 基于 STM32F407VET6

> **验证状态**: 本文档所有配置已在 STM32F407VET6 DevEBox 开发板上验证通过。
> **环境**: Zephyr 4.4.1-rc1
> **芯片**: STM32F407VET6 (I2C1, PB8-SCL / PB9-SDA)
> **项目路径**: `code/blinky_test`
> **驱动源码**: `code/blinky_test/src/iic_board.c` / `iic_board.h`
> **板级 DTS**: `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts`

---

## 目录

1. [I2C 协议基础](#1-i2c-协议基础)
2. [Zephyr I2C 子系统架构](#2-zephyr-i2c-子系统架构)
3. [设备树配置](#3-设备树配置)
4. [Kconfig 配置](#4-kconfig-配置)
5. [I2C API 详解](#5-i2c-api-详解)
6. [I2C 地址的坑](#6-i2c-地址的坑)
7. [驱动代码实现](#7-驱动代码实现)
8. [总线扫描与调试](#8-总线扫描与调试)
9. [常见问题排查](#9-常见问题排查)
10. [移植指南](#10-移植指南)
11. [快速参考卡](#11-快速参考卡)

---

## 1. I2C 协议基础

### 1.1 I2C 是什么

I2C (Inter-Integrated Circuit) 是一种**半双工、同步、串行总线协议**，由 Philips（现 NXP）于 1982 年设计。它只需要两根信号线：

| 信号线 | 方向 | 功能 |
|--------|------|------|
| SCL (Serial Clock) | 主→从 | 时钟信号，由主设备驱动 |
| SDA (Serial Data) | 双向 | 数据信号，开漏输出 |

### 1.2 电气特性

```
        VCC
         │
         ├──[R]── SCL ──┬── 主设备 SCL
         │              └── 从设备 SCL
         │
         ├──[R]── SDA ──┬── 主设备 SDA
         │              └── 从设备 SDA
         │
        GND

关键点:
1. 开漏输出: 设备只能拉低线，不能拉高
2. 外部上拉电阻: 将线拉回高电平（典型 4.7KΩ @ 5V, 2.2KΩ @ 3.3V）
3. 线与逻辑: 任一设备拉低 = 总线低电平
4. STM32 内部上拉: 可用但很弱 (~40KΩ)，建议外部上拉
```

### 1.3 通信时序

```
基本写操作:
  主: [START] [ADDR+W] [REG_ADDR] [DATA] [STOP]
从:                    [ACK]      [ACK]   [ACK]

基本读操作:
  主: [START] [ADDR+W] [REG_ADDR] [RESTART] [ADDR+R]       [NACK] [STOP]
从:                    [ACK]                [ACK]    [DATA]

关键信号:
  START:  SCL=高, SDA 下降沿
  STOP:   SCL=高, SDA 上升沿
  RESTART: 同 START，但不释放总线（原子操作保证）
  ACK:    接收方拉低 SDA 一个时钟周期
  NACK:   接收方不拉低 SDA，表示"够了"或"我不在"
```

### 1.4 速率等级

| 模式 | 速率 | 线长建议 | 典型用途 |
|------|------|----------|----------|
| 标准模式 | 100 KHz | < 1m | 兼容性最好，面包板推荐 |
| 快速模式 | 400 KHz | < 0.5m | 大多数传感器支持 |
| 快速模式+ | 1 MHz | < 0.3m | 少数设备支持 |
| 高速模式 | 3.4 MHz | 极短 | 极少使用 |

---

## 2. Zephyr I2C 子系统架构

### 2.1 分层架构

```
┌─────────────────────────────────────────────┐
│              应用代码 (iic_board.c)           │
│    i2c_write() / i2c_read() / i2c_reg_xxx() │
├─────────────────────────────────────────────┤
│           Zephyr I2C 核心 API                │
│         include/zephyr/drivers/i2c.h         │
├─────────────────────────────────────────────┤
│          STM32 I2C 驱动 (v1/v2)              │
│     drivers/i2c/i2c_stm32_v1.c (F407用这个)  │
├─────────────────────────────────────────────┤
│           STM32 HAL / LL 库                  │
│          modules/hal/stm32/                   │
├─────────────────────────────────────────────┤
│              硬件 I2C 外设                    │
│         STM32F407 I2C1 (APB1 总线)           │
└─────────────────────────────────────────────┘
```

### 2.2 STM32 I2C 驱动版本

| STM32 系列 | 驱动 compatible | 特点 |
|------------|-----------------|------|
| F1/F2/F3/F4/F7/L1 | `st,stm32-i2c-v1` | 经典 I2C，不支持 DMA |
| G0/G4/H7/L4/L5/U5/WB/WL | `st,stm32-i2c-v2` | 新版 I2C，支持 DMA |

STM32F407 使用 **v1 驱动**，特点：
- 中断驱动，每次传输最大 255 字节
- 不支持 DMA
- 已知问题: 某些情况下 BUSY 标志卡住（驱动已处理）

### 2.3 API 同步/异步特性

**所有常用 I2C API 都是同步阻塞的**：

| API | 阻塞? | 说明 |
|-----|-------|------|
| `i2c_write()` | 阻塞 | 等待所有字节发送完成 |
| `i2c_read()` | 阻塞 | 等待所有字节接收完成 |
| `i2c_write_read()` | 阻塞 | 等待写+读两个阶段完成 |
| `i2c_reg_write_byte()` | 阻塞 | 等待寄存器写入完成 |
| `i2c_reg_read_byte()` | 阻塞 | 等待寄存器读取完成 |
| `i2c_burst_write()` | 阻塞 | 等待批量写入完成 |
| `i2c_burst_read()` | 阻塞 | 等待批量读取完成 |
| `i2c_transfer_cb()` | **异步** | 回调方式，使用场景较少 |

调用这些 API 的线程会阻塞直到 I2C 传输完成。在 400KHz 下，传输 1 字节约需 22.5μs。

---

## 3. 设备树配置

### 3.1 I2C 控制器节点

STM32F407 的 I2C1 在 SoC dtsi 中已有基础定义：

```dts
/* zephyr/dts/arm/st/f4/stm32f4.dtsi 中预定义 */
i2c1: i2c@40005400 {
    compatible = "st,stm32-i2c-v1";
    reg = <0x40005400 0x400>;
    clocks = <&rcc STM32_CLOCK_BUS_APB1 0x00200000>;
    interrupts = <31 0>, <32 0>;
    interrupt-names = "event", "error";
    status = "disabled";    /* 默认关闭，需在板级 DTS 中启用 */
};
```

### 3.2 板级 DTS 配置（完整示例）

```dts
&i2c1 {
    /* ===== 引脚配置（必须！） ===== */
    pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
    pinctrl-names = "default";

    /* ===== 时钟频率 ===== */
    clock-frequency = <I2C_BITRATE_FAST>;   /* 400 KHz */

    /* ===== 启用 ===== */
    status = "okay";

    /* ===== 总线上的设备子节点 ===== */
    ssd1306: ssd1306@3c {
        compatible = "solomon,ssd1306";
        reg = <0x3c>;
        /* ... 其他属性见 OLED 教程 ... */
    };
};
```

### 3.3 pinctrl 详解

**为什么必须配置 pinctrl？**

STM32 的每个引脚可以映射到多种外设功能（AF，Alternate Function），默认是 GPIO 模式。如果不配置 pinctrl，I2C 信号无法输出到引脚。

**pinctrl 节点在哪里定义？**

在 HAL 模块的 pinctrl dtsi 中：
```
modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi
```

I2C1 相关的引脚定义：

| 节点名 | 引脚 | AF | 说明 |
|--------|------|----|------|
| `i2c1_scl_pb6` | PB6 | AF4 | I2C1 默认 SCL |
| `i2c1_sda_pb7` | PB7 | AF4 | I2C1 默认 SDA |
| `i2c1_scl_pb8` | PB8 | AF4 | I2C1 重映射 SCL |
| `i2c1_sda_pb9` | PB9 | AF4 | I2C1 重映射 SDA |

这些节点内部已预设了关键属性：
- `drive-open-drain;` — I2C 必须开漏输出
- `bias-pull-up;` — 内部弱上拉（外部仍需上拉电阻）

**如何查找可用引脚？**

1. 查阅 STM32F407 数据手册的 "Alternate function mapping" 表
2. 或搜索 pinctrl dtsi 文件：`grep i2c1 modules/hal/stm32/dts/st/f4/stm32f407*pinctrl*`

### 3.4 clock-frequency 选项

```dts
/* 三种预定义速率常量 */
clock-frequency = <I2C_BITRATE_STANDARD>;  /* 100 KHz, 默认值 */
clock-frequency = <I2C_BITRATE_FAST>;      /* 400 KHz, 最常用 */
clock-frequency = <I2C_BITRATE_FAST_PLUS>; /* 1 MHz, 少数设备支持 */

/* 也可以直接写数值 */
clock-frequency = <100000>;   /* 等效 I2C_BITRATE_STANDARD */
```

### 3.5 总线设备子节点

I2C 总线上的设备通过子节点声明：

```dts
&i2c1 {
    /* 格式: label: node_name@unit_address */
    /*   label:         供 C 代码引用 (DT_NODELABEL) */
    /*   node_name:     描述性名称 + @ + unit_address */
    /*   unit_address:  必须与 reg 属性匹配 */

    my_sensor: sensor@48 {
        compatible = "vendor,sensor-model";  /* 匹配驱动 */
        reg = <0x48>;                        /* 7 位 I2C 地址 */
        label = "MY_SENSOR";                 /* 旧式标签（可选） */
    };
};
```

**compatible 的作用**：
- Zephyr 驱动模型通过 compatible 字符串匹配驱动程序
- 如果使用 `"zephyr,i2c-device"`，表示通用设备，不绑定专用驱动
- 专用驱动（如 `"solomon,ssd1306"`）会自动初始化并提供高级 API

---

## 4. Kconfig 配置

### 4.1 必需配置

```ini
# prj.conf
CONFIG_I2C=y
```

仅此一项即可。Zephyr 的 Kconfig 系统会根据 DTS 中的 `st,stm32-i2c-v1` compatible 自动选择正确的驱动。

### 4.2 可选配置

```ini
# I2C Shell 命令（调试利器）
CONFIG_I2C_SHELL=y

# 用法:
#   i2c scan I2C_1                    — 扫描总线
#   i2c read_byte I2C_1 0x3C 0x00    — 读寄存器
#   i2c write_byte I2C_1 0x3C 0x00 0xAE  — 写寄存器
```

### 4.3 配置依赖关系

```
CONFIG_I2C=y
  ├── 自动 select: 根据板子选择 I2C 控制器驱动
  │   └── STM32F407: CONFIG_I2C_STM32_V1=y (自动)
  └── 如果 DTS 中有 i2c 设备子节点且 status="okay"
      └── 驱动自动初始化
```

---

## 5. I2C API 详解

### 5.1 获取设备句柄

```c
/* 方式 1: 通过节点标签（推荐） */
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

/* 方式 2: 通过 chosen 节点（如果设置了 zephyr,i2c = &i2c1） */
static const struct device *i2c_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_i2c));

/* 检查设备就绪 */
if (!device_is_ready(i2c_dev)) {
    LOG_ERR("I2C device not ready!");
    return -ENODEV;
}
```

**为什么用 `DEVICE_DT_GET` 而不是 `device_get_binding`？**

| 方式 | 编译时检查 | 运行时查找 | 推荐度 |
|------|-----------|-----------|--------|
| `DEVICE_DT_GET(DT_NODELABEL(i2c1))` | 有（节点不存在则编译失败） | 无 | 推荐 |
| `device_get_binding("I2C_1")` | 无 | 有（字符串匹配） | 旧式，不推荐 |

### 5.2 核心 API 速查

#### 纯写（无寄存器地址前缀）

```c
int i2c_write(const struct device *dev, const uint8_t *buf,
              uint32_t len, uint8_t addr);
```
- 时序: `[START] [addr+W] [buf[0]] [buf[1]] ... [STOP]`
- 适用: OLED 控制字节写入、简单命令设备

#### 纯读（无寄存器地址前缀）

```c
int i2c_read(const struct device *dev, uint8_t *buf,
             uint32_t len, uint8_t addr);
```
- 时序: `[START] [addr+R] [buf[0]] [buf[1]] ... [STOP]`
- 适用: 读取设备 ID、FIFO 数据

#### 先写后读（最灵活）

```c
int i2c_write_read(const struct device *dev, uint16_t addr,
                   const void *write_buf, size_t write_len,
                   void *read_buf, size_t read_len);
```
- 时序: `[START] [addr+W] [write_buf] [RESTART] [addr+R] [read_buf] [STOP]`
- 适用: 写寄存器地址后立即读数据（最常见操作）

#### 寄存器读写（便捷封装）

```c
/* 写 1 字节寄存器 */
int i2c_reg_write_byte(const struct device *dev, uint8_t addr,
                       uint8_t reg, uint8_t val);

/* 读 1 字节寄存器 */
int i2c_reg_read_byte(const struct device *dev, uint8_t addr,
                      uint8_t reg, uint8_t *val);

/* 写多字节寄存器（地址自动递增） */
int i2c_burst_write(const struct device *dev, uint8_t addr,
                    uint8_t reg, const uint8_t *data, uint32_t len);

/* 读多字节寄存器（地址自动递增） */
int i2c_burst_read(const struct device *dev, uint8_t addr,
                   uint8_t reg, uint8_t *data, uint32_t len);
```

### 5.3 API 选择决策树

```
需要 I2C 通信？
│
├─ 需要指定寄存器地址？
│   ├─ 是 → 读还是写？
│   │   ├─ 写 1 字节 → i2c_reg_write_byte()
│   │   ├─ 读 1 字节 → i2c_reg_read_byte()
│   │   ├─ 写多字节 → i2c_burst_write()
│   │   └─ 读多字节 → i2c_burst_read()
│   └─ 否 → 纯数据传输
│       ├─ 写 → i2c_write()
│       ├─ 读 → i2c_read()
│       └─ 先写后读 → i2c_write_read()
│
└─ 特殊需求？
    ├─ 16 位寄存器地址 → i2c_write_read() 手动拼接
    └─ 复杂时序 → i2c_transfer() 自定义消息序列
```

---

## 6. I2C 地址的坑

### 6.1 7 位地址 vs 8 位地址

这是 I2C 新手最容易踩的坑：

```
8 位写地址: 0x78 = 0b0111_1000  ← 数据手册常见写法
8 位读地址: 0x79 = 0b0111_1001
7 位地址:   0x3C = 0b0111_100   ← Zephyr API 使用的格式

关系: 7位地址 = 8位写地址 >> 1
      0x3C << 1 = 0x78 (写)
      0x3C << 1 | 1 = 0x79 (读)
```

### 6.2 Zephyr 统一使用 7 位地址

所有 Zephyr I2C API 的 `addr` 参数都是 **7 位地址**，驱动内部自动处理 R/W 位拼接。

### 6.3 常见设备的地址对照

| 设备 | 数据手册地址 | 7 位地址 | DTS reg 值 |
|------|-------------|---------|------------|
| SSD1306 OLED | 0x78 / 0x7A | 0x3C / 0x3D | `<0x3C>` / `<0x3D>` |
| BMP280 气压 | 0x76 / 0x77 | 0x76 / 0x77 | `<0x76>` / `<0x77>` |
| MPU6050 加速度 | 0x68 / 0x69 | 0x68 / 0x69 | `<0x68>` / `<0x69>` |
| AT24C02 EEPROM | 0xA0 | 0x50 | `<0x50>` |

**注意**: BMP280/MPU6050 等设备的地址本身就是 7 位格式（高位为 0），不需要右移。只有当地址 ≥ 0x80 时才需要怀疑是 8 位格式。

### 6.4 如何判断？

- 地址 < 0x80 → 大概率就是 7 位地址，直接使用
- 地址 ≥ 0x80 → 大概率是 8 位地址，右移 1 位
- 查阅数据手册的 "I2C Device Address" 章节

---

## 7. 驱动代码实现

### 7.1 代码结构

```
src/
├── iic_board.h    — 接口声明
└── iic_board.c    — 实现（I2C 底层操作封装）
```

### 7.2 初始化

```c
static const struct device *i2c1_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

int i2c_board_init(void)
{
    if (!device_is_ready(i2c1_dev)) {
        LOG_ERR("I2C1 device not ready!");
        return -ENODEV;
    }
    return 0;
}
```

**关键点**：
- `DEVICE_DT_GET()` 是编译时常量，节点不存在则编译失败
- `device_is_ready()` 检查驱动是否已初始化就绪
- I2C 驱动在 `POST_KERNEL` 阶段初始化，`main()` 中调用时一定已就绪

### 7.3 总线扫描

```c
int i2c_board_scan(void)
{
    int count = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        /* 发送空写操作，检测设备是否应答 */
        if (i2c_write(i2c1_dev, NULL, 0, addr) == 0) {
            LOG_INF("Device found at 0x%02X", addr);
            count++;
        }
    }
    return count;
}
```

**扫描原理**：对每个地址发送 START + 地址 + STOP，设备回复 ACK 表示存在。

### 7.4 从设备树获取设备地址

```c
/* DTS: i2cdev@3c { reg = <0x3C>; }; */
#define I2C_DEV_ADDR  DT_REG_ADDR(DT_NODELABEL(i2c_device_0x78))

/* 使用时直接传给 API */
i2c_reg_read_byte(i2c1_dev, I2C_DEV_ADDR, 0x00, &val);
```

这样修改 DTS 中的 reg 值，C 代码自动跟随，无需手动同步。

---

## 8. 总线扫描与调试

### 8.1 使用 I2C Shell

启用 `CONFIG_I2C_SHELL=y` 后，可通过串口 Shell 调试：

```
# 扫描总线
uart:~$ i2c scan I2C_1
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
30: -- -- -- -- -- -- -- -- -- -- 3c -- -- -- -- --
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
70: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

1 devices found on I2C_1

# 读写寄存器
uart:~$ i2c read_byte I2C_1 0x3C 0x00
uart:~$ i2c write_byte I2C_1 0x3C 0x00 0xAE
```

### 8.2 常见扫描结果分析

| 现象 | 可能原因 | 解决方法 |
|------|---------|---------|
| 无设备发现 | 接线错误/未上拉 | 检查 SDA/SCL 接线，确认上拉电阻 |
| 所有地址都响应 | SDA 被拉低（短路） | 检查 SDA 线是否短路到 GND |
| 地址与预期不符 | 7/8 位地址混淆 | 尝试将地址右移 1 位 |
| 扫描成功但通信失败 | 速率过高 | 降低 clock-frequency |

---

## 9. 常见问题排查

### 9.1 pinctrl 未配置

**症状**: `i2c_write()` 返回负错误码，示波器看不到 SCL 波形

**原因**: 未配置 pinctrl，I2C 信号无法输出到引脚

**解决**: 在 DTS 中添加：
```dts
&i2c1 {
    pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
    pinctrl-names = "default";
};
```

### 9.2 device_is_ready() 返回 false

**原因**:
1. DTS 中 `status = "disabled"`
2. I2C 驱动未编译（缺少 `CONFIG_I2C=y`）
3. 时钟未使能（DTS 中 clocks 属性错误）

### 9.3 I2C 通信超时

**原因**:
1. 总线上无上拉电阻 → SDA/SCL 始终低电平
2. 从设备地址错误
3. I2C 速率过高（面包板杜邦线建议 ≤ 100KHz）
4. STM32 I2C v1 的 BUSY 标志卡住（驱动已处理，但极端情况仍可能发生）

### 9.4 多个 I2C 设备冲突

**症状**: 操作设备 A 后设备 B 通信失败

**原因**: 两个设备共享同一总线但地址相同

**解决**: 修改设备地址引脚配置（大多数 I2C 设备有地址选择引脚）

---

## 10. 移植指南

### 10.1 移植到其他 STM32 芯片

| 步骤 | 操作 |
|------|------|
| 1 | 确认芯片有哪些 I2C 外设（查阅数据手册） |
| 2 | 确认 I2C 引脚（查阅 AF mapping 表或 pinctrl dtsi） |
| 3 | 修改 DTS 中的 pinctrl-0 引脚定义 |
| 4 | 确认驱动版本：F4/L1 用 v1，G4/H7 用 v2 |
| 5 | 确认时钟源：I2C 时钟来自哪个 APB 总线 |

### 10.2 移植到其他品牌芯片

Zephyr I2C API 是统一的，只需修改 DTS 配置：

```dts
/* ESP32 示例 */
&i2c0 {
    sda-pin = <21>;
    scl-pin = <22>;
    clock-frequency = <I2C_BITRATE_FAST>;
    status = "okay";
};

/* NRF52840 示例 */
&i2c1 {
    compatible = "nordic,nrf-twi";
    sda-pin = <26>;
    scl-pin = <27>;
    status = "okay";
};
```

应用层代码（iic_board.c）无需修改，只需更新 DTS 和 `DEVICE_DT_GET` 中的节点名。

### 10.3 添加新 I2C 设备

1. 在 DTS 的 I2C 节点下添加子节点
2. 如果 Zephyr 有对应驱动（搜索 compatible），设备会自动初始化
3. 如果无专用驱动，使用 `i2c_write()`/`i2c_read()` 等底层 API 手动通信

---

## 11. 快速参考卡

### DTS 模板

```dts
&i2c1 {
    pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
    pinctrl-names = "default";
    clock-frequency = <I2C_BITRATE_FAST>;
    status = "okay";

    mydev: mydev@48 {
        compatible = "vendor,chip";
        reg = <0x48>;
    };
};
```

### C 代码模板

```c
#include <zephyr/drivers/i2c.h>

static const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

void example(void)
{
    if (!device_is_ready(i2c)) return;

    uint8_t val;
    i2c_reg_read_byte(i2c, 0x48, 0x00, &val);
    i2c_reg_write_byte(i2c, 0x48, 0x00, 0xFF);

    uint8_t buf[4];
    i2c_burst_read(i2c, 0x48, 0x00, buf, sizeof(buf));
}
```

### 错误码速查

| 返回值 | 含义 |
|--------|------|
| 0 | 成功 |
| -EIO | I/O 错误（NACK、总线错误等） |
| -ENOTSUP | 不支持的操作 |
| -EINVAL | 无效参数 |
| -ENODEV | 设备未就绪 |
