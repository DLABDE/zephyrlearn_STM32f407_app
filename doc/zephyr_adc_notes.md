# Zephyr ADC 子系统学习笔记

> 硬件: STM32F407VET6 DevEBox (ADC1: PA1/CH1, PA2/CH2)
> Zephyr: v4.4.0
> 目的: 从零理解 Zephyr ADC 子系统的架构、两种读取模式 (中断 vs DMA)、设备树配置、DMA 基础知识、以及完整的踩坑排查

---

## 目录

1. [架构总览](#1-架构总览)
2. [设备树配置 (DTS)](#2-设备树配置-dts)
    - 2.1 [adc1 节点](#21-adc1-节点-soc-dtsi-中预定义)
    - 2.2 [板卡 DTS 覆盖](#22-板卡-dts-覆盖-需要自己写)
    - 2.3 [DMA 控制器使能](#23-dma-控制器使能)
    - 2.4 [VREF 校准参考电压](#24-vref-校准参考电压-可选但推荐)
    - 2.5 [DTS 配置关系图](#25-完整-dts-配置关系图)
    - 2.6 [DTS channel vs C 代码](#26-dts-channeln-与-c-代码-adc_channel_cfg-的关系)
    - 2.7 [pinctrl 详解与内部通道](#27-pinctrl-详解与内部通道)
3. [Kconfig 配置 (prj.conf)](#3-kconfig-配置-prjconf)
4. [C 代码 API](#4-c-代码-api)
    - 4.9 [内部温度传感器 (通道 16)](#49-内部温度传感器-通道-16)
5. [两种读取模式详解](#5-两种读取模式详解)
6. [DMA 基础知识](#6-dma-基础知识)
7. [STM32 ADC 时钟与预分频器](#7-stm32-adc-时钟与预分频器)
8. [踩坑记录](#8-踩坑记录)
9. [配置速查表](#9-配置速查表)
10. [源码路径索引](#10-源码路径索引)

---

## 1. 架构总览

Zephyr ADC 子系统采用分层架构:

```
┌──────────────────────────────────────────────────────────────────┐
│                        应用代码                                   │
│  adc_read()  /  adc_raw_to_millivolts()                         │
│  封装: adc_read_channel()  /  adc_read_all_scan()               │
└──────────────────────────┬───────────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                    ADC 核心层 (API)                               │
│  include/zephyr/drivers/adc.h                                    │
│  统一接口: 不关心底层是 STM32 / Nordic / ESP32 ...               │
│  核心结构体: adc_sequence, adc_channel_cfg                       │
└──────────────────────────┬───────────────────────────────────────┘
                           │
┌──────────────────────────▼──────┬────────────────────────────────┐
│                         驱动层  │  DMA 层                         │
│  drivers/adc/adc_stm32.c        │  drivers/dma/dma_stm32_v1.c    │
│  ┌───────────────────────┐      │  ┌────────────────────┐        │
│  │ 中断模式: EOCS→ISR→读DR │      │  │ 扫描模式: DMA自动搬运 │        │
│  │ 适用于单通道采集        │      │  │ 适用于多通道采集      │        │
│  └───────────────────────┘      │  └────────────────────┘        │
└─────────────────────────────────┴────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                  硬件 (STM32F407)                                 │
│  ADC1 @ 0x40012000                                              │
│  ├─ IN1 (PA1)  ─→ 通道 1                                       │
│  ├─ IN2 (PA2)  ─→ 通道 2                                       │
│  ├─ IN16        ─→ 内部温度传感器                                │
│  ├─ IN17        ─→ 内部 VREFINT (~1.21V)                        │
│  └─ DMA2 Stream0 Channel0                                       │
└──────────────────────────────────────────────────────────────────┘
```

**核心概念**:

- **adc_sequence**: 定义一次 ADC 读取操作的参数 (通道位掩码、分辨率、缓冲区等)
- **adc_channel_cfg**: 定义通道的硬件配置 (增益、参考源、采样时间等)
- **adc_raw_to_millivolts()**: 将原始 ADC 值 (0~4095) 转换为实际电压 (mV)
- **SCAN 模式**: 硬件按顺序自动转换多个通道，各通道数据依次写入数据寄存器
- **OVR (Overrun)**: 上一次转换结果未被读取时，下一次转换已完成 → 数据丢失

---

## 2. 设备树配置 (DTS)

### 2.1 adc1 节点 (SoC dtsi 中预定义)

STM32 的 ADC 在 SoC 的 `.dtsi` 文件中已有基础定义:

```dts
/* zephyr/dts/arm/st/f4/stm32f4.dtsi (不需要自己写) */
adc1: adc@40012000 {
    compatible = "st,stm32f4-adc", "st,stm32-adc";
    reg = <0x40012000 0x400>;
    clocks = <&rcc STM32_CLOCK(APB2, 8)>;
    clock-names = "adcx";
    interrupts = <18 0>;
    #io-channel-cells = <1>;
    st,adc-resolutions = <12 10 8 6>;       /* 支持的分辨率 */
    sampling-times = <3 15 28 56 84 112 144 480>;  /* 采样周期选项 (单位: ADC时钟周期) */
    st,adc-clock-source = "SYNC";
    st,adc-sequencer = "programmable";
    status = "disabled";
};
```

### 2.2 板卡 DTS 覆盖 (需要自己写)

```dts
&adc1 {
    #address-cells = <1>;     /* 子节点 reg 地址占 1 个 32-bit cell (通道号) */
    #size-cells = <0>;        /* 子节点 reg 没有"大小"部分 (通道号没有大小概念) */

    pinctrl-0 = <&adc1_in1_pa1 &adc1_in2_pa2>;
    pinctrl-names = "default";
    st,adc-prescaler = <4>;   /* ADC 时钟预分频: APB2/(2×prescaler) = 84/8 = 10.5MHz */
    dmas = <&dma2 0 0 (STM32_DMA_PERIPH_RX | STM32_DMA_16BITS) STM32_DMA_FIFO_FULL>;
    status = "okay";

    channel@1 {
        reg = <1>;
        zephyr,gain = "ADC_GAIN_1";             /* 增益: ×1 (不放大不衰减) */
        zephyr,reference = "ADC_REF_INTERNAL";   /* 参考: 片内 VDDA ≈ 3.3V */
        zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;  /* 采样时间: 最短 */
        zephyr,resolution = <12>;               /* 分辨率: 12 位 (0~4095) */
    };

    channel@2 {
        reg = <2>;
        zephyr,gain = "ADC_GAIN_1";
        zephyr,reference = "ADC_REF_INTERNAL";
        zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
        zephyr,resolution = <12>;
    };
};
```

#### 关键属性说明

| 属性 | 说明 | 值 |
|------|------|-----|
| `#address-cells` | 子节点 reg 地址占用的 cell 数 | `<1>` — 通道号只需 1 个 32-bit cell |
| `#size-cells` | 子节点 reg 大小占用的 cell 数 | `<0>` — 通道号没有"大小" |
| `st,adc-prescaler` | ADC 时钟预分频系数 | `<4>` — APB2/(2×4) = 84/8 = 10.5MHz |
| `st,adc-clock-source` | 时钟源 (SoC dtsi 预定义) | `"SYNC"` — 与 APB2 总线时钟同步 |
| `dmas` | DMA 通道请求配置 (详见第 6 章) | DMA2 Stream0 Channel0, 外设→内存, 16-bit |

### 2.3 DMA 控制器使能

```dts
&dma2 {
    status = "okay";    /* 使能 DMA2 控制器 (ADC1 使用 DMA2 Stream0 Channel0) */
};
```

### 2.4 VREF 校准参考电压 (可选但推荐)

```dts
&vref {
    status = "okay";    /* 使能内部 VREFINT 通道 (ADC1 CH17, ~1.21V 校准电压) */
};
```

VREFINT 是芯片内部的稳定参考电压 (~1.21V)，驱动用它的测量值来反推真实的 VDDA 电压:

```
VDDA_real = VREFINT_cal / VREFINT_measured × VDDA_cal

例: VREFINT_cal = 1220mV (出厂校准值)
    VREFINT_measured = 1150mV (实际读到的raw值换算)
    VDDA_cal = 3300mV
    → VDDA_real = 1220/1150 × 3300 ≈ 3501mV (实际供电偏高!)
```

不启用 vref 时，驱动假设 VDDA = 3.3V，精度会下降，但学习阶段影响不大。

### 2.5 完整 DTS 配置关系图

```
SoC dtsi (预定义)                    板卡 DTS (自己写)
─────────────────                    ────────────────
adc1: adc@40012000 {                 &adc1 {
    compatible = "st,stm32f4-adc";       #address-cells = <1>;
    reg = <0x40012000 0x400>;            #size-cells = <0>;
    clocks = <&rcc ...>;                 st,adc-prescaler = <4>;
    interrupts = <18 0>;                 dmas = <&dma2 0 0 ...>;
    status = "disabled"; ──→             status = "okay";
                                      │
}                                     ├── channel@1 { reg = <1>; ... }
                                      └── channel@2 { reg = <2>; ... }
                                    };
&dma2 {
    status = "disabled"; ──→     &dma2 { status = "okay"; };
};
```

### 2.6 DTS channel@N 与 C 代码 adc_channel_cfg 的关系

**两者不重复！** 它们是互补的:

```
DTS channel@1 (硬件默认值):
  ┌─ reg = <1>                ← 通道编号
  ├─ zephyr,gain              ← 默认增益
  ├─ zephyr,reference         ← 默认参考源
  ├─ zephyr,acquisition-time  ← 默认采样时间
  └─ zephyr,resolution        ← 默认分辨率
            │
            ▼  驱动初始化时先读取 DTS 默认值
            │
  adc_channel_cfg (运行时覆盖):
  ┌─ .channel_id = 1          ← 指定要配置哪个通道
  ├─ .gain = ADC_GAIN_1       ← 覆盖/确认增益
  ├─ .reference = ADC_REF...  ← 覆盖/确认参考源
  └─ .acquisition_time = ...  ← 覆盖/确认采样时间
```

**规则**: 驱动先读 DTS，再用 `adc_channel_cfg` 中的值覆盖。未设置字段 (值为 0) 沿用 DTS 默认值。

简单场景下只写 DTS 或只写 C 代码也能工作，两者都写是为了显式控制 + 硬件可见性。

### 2.7 pinctrl 详解与内部通道

#### pinctrl 是什么

`pinctrl` = **Pin Control（引脚复用控制）**。STM32 的一个 GPIO 引脚可以扮演多种角色，pinctrl 就是告诉芯片"把这个引脚配置成 ADC 模式，而不是 GPIO 模式"。

#### pinctrl 的值从哪来

pinctrl 的可用值定义在一个单独的 `.dtsi` 文件中，你的板子 DTS 头部 `#include` 就包含了它：

```dts
/* stm32f4_devebox.dts */
#include <st/f4/stm32f407v(e-g)tx-pinctrl.dtsi>   /* ← 就是这个文件！ */
```

实际路径（在你的工程中）：
```
modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi
```

打开这个文件，搜索 `adc1`，会看到 **仅 16 个外部引脚的 pinctrl 定义**：

```dts
/omit-if-no-ref/ adc1_in0_pa0: adc1_in0_pa0 {  pinmux = <STM32_PINMUX('A', 0, ANALOG)>;  };
/omit-if-no-ref/ adc1_in1_pa1: adc1_in1_pa1 {  pinmux = <STM32_PINMUX('A', 1, ANALOG)>;  };
/omit-if-no-ref/ adc1_in2_pa2: adc1_in2_pa2 {  pinmux = <STM32_PINMUX('A', 2, ANALOG)>;  };
...
/omit-if-no-ref/ adc1_in15_pc5: adc1_in15_pc5 { pinmux = <STM32_PINMUX('C', 5, ANALOG)>;  };
/*  ← 到此为止！没有 adc1_in16、adc1_in17、adc1_in18 */
```

#### 如何自己查有哪些可用的 pinctrl

```
通用方法: 打开 include 链中的 pinctrl dtsi 文件，搜索对应外设名称。

本例: grep "adc1_in" modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi

结果: adc1_in0_pa0, adc1_in1_pa1, ..., adc1_in15_pc5  (只有 0~15)
```

`/omit-if-no-ref/` 是 Zephyr 的特殊语法：只有被 `&` 引用了才真正包含这个节点。所以 pinctrl 列表里的每一个值，只要你在 `pinctrl-0` 里写了 `&那个值`，就会生效。

#### 内部通道不需要 pinctrl

```
STM32F407 ADC1 的 19 个通道:
  ┌─ 通道 0~15 ─→ 外部 GPIO 引脚 (PA0, PA1, ..., PC5)
  │               └─ 需要 pinctrl 配置引脚为 ANALOG 模式
  │
  ├─ 通道 16   ─→ 内部温度传感器  (芯片内部模拟总线直连)
  │               └─ 不需要 pinctrl! 没有外部引脚!
  │
  ├─ 通道 17   ─→ 内部 VREFINT     (芯片内部模拟总线直连)
  │               └─ 不需要 pinctrl!
  │
  └─ 通道 18   ─→ 内部 VBAT 监测  (芯片内部模拟总线直连)
                  └─ 不需要 pinctrl!
```

**如果你在 pinctrl-0 中写了 `&adc1_in16`，编译会报错**：

```
devicetree error: undefined node label 'adc1_in16'
```

因为 `adc1_in16` 根本不存在于 pinctrl 文件中——通道 16 是内部温度传感器，不经过任何 GPIO，无需 pinctrl。

#### TSVREFE: 内部通道的"开关"

内部通道虽然不需要 pinctrl，但需要一个特殊的硬件使能位：**TSVREFE**（Temperature Sensor and VREFINT Enable）。

这个位在 ADC 通用控制寄存器（ADC_CCR）的第 23 位，负责打开内部温度传感器和 VREFINT 的模拟信号通路。**不设置这个位，通道 16 和 17 读到的值都是无效的。**

**关键发现**: Zephyr 的 STM32 ADC 驱动 (`adc_stm32.c`) **不会自动设置 TSVREFE**。你必须手动设置：

```c
#include <stm32_ll_adc.h>

static void adc_tempsensor_enable(void)
{
    ADC_TypeDef *adc = (ADC_TypeDef *)DT_REG_ADDR(ADC_NODE);

    /* 读取当前内部通道状态 */
    uint32_t path = LL_ADC_GetCommonPathInternalCh(
        STM32_ADC_COMMON_INSTANCE(adc));

    /* 如果温度传感器未使能，则设置 TSVREFE 位并等待稳定 */
    if (!(path & LL_ADC_PATH_INTERNAL_TEMPSENSOR)) {
        LL_ADC_SetCommonPathInternalCh(
            STM32_ADC_COMMON_INSTANCE(adc),
            path | LL_ADC_PATH_INTERNAL_TEMPSENSOR);
        k_usleep(LL_ADC_DELAY_TEMPSENSOR_STAB_US);  /* 等待稳定 ~10μs */
    }
}
```

> **注意**: TSVREFE 同时控制温度传感器（通道 16）和 VREFINT（通道 17）。所以只要你想用其中任何一个，就必须使能 TSVREFE。好的一面是使能一次，两个内部通道都能用。

---

## 3. Kconfig 配置 (prj.conf)

### 3.1 仅中断模式 (单通道逐读)

```ini
CONFIG_ADC=y                          # ADC 子系统总开关
```

### 3.2 DMA 模式 (多通道扫描)

```ini
CONFIG_ADC=y                          # ADC 子系统总开关
CONFIG_DMA=y                          # DMA 子系统总开关
CONFIG_ADC_STM32_DMA=y               # 启用 STM32 ADC DMA 模式
```

**注意**: `CONFIG_ADC_STM32_DMA` 是编译时开关。启用后驱动使用 DMA 搬运数据；不启用则使用中断逐次读取。

### 3.3 可选调试配置

```ini
CONFIG_LOG=y                          # 启用日志子系统
CONFIG_ADC_LOG_LEVEL_DBG=y           # ADC 驱动调试日志
CONFIG_MAIN_STACK_SIZE=2048          # 增大主线程栈 (ADC 调用链较深)
```

---

## 4. C 代码 API

### 4.1 头文件

```c
#include <zephyr/drivers/adc.h>       /* ADC API */
```

### 4.2 adc_sequence — 一次读取的参数

```c
struct adc_sequence {
    int32_t channels;          /* 通道位掩码: BIT(通道号), 如 BIT(1)|BIT(2) 扫描两个通道 */
    int16_t *buffer;           /* 数据缓冲区指针 */
    size_t buffer_size;        /* 缓冲区大小 (字节), 必须 ≥ 通道数 × sizeof(sample) */
    uint8_t resolution;        /* 分辨率 (bits), STM32F4 支持 12/10/8/6 */
    /* ... 其他字段省略 */
};
```

### 4.3 adc_channel_cfg — 通道配置

```c
struct adc_channel_cfg {
    enum adc_gain gain;                /* 增益: ADC_GAIN_1 = ×1 */
    enum adc_reference reference;      /* 参考源: ADC_REF_INTERNAL = VDDA */
    uint16_t acquisition_time;         /* 采样时间: ADC_ACQ_TIME_DEFAULT */
    uint8_t channel_id;                /* 物理通道号: 1=ADC_IN1, 2=ADC_IN2 */
    /* 可选字段 (未设置=使用默认值): */
    bool differential;                 /* 差分模式 (默认 false = 单端) */
    uint8_t input_positive;            /* 差分模式正输入端 */
};
```

### 4.4 设备获取与就绪检查

```c
#define ADC_NODE DT_ALIAS(adc1)
const struct device *adc_dev = DEVICE_DT_GET(ADC_NODE);

/* 检查设备就绪 */
if (!device_is_ready(adc_dev)) {
    printk("ADC device not ready\n");
    return -1;
}
```

**注意**: ADC 没有 GPIO 那样的 `*_DT_SPEC_GET` 封装宏，只能直接用 `const struct device *`。

### 4.5 通道初始化

```c
struct adc_channel_cfg channel_cfg1 = {
    .gain = ADC_GAIN_1,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id = 1,
};
ret = adc_channel_setup(adc_dev, &channel_cfg1);

/* 每个通道必须单独调用一次 adc_channel_setup */
struct adc_channel_cfg channel_cfg2 = {
    .gain = ADC_GAIN_1,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id = 2,
};
ret = adc_channel_setup(adc_dev, &channel_cfg2);
```

### 4.6 读取 ADC (方式一: 中断逐通道)

```c
/* adc_sequence 只包含 1 个通道 → 驱动不启用 SCAN 模式 */
static const struct adc_sequence seq_ch1 = {
    .channels = BIT(1),
    .buffer = &sample_buf,
    .buffer_size = sizeof(sample_buf),
    .resolution = 12,
};

ret = adc_read(adc_dev, &seq_ch1);  /* 阻塞等待转换完成 */
raw = sample_buf;
adc_raw_to_millivolts(adc_ref_internal(adc_dev), ADC_GAIN_1, 12, &raw);

/* 再读第二个通道 */
ret = adc_read(adc_dev, &seq_ch2);
```

### 4.7 读取 ADC (方式二: DMA 扫描)

```c
/* adc_sequence 包含 2 个通道 → 驱动启用 SCAN + DMA */
static int16_t sample_buffer_dma[2];
static const struct adc_sequence seq_scan = {
    .channels = BIT(1) | BIT(2),
    .buffer = sample_buffer_dma,
    .buffer_size = sizeof(sample_buffer_dma),
    .resolution = 12,
};

ret = adc_read(adc_dev, &seq_scan);  /* DMA 自动搬运两个通道的数据 */
raw0 = sample_buffer_dma[0];         /* 通道 1 的值 */
raw1 = sample_buffer_dma[1];         /* 通道 2 的值 */
```

### 4.8 原始值转电压

```c
/*
 * 公式: mV = raw / 2^resolution × Vref × 1000
 *
 * 例 (12-bit, Vref = 3.3V):
 *   raw = 2047 → mV = 2047/4096 × 3300 ≈ 1649 mV
 *   raw = 4095 → mV = 4095/4096 × 3300 ≈ 3299 mV
 */
adc_raw_to_millivolts(adc_ref_internal(adc_dev), ADC_GAIN_1, 12, &raw);
```

**注意**: `adc_raw_to_millivolts` 就地修改 `raw` 变量的值，输入输出是同一个变量。

### 4.9 内部温度传感器 (通道 16)

#### 硬件原理

STM32F407 芯片内部集成了一个温度传感器，连接到 ADC1 的通道 16。它输出电压随温度线性变化：

```
V_temp ≈ 760mV (25°C) + 2.5mV/°C × ΔT
```

#### 特殊要求

| 要求 | 说明 |
|------|------|
| TSVREFE = 1 | ADC_CCR 寄存器第 23 位，使能内部温度传感器通路（见 [2.7](#27-pinctrl-详解与内部通道)） |
| 稳定时间 | 使能后需等待 `LL_ADC_DELAY_TEMPSENSOR_STAB_US` ≈ 10μs |
| 采样时间 | 数据手册要求 ≥ 10μs。使用 `ADC_ACQ_TIME_MAX`（480 周期 @ 10.5MHz ≈ 45.7μs） |

#### 温度转换公式 (STM32F4 双点校准)

ST 出厂时为每颗芯片写入了两个校准值（存储在系统 Flash 的 OTP 区域）：

| 地址 | 含义 |
|------|------|
| `0x1FFF7A2C` | **TS_CAL1**: ADC 原始值（在 30°C、VDDA=3.3V 时测量） |
| `0x1FFF7A2E` | **TS_CAL2**: ADC 原始值（在 110°C、VDDA=3.3V 时测量） |

利用这两个校准点进行线性插值：

```
slope = (110 - 30) / (TS_CAL2 - TS_CAL1)
temp  = slope × (raw - TS_CAL1) + 30
```

**注意**: 校准值是在 VDDA = 3.3V 条件下测量的。如果实际 VDDA 不同，需要先将 raw 值按比例修正。使用 `adc_raw_to_millivolts()` 先将校准值和测量值都转为 mV，就能自动抵消 VDDA 差异。

#### 完整实现

```c
#include <stm32_ll_adc.h>

/*
 * 步骤 1: 在 adc_init() 中使能 TSVREFE（必须在 channel_setup 之前）
 */
static void adc_tempsensor_enable(void)
{
    ADC_TypeDef *adc = (ADC_TypeDef *)DT_REG_ADDR(ADC_NODE);
    uint32_t path = LL_ADC_GetCommonPathInternalCh(
        STM32_ADC_COMMON_INSTANCE(adc));
    if (!(path & LL_ADC_PATH_INTERNAL_TEMPSENSOR)) {
        LL_ADC_SetCommonPathInternalCh(STM32_ADC_COMMON_INSTANCE(adc),
                                       path | LL_ADC_PATH_INTERNAL_TEMPSENSOR);
        k_usleep(LL_ADC_DELAY_TEMPSENSOR_STAB_US);
    }
}

/*
 * 步骤 2: 配置通道 16（需要最长的采样时间）
 */
struct adc_channel_cfg channel_cfg16 = {
    .gain = ADC_GAIN_1,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_MAX,   /* 温度传感器必须用最长采样时间 */
    .channel_id = 16,
};
adc_channel_setup(adc_dev, &channel_cfg16);

/*
 * 步骤 3: 温度转换函数 (raw_mV → °C)
 */
static int32_t adc_raw_to_temp_celsius(int32_t raw_mv)
{
    uint32_t ts_cal1 = *((uint16_t *)0x1FFF7A2C);
    uint32_t ts_cal2 = *((uint16_t *)0x1FFF7A2E);

    adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                          ADC_GAIN_1, 12, (int32_t *)&ts_cal1);
    adc_raw_to_millivolts(adc_ref_internal(adc_dev),
                          ADC_GAIN_1, 12, (int32_t *)&ts_cal2);

    return (int32_t)((int64_t)(raw_mv - ts_cal1) * 80
                     / (ts_cal2 - ts_cal1) + 30);
}

/*
 * 步骤 4: DMA 扫描模式中加入通道 16
 */
static int16_t sample_buffer_dma[3];  /* 3 个通道 */
static const struct adc_sequence seq_scan = {
    .channels = BIT(1) | BIT(2) | BIT(16),   /* CH1 + CH2 + 温度 */
    .buffer = sample_buffer_dma,
    .buffer_size = sizeof(sample_buffer_dma),
    .resolution = 12,
};

/* DMA 搬运完成后: */
/* sample_buffer_dma[0] = 通道 1 (PA1)  */
/* sample_buffer_dma[1] = 通道 2 (PA2)  */
/* sample_buffer_dma[2] = 通道 16 (温度) */
```

> **DMA 多通道数据顺序**: SCAN 模式下 DMA 按通道号从小到大依次搬运，与 `channels` 中 BIT(N) 的书写顺序无关。`BIT(1)|BIT(2)|BIT(16)` 的结果与 `BIT(16)|BIT(1)|BIT(2)` 一样，buffer[0] 始终是通道 1，buffer[1] 是通道 2，buffer[2] 是通道 16。

---

## 5. 两种读取模式详解

### 5.1 方式一: 中断逐通道模式

```
adc_read(通道1)
  │
  ├─ 驱动: channels = BIT(1) → 单通道, 不启用 SCAN
  ├─ 硬件: 转换 1 次 → EOCS 中断 → ISR 读 DR → sem_give()
  └─ 返回

adc_read(通道2)
  │
  └─ (同上, 通道 2)
```

| 优点 | 缺点 |
|------|------|
| 不需要 DMA | 多通道时需要多次调用 adc_read |
| 纯中断即可，配置简单 | 每次调用有 sem 等待开销 |
| STM32F4 非 DMA 模式唯一安全方案 | 不适合高速连续采集 |

### 5.2 方式二: DMA 扫描模式

```
adc_read(通道1+2)
  │
  ├─ 驱动: channels = BIT(1)|BIT(2) → SCAN 模式 + DMA
  ├─ 硬件: 转换 通道1 → DMA 读 DR → 写 buffer[0]
  │             转换 通道2 → DMA 读 DR → 写 buffer[1]
  └─ DMA 传输完成中断 → sem_give() → 返回
```

```
┌─────────── 时间线 ───────────┐
│ 通道1转换 (采样+12周期)      │  ~1.4μs
│ DMA搬数据 (1次总线操作)      │  ~12ns    ← DMA 足够快!
│ 通道2转换 (采样+12周期)      │  ~1.4μs
│ DMA搬数据 (1次总线操作)      │  ~12ns
│ DMA完成中断 → sem_give()    │
└──────────────────────────────┘
```

| 优点 | 缺点 |
|------|------|
| 一次调用读取多通道 | 需要 DMA 配置 (DTS + Kconfig) |
| CPU 零开销搬运数据 | 需要额外的 DMA 控制器时钟 |
| 不会发生 OVR | 编译时确定 (CONFIG_ADC_STM32_DMA) |

### 5.3 为什么 STM32F4 非 DMA 模式不能多通道扫描

```
┌── 硬件行为 ──────────────────────────────────────────┐
│                                                      │
│  SCAN 模式: 通道1转换完 → 立即开始通道2 (背靠背!)    │
│                                                      │
│  通道1完成 → EOCS=1 → NVIC pending                  │
│  通道2完成 → EOCS=1 → NVIC 已 pending (不会重复!)    │
│            → DR 被通道2数据覆盖 → OVR!               │
│                                                      │
│  ISR 终于触发 (处理了一次合并后的中断):               │
│   1. OVR=1 → 清除标志 → 打印错误                     │
│   2. 读 DR → 拿到通道2数据 (通道1丢失)               │
│   3. samples_count=1 ≠ channel_count=2               │
│   4. 没有更多 EOCS → 信号量永不释放 → 死机!          │
└──────────────────────────────────────────────────────┘
```

**结论**: STM32F4 的 SCAN 模式必须配合 DMA 使用。纯中断模式只能逐通道读取。

---

## 6. DMA 基础知识

### 6.1 DMA 是什么

DMA (Direct Memory Access，直接存储器访问) 是一种硬件机制，允许外设和内存之间直接传输数据，**不经过 CPU**。

```
无 DMA:                     有 DMA:
┌─────┐      ┌─────┐       ┌─────┐               ┌─────┐
│ CPU │ ←1→  │ RAM │       │ CPU │  "DMA, 帮我    │ RAM │
└──┬──┘      └─────┘       └──┬──┘   搬数据!"     └──▲──┘
   │ ① 读ADC_DR              │                      │
┌──▼──┐                      │             ┌────────┴──┐
│ ADC │                      └────────────→│   DMA     │
└─────┘                                     │ 控制器    │
                                            └──┬────┬──┘
                                               │ ②  │ ③
                                            ┌──▼──┐ │
                                            │ ADC │←┘
                                            └─────┘
① CPU 读: 中断+读寄存器+写内存 (每条指令占CPU)
② DMA 读: 硬件自动从 ADC_DR 读数据
③ DMA 写: 硬件自动写入 RAM 缓冲区
```

### 6.2 STM32F4 DMA 架构

```
STM32F407 有 2 个 DMA 控制器:
  DMA1: 8 个 Stream (0~7), 每个 Stream 有 8 个 Channel (0~7)
  DMA2: 8 个 Stream (0~7), 每个 Stream 有 8 个 Channel (0~7)

ADC1 的 DMA 连接:
  ADC1 → DMA2 Stream 0, Channel 0
  或者  ADC1 → DMA2 Stream 4, Channel 0  (两种都可选)
```

**Stream** vs **Channel**:
- **Channel (通道)**: 外设的 DMA 请求线编号，每个外设映射到固定的 Channel
- **Stream (流)**: DMA 控制器的独立处理单元，每个 Stream 可以选择一个 Channel

类比例子: 一个 DMA 控制器有 8 个"出纳窗口" (Stream)，每个窗口可以从 8 个"队列" (Channel) 中选择一个服务。

### 6.3 DTS 中 dmas 属性详解

```dts
dmas = <&dma2 0 0 (STM32_DMA_PERIPH_RX | STM32_DMA_16BITS) STM32_DMA_FIFO_FULL>;
          │   │ │  │                                                       │
          │   │ │  │                                                       └─ Cell 3: 特性
          │   │ │  └─ Cell 2: 通道配置 (方向+数据宽度+地址递增)               (FIFO阈值)
          │   │ └─ Cell 1: Slot (DMA 请求通道号 0 = ADC1)
          │   └─ Cell 0: Channel (DMA2 Stream 0)
          └─ DMA 控制器引用
```

详细拆解:

| Cell | 值 | 含义 |
|------|-----|------|
| Cell 0: channel | `0` | DMA2 Stream 0 |
| Cell 1: slot | `0` | DMA Channel 0 (ADC1 的请求线) |
| Cell 2: channel_config | `(PERIPH_RX \| 16BITS)` | 外设→内存 + 16-bit 数据宽度 + 内存地址递增 |
| Cell 3: features | `FIFO_FULL` | FIFO 全满阈值 |

`STM32_DMA_PERIPH_RX` 展开后:

```
STM32_DMA_PERIPH_RX = STM32_DMA_PERIPH_TO_MEMORY | STM32_DMA_MEM_INC
                    = 方向: 外设读 → 内存写  +  内存地址自动递增

STM32_DMA_16BITS = STM32_DMA_PERIPH_16BITS | STM32_DMA_MEM_16BITS
                 = 外设数据宽度 16-bit   +  内存数据宽度 16-bit
```

### 6.4 突发传输 (Burst)

突发传输是一次总线申请搬运多个数据单元的能力:

```
单拍 (Single):  申请总线 → 读1 → 写1 → 释放总线  (每次搬 1 个半字)
4拍 (INCR4):    申请总线 → 读1→读2→读3→读4 → 写1→写2→写3→写4 → 释放

好处: 减少总线申请开销，提高吞吐量
STM32F4 选项: SINGLE(0), INCR4(1), INCR8(2), INCR16(3)
```

对于 ADC 这种低速外设，单拍传输完全够用。之前 DMA 驱动报的 "burst size error" 是驱动内部将 data_size 误当作 burst 值，驱动会自动降级为 SINGLE，不影响功能。

### 6.5 dmas 与 Kconfig 的关系

```
dmas 属性只是声明 "这个外设可以用 DMA"。
是否真正启用取决于:
  1. Kconfig: CONFIG_DMA=y AND CONFIG_ADC_STM32_DMA=y
  2. DTS: dma 控制器 status = "okay"
Key:   1 AND 2 → DMA 模式
       只有 dmas 但 Kconfig 没开 → 驱动不初始化 DMA, 当做没有 DMA
       没有 dmas → 只能用中断模式
```

---

## 7. STM32 ADC 时钟与预分频器

### 7.1 时钟树

```
STM32F407 ADC1 时钟路径:
  HSE (8MHz) → PLL ×336 → SYSCLK (168MHz)
                         → AHB (/1) = 168MHz
                            → APB2 (/2) = 84MHz
                                                    ┌── ADC 预分频器 ─┐
                               → ADC 时钟源 ────────→ ADC时钟 = ? MHz
                               (SYNC = APB2)
```

### 7.2 预分频器计算

STM32F4 的 ADC 预分频器是 2 的幂:

```
ADC 时钟 = APB2 / (2 × st,adc-prescaler)

st,adc-prescaler = <2>   → ADC时钟 = 84 / 4 = 21 MHz
st,adc-prescaler = <4>   → ADC时钟 = 84 / 8 = 10.5 MHz
st,adc-prescaler = <8>   → ADC时钟 = 84 / 16 = 5.25 MHz
```

**STM32F407 规格**: ADC 最大时钟 = **36 MHz**。超过此值导致转换不稳定。

### 7.3 如何选择预分频器

```
转换时间 = (采样周期 + 分辨率) / ADC时钟

以 12-bit, sampling = 3 周期为例:
  prescaler=2:  转换时间 = (3+12) / 21MHz  ≈ 714ns  ← 信号质量好时推荐
  prescaler=4:  转换时间 = (3+12) / 10.5MHz ≈ 1.43μs ← 适中
  prescaler=8:  转换时间 = (3+12) / 5.25MHz ≈ 2.86μs ← 信号内阻大时推荐
```

原则: **在满足信号质量的前提下，选择较小的分频以获得更快的转换速度。**

---

## 8. 踩坑记录

### 坑 1: undefined reference to `__device_dts_ord_12` (编译失败)

**现象**:
```
undefined reference to `__device_dts_ord_12'
collect2.exe: error: ld returned 1 exit status
```

**根因**: `prj.conf` 中缺少 `CONFIG_ADC=y`。Zephyr 中 DTS 和 Kconfig 是两套独立机制:
- DTS 声明硬件存在 (status = "okay")
- Kconfig 决定驱动是否编译 (CONFIG_ADC=y)
- **两者缺一不可！** 只有 DTS 没有 Kconfig → 驱动不编译 → 设备实例符号不存在 → 链接失败

**修复**:
```ini
CONFIG_ADC=y
```

### 坑 2: 首次调用 adc_read 后约 1 秒死机

**现象**: 系统正常运行约 1 秒 (led_count=1000 时触发 ADC 读取)，然后死机无输出。

**根因**: Zephyr 默认 main 线程栈只有 **1024 字节**。`adc_read()` 调用链较深 (HAL 层 → LL 层 → 寄存器操作 + 中断上下文)，加上 `adc_raw_to_millivolts` 和 `printk` 的栈消耗，栈溢出触发 Hard Fault。

**修复**:
```ini
CONFIG_MAIN_STACK_SIZE=2048
```

**经验法则**: 使用 ADC、I2C、SPI 等复杂外设驱动时，建议 main 线程栈 ≥ 2048。

### 坑 3: ADC overrun error — 多通道扫描 + 中断模式 = OVR

**现象**:
```
<err> adc_stm32: ADC overrun error occurred. Use DMA, reduce clock source
     frequency, increase prescaler value or increase sampling times.
```
随后系统死机，无更多输出。

**根因**: STM32F4 在 SCAN 模式下两个通道背靠背连续转换 (间隔仅 ~1.4μs)。NVIC 无法为两个 EOCS 各触发一次 ISR (两个 EOCS 合并成一次 pending)，导致前一个通道的数据被覆盖 → OVR → 信号量永不释放 → 死机。

**关键**: 这与时钟频率无关！即使把 prescaler 设到 8 (10.5MHz)，两个通道间隔也只有 ~1.4μs，NVIC 中断延迟通常在数百 ns ~ 数 μs，无法保证每次都响应到。

**修复**: 二选一
1. **逐通道读取** (中断模式): 每次 adc_read 只请求 1 个通道
2. **DMA 扫描模式**: 启用 DMA 后，硬件自动搬运数据，不存在 ISR 读取延迟

### 坑 4: `||` 逻辑运算符误用

**现象**: 只读取到一个通道的值，另一个通道始终不变。

**根因**:
```c
/* 错误: || 是逻辑或，有短路求值! */
ret = adc0_get_mv(0, &val0_mv) || adc0_get_mv(1, &val1_mv);

/* 如果第一个调用返回非零 (失败)，第二个调用不会执行! */
/* 且 ret 只能是 0 或 1，丢失实际错误码 */
```

**修复**:
```c
/* 逐通道模式 */
ret  = adc_read_channel(1, &val0_mv);
ret |= adc_read_channel(2, &val1_mv);

/* 或 DMA 扫描模式 (推荐) */
ret = adc_read_all_scan(&val0_mv, &val1_mv);
```

### 坑 5: DMA burst size 无害警告

**现象**:
```
<err> dma_stm32_v1: Memory burst size error,using single burst as default
<err> dma_stm32_v1: Peripheral burst size error,using single burst as default
```

**根因**: Zephyr ADC 驱动的 DMA 配置宏将数据宽度值 (16-bit = 1) 当作突发长度 (INCR4 = 1) 传给 DMA 驱动。DMA 驱动检测到 FIFO 未启用时不支持突发传输，自动降级为 SINGLE。

**影响**: 完全无害。DMA 驱动已自动修正为正确配置，数据传输正常。只是 `LOG_ERR` 级别的日志输出。

**修复**: 删除 `prj.conf` 中的 `CONFIG_LOG=y` 即可静默; 或不做任何处理 (不影响功能)。

### 坑 6: DTS channel@N 与 C 代码 adc_channel_cfg 的困惑

**现象**: 不理解为什么同一个通道的参数需要在 DTS 和 C 代码中都定义一遍。

**答案**:
```
DTS channel@1: 硬件默认配置，在驱动初始化时读取
C 代码 adc_channel_cfg: 运行时覆盖，可以在运行时动态修改

实际流程:
  驱动 → 读 DTS 默认值 → 用 adc_channel_cfg 覆盖 → 写入硬件寄存器

如果 adc_channel_cfg 某字段 = 0 (未设置)，则沿用 DTS 默认值。
```

简单场景下只写 DTS 或只写 C 代码也能工作，两者都写是为了显式控制 + 硬件可见性。

### 坑 7: `undefined node label 'adc1_in16'` — 内部通道加了 pinctrl

**现象**:
```
devicetree error: /soc/adc@40012000: undefined node label 'adc1_in16'
```

**根因**: 在 `pinctrl-0` 中添加了 `&adc1_in16`，但通道 16 是内部温度传感器，不经过 GPIO 引脚，pinctrl 文件中不存在 `adc1_in16` 的定义。

**排查方法**: 打开 pinctrl dtsi 文件搜索 `adc1_in`，只能找到 0~15。

**修复**:
```dts
/* 移除内部通道的 pinctrl 引用 */
pinctrl-0 = <&adc1_in1_pa1 &adc1_in2_pa2>;
/*          ← 没有 &adc1_in16！内部通道不需要 pinctrl */
```

**关键**: 只有外部引脚通道（0~15）需要 pinctrl。内部通道（16: 温度, 17: VREFINT, 18: VBAT）不需要 pinctrl，但需要设置 TSVREFE 位（见 [2.7](#27-pinctrl-详解与内部通道)）。

---

## 9. 配置速查表

### DTS 层

```
adc1 节点:
  #address-cells = <1>;                                     子节点只含通道号
  #size-cells = <0>;                                        通道号无"大小"
  pinctrl-0 = <&adc1_in1_pa1 &adc1_in2_pa2>;               ADC 引脚
  st,adc-prescaler = <4>;                                   ADC 时钟 = APB2/8
  dmas = <&dma2 0 0 (PERIPH_RX|16BITS) FIFO_FULL>;        DMA 配置
  status = "okay";

dma2 节点:
  status = "okay";                                          使能 DMA2 控制器

vref 节点 (可选):
  status = "okay";                                          提高电压转换精度

channel@N:
  reg = <N>;                                                通道编号
  zephyr,gain = "ADC_GAIN_1";                              增益 ×1
  zephyr,reference = "ADC_REF_INTERNAL";                   参考源 VDDA
  zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;        外部通道: 最短采样时间
  zephyr,acquisition-time = <ADC_ACQ_TIME_MAX>;            内部通道(16/17): 最长采样时间
  zephyr,resolution = <12>;                                12 位精度

TSVREFE (内部通道使能):
  LL_ADC_SetCommonPathInternalCh(... | LL_ADC_PATH_INTERNAL_TEMPSENSOR)
  k_usleep(LL_ADC_DELAY_TEMPSENSOR_STAB_US)                等待约 10μs 稳定

pinctrl 查找:
  打开 modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi
  搜索对应外设名 (如 "adc1_in") 即可看到所有可用 pinctrl 值
  外部通道 0~15 需要 pinctrl，内部通道 16~18 不需要
```

### Kconfig 层

```
中断模式:
  CONFIG_ADC=y
  CONFIG_MAIN_STACK_SIZE=2048

DMA 模式:
  CONFIG_ADC=y
  CONFIG_DMA=y
  CONFIG_ADC_STM32_DMA=y
  CONFIG_MAIN_STACK_SIZE=2048
```

### C 代码层

```
/* 初始化 */
DEVICE_DT_GET(DT_ALIAS(adc1))          获取 ADC 设备
device_is_ready(adc_dev)               检查设备就绪
adc_channel_setup(dev, &cfg)           配置通道参数 (每个通道一次)

/* 模式一: 中断逐通道 (不需要 DMA) */
adc_sequence.channels = BIT(N)         单通道
adc_read(dev, &seq)                    阻塞读取
adc_raw_to_millivolts(...)             原始值 → mV

/* 模式二: DMA 扫描 (需要 DMA) */
adc_sequence.channels = BIT(1)|BIT(2)  多通道
adc_sequence.buffer = sample_buf[N]    N = 通道数
adc_read(dev, &seq)                    DMA 自动搬运

/* 温度传感器 (通道 16) */
LL_ADC_SetCommonPathInternalCh(...)    使能 TSVREFE（通道 16/17 必须）
adc_channel_setup(ch16, ADC_ACQ_TIME_MAX)  长采样时间
adc_raw_to_temp_celsius(raw_mV)        原始 mV → °C（双点校准插值）
```

---

## 10. 源码路径索引

| 文件 | 路径 | 说明 |
|------|------|------|
| STM32 ADC 驱动 | `drivers/adc/adc_stm32.c` | 含 ISR、DMA、时钟初始化 |
| STM32 DMA v1 驱动 | `drivers/dma/dma_stm32_v1.c` | DMA 配置与传输控制 |
| ADC API 头文件 | `include/zephyr/drivers/adc.h` | adc_sequence, adc_channel_cfg |
| ADC 控制器绑定 | `dts/bindings/adc/adc-controller.yaml` | 通用 ADC 绑定 |
| STM32 ADC 绑定 | `dts/bindings/adc/st,stm32-adc.yaml` | STM32 ADC DTS 属性 |
| STM32 DMA 绑定 | `dts/bindings/dma/st,stm32-dma-v1.yaml` | DMA 四个 cell 详解 |
| DMA 宏定义 | `include/zephyr/dt-bindings/dma/stm32_dma.h` | PERIPH_RX, 16BITS 等宏 |
| STM32F4 dtsi | `dts/arm/st/f4/stm32f4.dtsi` | ADC1 基础定义 + VREF |
| STM32F4 pinctrl | `modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi` | adc1_in1_pa1 等 |
| 本项目 adc.c | `code/blinky_test/src/adc.c` | 两种读取模式完整实现 |
| 本项目 DTS | `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts` | 板级 ADC 配置 |
| STM32 温度传感器驱动 | `drivers/sensor/st/stm32_temp/stm32_temp.c` | `die_temp` 传感器驱动（含 TSVREFE 使能） |
| LL ADC 头文件 | `modules/hal/stm32/stm32cube/stm32f4xx/.../stm32f4xx_ll_adc.h` | `LL_ADC_SetCommonPathInternalCh()` 等 |
| 本板卡 pinctrl | `modules/hal/stm32/dts/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi` | 查 pinctrl 可用值的入口 |