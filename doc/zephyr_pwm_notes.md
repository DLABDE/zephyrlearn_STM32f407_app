# Zephyr PWM 子系统学习笔记

> 硬件: STM32F407VET6 DevEBox (LED: PA6/TIM3_CH1, PA7/TIM3_CH2)
> Zephyr: v4.4.0
> 目的: 从零理解 Zephyr PWM 子系统的架构、设备树配置、C API、pinctrl 引脚复用、以及 GPIO/PWM 动态切换

---

## 目录

1. [架构总览](#1-架构总览)
2. [设备树配置 (DTS)](#2-设备树配置-dts)
3. [Kconfig 配置 (prj.conf)](#3-kconfig配置-prjconf)
4. [C 代码 API](#4-c-代码-api)
5. [Pinctrl 引脚复用与 GPIO/PWM 动态切换](#5-pinctrl-引脚复用与-gpiopwm-动态切换)
6. [STM32 定时器与预分频器](#6-stm32-定时器与预分频器)
7. [踩坑记录](#7-踩坑记录)
8. [DTS 别名规则详解](#8-dts-别名规则详解)
9. [配置速查表](#9-配置速查表)
10. [源码路径索引](#10-源码路径索引)

---

## 1. 架构总览

Zephyr PWM 子系统采用分层架构，与 Linux PWM 子系统设计理念类似:

```
┌──────────────────────────────────────────────────────────────────┐
│                        应用代码                                   │
│  pwm_set_pulse_dt()  /  pwm_set_cycles()  /  pwm_set()         │
└──────────────────────────┬───────────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                    PWM 核心层 (API)                               │
│  include/zephyr/drivers/pwm.h                                    │
│  统一接口: 不关心底层是 STM32 / Nordic / ESP32 ...               │
└──────────────────────────┬───────────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                  硬件驱动层                                       │
│  drivers/pwm/pwm_stm32.c  →  操作 TIMx 寄存器                   │
│  drivers/pwm/pwm_nrfx.c   →  操作 Nordic PWM 外设               │
│  drivers/pwm/pwm_esp32.c  →  操作 ESP32 LEDC/MCPWM              │
└──────────────────────────┬───────────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────────┐
│                  Pinctrl 层 (引脚复用)                            │
│  将 GPIO 引脚配置为复用功能 (AF) 模式                             │
│  STM32: MODER 寄存器 01=GPIO, 10=AF, AFR 寄存器选择 AF 编号      │
└──────────────────────────────────────────────────────────────────┘
```

**核心概念**:

- **pwm_dt_spec**: 从设备树解析出的 PWM 描述符 (设备 + 通道 + 周期 + 极性)
- **周期 (period)**: PWM 信号一个完整周期的时长 (纳秒)
- **脉宽 (pulse)**: 高电平持续的时长 (纳秒)
- **占空比 (duty cycle)**: pulse / period × 100%
- **pinctrl**: 引脚控制器，负责在 GPIO 模式和复用功能 (AF) 模式之间切换

**STM32 上的数据流**:

```
应用调用 pwm_set_pulse_dt(&pwm_led0, pulse)
    │
    ▼
PWM 核心: pwm_set(dev, channel, period, pulse, flags)
    │
    ▼
STM32 PWM 驱动: pwm_stm32_set_cycles()
    │  将 period/pulse 从纳秒转换为定时器计数值
    │  设置 TIMx_ARR (周期) 和 TIMx_CCR (脉宽)
    ▼
硬件: TIM3 定时器 → AF2 复用 → PA6 引脚输出 PWM 波形
```

---

## 2. 设备树配置 (DTS)

### 2.1 timers 节点 (SoC dtsi 中预定义)

STM32 的定时器在 SoC 的 `.dtsi` 文件中已有基础定义:

```dts
/* zephyr/dts/arm/st/f4/stm32f4.dtsi 中 (不需要自己写) */
timers3: timers@40000400 {
    compatible = "st,stm32-timers";
    reg = <0x40000400 0x400>;
    clocks = <&rcc STM32_CLOCK(APB1, 1)>,
             <&rcc STM32_SRC_TIMPCLK1 NO_SEL>;
    resets = <&rctl STM32_RESET(APB1, 1)>;
    interrupts = <29 0>;
    st,prescaler = <0>;              /* 预分频器，默认 0 (不分频) */
    status = "disabled";

    pwm {
        compatible = "st,stm32-pwm";
        status = "disabled";
        #pwm-cells = <3>;           /* 3 个参数: channel, period, flags */
    };
};
```

### 2.2 板卡 DTS 覆盖 (需要自己写)

在板卡 DTS 中启用定时器并配置 PWM:

```dts
&timers3 {
    status = "okay";                 /* 启用 TIM3 */
    st,prescaler = <83>;             /* 预分频器值，写入 TIMx_PSC 寄存器 */

    pwm3: pwm {                      /* 标签 pwm3，供 &pwm3 引用 */
        status = "okay";             /* 启用 PWM 子节点 */
        pinctrl-0 = <&tim3_ch1_pa6   /* TIM3_CH1 → PA6 (AF2) */
                     &tim3_ch2_pa7>; /* TIM3_CH2 → PA7 (AF2) */
        pinctrl-names = "default";   /* pinctrl 状态名 */
    };
};
```

#### 关键属性说明

| 属性 | 说明 | 示例 |
|------|------|------|
| `st,prescaler` | 预分频器值，实际分频 = prescaler + 1 | `<83>` → 分频 84，84MHz/84 = 1MHz |
| `pinctrl-0` | 默认引脚配置，可包含多个引脚 | `<&tim3_ch1_pa6 &tim3_ch2_pa7>` |
| `pinctrl-names` | pinctrl 状态名称 | `"default"` |
| `#pwm-cells` | `pwms` 属性的参数个数，固定为 3 | `<3>` |

### 2.3 pwm-leds 节点 (应用层)

`pwm-leds` 是 PWM 控制 LED 的标准绑定，类似于 `gpio-leds`:

```dts
pwmleds: pwmleds {                   /* 标签 pwmleds，供 &pwmleds 引用 */
    compatible = "pwm-leds";

    pwm_led0: pwm_led_0 {            /* 标签 pwm_led0 */
        pwms = <&pwm3 1 PWM_MSEC(20) PWM_POLARITY_NORMAL>;
        /*  &pwm3:               引用 timers3 的 pwm 子节点 */
        /*  1:                   TIM3 通道 1 → PA6 */
        /*  PWM_MSEC(20):        周期 20ms = 50Hz */
        /*  PWM_POLARITY_NORMAL: 正常极性 (高电平有效) */
    };

    pwm_led1: pwm_led_1 {
        pwms = <&pwm3 2 PWM_MSEC(20) PWM_POLARITY_NORMAL>;
        /*  2: TIM3 通道 2 → PA7 */
    };
};
```

#### pwms 属性的 3 个参数

| 参数 | 说明 | 示例 |
|------|------|------|
| 第 1 个: phandle | PWM 设备引用 | `&pwm3` |
| 第 2 个: channel | 定时器通道号 (1~4) | `1` = TIM3_CH1 |
| 第 3 个: period | 周期 (纳秒) | `PWM_MSEC(20)` = 20,000,000 ns |

#### 周期宏

| 宏 | 值 | 用途 |
|------|------|------|
| `PWM_HZ(n)` | 1,000,000,000 / n | 按频率定义周期 |
| `PWM_MSEC(n)` | n × 1,000,000 | 按毫秒定义周期 |
| `PWM_USEC(n)` | n × 1,000 | 按微秒定义周期 |
| `PWM_NSEC(n)` | n | 按纳秒定义周期 |
| `PWM_SEC(n)` | n × 1,000,000,000 | 按秒定义周期 |

### 2.4 aliases 别名

```dts
aliases {
    pwm-led0 = &pwm_led0;    /* C 代码: DT_ALIAS(pwm_led0) */
    pwm-led1 = &pwm_led1;    /* C 代码: DT_ALIAS(pwm_led1) */
};
```

**注意**: 别名只能用连字符 `-`，C 代码中必须替换为下划线 `_`。详见 [第 8 章](#8-dts-别名规则详解)。

### 2.5 完整 DTS 配置关系图

```
SoC dtsi (预定义)                    板卡 DTS (自己写)
─────────────────                    ────────────────
timers3: timers@40000400 {           &timers3 {
    compatible = "st,stm32-timers";      status = "okay";
    st,prescaler = <0>;  ←────────────── st,prescaler = <83>;  /* 覆盖 */
    status = "disabled";
                                        pwm3: pwm {
    pwm {                                    status = "okay";
        compatible = "st,stm32-pwm";         pinctrl-0 = <&tim3_ch1_pa6
        #pwm-cells = <3>;                                &tim3_ch2_pa7>;
        status = "disabled";                 pinctrl-names = "default";
    }                                    };
};                                      };

应用层 DTS (自己写)
───────────────────
pwmleds: pwmleds {
    compatible = "pwm-leds";
    pwm_led0: pwm_led_0 {
        pwms = <&pwm3 1 PWM_MSEC(20) PWM_POLARITY_NORMAL>;
    };         │     │  │            │
    │          │     │  │            └── 极性
    │          │     │  └── 周期 20ms
    │          │     └── 通道 1
    │          └── 引用 pwm3 标签
    └── 标签 pwm_led0
};

aliases {
    pwm-led0 = &pwm_led0;   ← C 代码用 DT_ALIAS(pwm_led0) 引用
};
```

---

## 3. Kconfig 配置 (prj.conf)

```ini
CONFIG_PWM=y                          /* PWM 子系统总开关 */
```

### 说明

- `CONFIG_PWM=y` 是唯一需要手动配置的选项
- `CONFIG_INPUT_GPIO_KEYS` 等驱动选项在 DTS 中存在对应 compatible 节点时自动启用
- STM32 PWM 驱动 (`st,stm32-pwm`) 在 DTS 中 `status = "okay"` 时自动编译

### 可选调试配置

```ini
CONFIG_PWM_LOG_LEVEL_DBG=y            /* PWM 驱动调试日志 */
CONFIG_LOG=y                          /* 日志系统总开关 (PWM_LOG 依赖) */
```

---

## 4. C 代码 API

### 4.1 头文件

```c
#include <zephyr/drivers/pwm.h>       /* PWM API */
#include <zephyr/drivers/pinctrl.h>   /* pinctrl API (动态切换引脚模式时需要) */
```

### 4.2 pwm_dt_spec — PWM 设备描述符

```c
struct pwm_dt_spec {
    const struct device *dev;    /* PWM 设备实例 */
    uint32_t channel;            /* 通道号 (从 DTS pwms 属性解析) */
    uint32_t period;             /* 周期 (纳秒，从 DTS pwms 属性解析) */
    pwm_flags_t flags;           /* 极性等标志 (从 DTS pwms 属性解析) */
};
```

从设备树获取:

```c
static const struct pwm_dt_spec pwm_led0 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
/* 解析后:
 *   dev     = DEVICE_DT_GET(DT_ALIAS(pwm_led0)) 指向的设备
 *   channel = 1  (TIM3_CH1)
 *   period  = 20,000,000  (20ms = 20,000,000 ns)
 *   flags   = PWM_POLARITY_NORMAL
 */
```

### 4.3 初始化与就绪检查

```c
/* 检查 PWM 设备是否就绪 */
if (!pwm_is_ready_dt(&pwm_led0)) {
    printk("PWM device not ready\n");
    return 0;
}
```

`pwm_is_ready_dt()` 检查 `pwm_dt_spec.dev` 指向的设备是否已成功初始化。PWM 驱动在 `pwm_stm32_init()` 中会:
1. 启用定时器时钟
2. 应用 pinctrl 配置 (将引脚切换到 AF 模式)
3. 配置预分频器
4. 启动定时器计数

### 4.4 设置 PWM 输出

#### 方法 1: pwm_set_pulse_dt (最常用)

```c
int pwm_set_pulse_dt(const struct pwm_dt_spec *spec, uint32_t pulse);
```

- 使用 DTS 中定义的周期，只设置脉宽
- `pulse` 单位: 纳秒
- 返回: 0 成功，负数错误码

```c
/* 呼吸灯: 占空比从 0% 到 100% */
uint32_t pulse = pwm_led0.period * pwm_step / 100;
pwm_set_pulse_dt(&pwm_led0, pulse);
```

#### 方法 2: pwm_set_dt (同时设置周期和脉宽)

```c
int pwm_set_dt(const struct pwm_dt_spec *spec, uint32_t period, uint32_t pulse);
```

- 可以动态修改周期和脉宽
- 适用于需要改变频率的场景

#### 方法 3: pwm_set (最底层)

```c
int pwm_set(const struct device *dev, uint32_t channel,
            uint32_t period, uint32_t pulse, pwm_flags_t flags);
```

- 完全手动指定所有参数
- 不依赖 `pwm_dt_spec`

#### 方法 4: pwm_set_cycles (直接操作计数器)

```c
int pwm_set_cycles(const struct device *dev, uint32_t channel,
                   uint32_t period, uint32_t pulse, pwm_flags_t flags);
```

- 参数单位是定时器计数值，不是纳秒
- 避免了纳秒到计数值的转换
- 适用于需要精确控制计数器的场景

### 4.5 周期与频率查询

```c
/* 获取定时器每秒的计数周期数 */
uint64_t cycles_per_sec;
pwm_get_cycles_per_sec(pwm_led0.dev, pwm_led0.channel, &cycles_per_sec);

/* 计算实际频率 */
uint32_t freq = (uint32_t)(cycles_per_sec / pwm_led0.period);
```

### 4.6 极性标志

| 标志 | 说明 |
|------|------|
| `PWM_POLARITY_NORMAL` | 正常极性: 脉宽期间为高电平 |
| `PWM_POLARITY_INVERTED` | 反转极性: 脉宽期间为低电平 |

---

## 5. Pinctrl 引脚复用与 GPIO/PWM 动态切换

### 5.1 STM32 引脚模式

STM32 的每个 GPIO 引脚有多种工作模式，由 MODER 寄存器控制:

| MODER[1:0] | 模式 | 说明 |
|-------------|------|------|
| 00 | 输入 | GPIO 输入 (复位默认) |
| 01 | 通用输出 | GPIO 输出模式 |
| 10 | 复用功能 | AF 模式 (PWM/SPI/UART 等) |
| 11 | 模拟 | ADC/DAC |

PA6 的两种用途:

```
GPIO 模式 (MODER=01):  gpio_pin_configure_dt() 设置 → 通用输出，控制 LED 亮灭
AF2 模式 (MODER=10):   pinctrl_apply_state() 设置 → TIM3_CH1，输出 PWM 波形
```

AF 编号由 AFR 寄存器决定，PA6 的 AF2 = TIM3_CH1，这在 STM32 参考手册的引脚复用表中有定义。

### 5.2 Pinctrl 在 PWM 驱动中的自动应用

当 PWM 驱动初始化时 (`pwm_stm32_init`)，会自动调用:

```c
/* drivers/pwm/pwm_stm32.c 第 710 行 */
r = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
```

这意味着: **只要 DTS 中正确配置了 pinctrl-0，PWM 驱动初始化时就会自动把引脚切到 AF 模式**，不需要手动调用 pinctrl。

### 5.3 动态切换 GPIO ↔ PWM

当同一个引脚需要在 GPIO 模式和 PWM 模式之间切换时 (例如: 按键切换 LED 模式):

#### 切换到 PWM 模式

```c
PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3));   /* 文件顶层: 编译期定义 pinctrl 配置数据 */

/* 运行时: 应用 PWM 的 pinctrl 配置 */
gpio_pin_set_dt(&led, 0);                 /* 先关闭 GPIO 输出 */
pinctrl_apply_state(
    PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3)),
    PINCTRL_STATE_DEFAULT
);                                        /* PA6 → AF2 模式 */
```

#### 切换回 GPIO 模式

```c
pwm_set_pulse_dt(&pwm_led0, 0);          /* 先停止 PWM 输出 */
gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);  /* PA6 → GPIO 输出模式 */
```

`gpio_pin_configure_dt()` 内部会将 MODER 寄存器设回 01 (通用输出)，覆盖 pinctrl 设置的 AF 模式。

### 5.4 PINCTRL_DT_DEFINE 详解

```c
/* 编译期宏: 定义 pinctrl 配置数据结构 (必须在文件顶层使用!) */
PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3));

/* 运行时: 获取预定义的 pinctrl 配置 */
PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3));

/* 运行时: 应用 pinctrl 配置 */
pinctrl_apply_state(config, PINCTRL_STATE_DEFAULT);
```

**为什么 PINCTRL_DT_DEFINE 不能放在函数体内?**

`PINCTRL_DT_DEFINE` 展开后是 `static const struct pinctrl_dev_config ...` 的定义——这是文件作用域的声明，C 语法不允许在函数体内定义 `static` 全局变量。

### 5.5 pinctrl_apply_state 的副作用

**关键**: `pinctrl_apply_state` 会应用 PWM 节点的**全部** pinctrl 配置。

```dts
pinctrl-0 = <&tim3_ch1_pa6 &tim3_ch2_pa7>;
/*              ↑ PA6           ↑ PA7     */
```

调用 `pinctrl_apply_state` 后，**PA6 和 PA7 都被切换到 AF2 模式**。如果 PA7 同时被用作 GPIO LED，它将脱离 GPIO 控制。

**修复**: 在 apply 之后，把不需要 AF 模式的引脚重新配回 GPIO:

```c
pinctrl_apply_state(...);                        /* PA6→AF2, PA7→AF2 (副作用) */
gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE); /* PA7→GPIO (恢复) */
```

---

## 6. STM32 定时器与预分频器

### 6.1 时钟树

```
STM32F407 时钟树 (与 TIM3 相关):
  HSE (8MHz) → PLL → SYSCLK (168MHz)
                      → AHB (/1) = 168MHz
                         → APB1 (/4) = 42MHz
                            → 定时器时钟 = 42MHz × 2 = 84MHz
                               (APB1 预分频 ≠ 1 时，定时器时钟自动 ×2)
```

### 6.2 预分频器计算

`st,prescaler` 属性的值写入 TIMx_PSC 寄存器，实际分频系数 = prescaler + 1。

```
计数频率 = 定时器时钟 / (prescaler + 1)

示例:
  prescaler = 0:   计数频率 = 84 MHz / 1   = 84 MHz
  prescaler = 83:  计数频率 = 84 MHz / 84  = 1 MHz
  prescaler = 839: 计数频率 = 84 MHz / 840 = 100 kHz
```

### 6.3 16 位定时器限制

STM32F4 的 TIM3 是 **16 位定时器**，最大计数值 = 65,535 (0xFFFF)。

```
周期计数值 = 计数频率 × 周期(秒)

prescaler = 0 (84 MHz):
  20ms 需要 = 84,000,000 × 0.02 = 1,680,000 计数
  1,680,000 > 65,535 → ❌ 溢出! 驱动返回 -ENOTSUP

prescaler = 83 (1 MHz):
  20ms 需要 = 1,000,000 × 0.02 = 20,000 计数
  20,000 < 65,535 → ✅ 正常工作

prescaler = 839 (100 kHz):
  20ms 需要 = 100,000 × 0.02 = 2,000 计数
  2,000 < 65,535 → ✅ 正常工作，但分辨率降低
```

### 6.4 如何选择预分频器

原则: **在满足周期要求的前提下，选择最小的分频，获得最高的占空比分辨率。**

```
占空比分辨率 = 1 / (计数频率 × 周期)

prescaler = 83  (1 MHz):  分辨率 = 1 / 20,000 = 0.005%  ← 推荐
prescaler = 839 (100 kHz): 分辨率 = 1 / 2,000  = 0.05%
```

### 6.5 STM32F4 定时器位数

| 定时器 | 位数 | 说明 |
|--------|------|------|
| TIM1, TIM8 | 16 位 | 高级定时器 |
| TIM2, TIM5 | **32 位** | 通用定时器，最大计数 4,294,967,295 |
| TIM3, TIM4 | 16 位 | 通用定时器 |
| TIM6, TIM7 | 16 位 | 基本定时器 |
| TIM9~TIM14 | 16 位 | 通用定时器 |

如果需要长周期 + 高分辨率，优先使用 TIM2 或 TIM5 (32 位)。

---

## 7. 踩坑记录

### 坑 1: DT_ALIAS(pwm-led0) 编译失败

**现象**:
```
error: 'led0_P_pwms_IDX_0_VAL_channel' undeclared here (not in a function)
```

**根因**: C 代码中 `DT_ALIAS()` 的参数是 C 标识符，不允许连字符 `-`。预处理器将 `pwm-led0` 解析为 `pwm - led0` (减法运算)。

**修复**: 别名中的连字符在 C 代码中替换为下划线:
```c
/* 错误 */
PWM_DT_SPEC_GET(DT_ALIAS(pwm-led0))

/* 正确 */
PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0))
```

### 坑 2: 别名中使用下划线被 DTS 编译器拒绝

**现象**:
```
devicetree error: /aliases: alias property name 'pwm_led0'
                  should include only characters from [0-9a-z-]
```

**根因**: Zephyr 构建系统强制规定别名只能用 `[0-9a-z-]`，禁止下划线。这是为了消除歧义——构建系统自动将连字符转为下划线生成 C 宏名，如果允许下划线，`pwm-led0` 和 `pwm_led0` 会生成同一个宏名。

**规则**:
```
DTS 别名:  只能用连字符 -    →  pwm-led0
C 代码:    只能用下划线 _    →  DT_ALIAS(pwm_led0)
构建系统:  自动 - → _ 转换   →  DT_N_ALIAS_pwm_led0
```

### 坑 3: & 引用路径语法报错

**现象**:
```
devicetree error: parse error: expected ';' or ','
```

**根因**: `&` 是 phandle 引用操作符，只能引用标签 (label)，不支持路径语法:
```dts
/* 错误 — & 不支持路径语法 */
pwm-led1 = &pwmleds/pwm_led_1;

/* 正确 — & 引用标签 */
pwm-led1 = &pwm_led1;
```

### 坑 4: PINCTRL_DT_DEFINE 放在函数体内

**现象**: 编译错误，`static` 声明不能出现在函数体内。

**根因**: `PINCTRL_DT_DEFINE` 展开后是 `static const struct ...` 的定义，C 语法要求文件作用域。

**修复**: 移到文件顶层 (函数外面):
```c
/* 文件顶层 — 正确 */
PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3));

int main(void) {
    /* 函数内只调用 apply */
    pinctrl_apply_state(
        PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3)),
        PINCTRL_STATE_DEFAULT
    );
}
```

### 坑 5: pinctrl_apply_state 误切 PA7 导致 main 线程挂掉

**现象**: 长按按键切换到 PWM 模式后，两个 LED 都熄灭，main 线程不再响应。

**根因**: `pinctrl_apply_state` 应用了 pwm3 的全部 pinctrl 配置:
```dts
pinctrl-0 = <&tim3_ch1_pa6 &tim3_ch2_pa7>;
```
PA7 也被切到 AF2 模式，脱离 GPIO 控制。之后 `gpio_pin_toggle_dt(&led1)` 操作处于 AF 模式的 PA7 返回错误，原代码 `return 0` 导致 main 线程直接退出。

**修复**: 在 apply 之后恢复 PA7 的 GPIO 控制:
```c
pinctrl_apply_state(...);                         /* PA6→AF2, PA7→AF2 */
gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);  /* PA7→GPIO 恢复 */
```

同时，错误处理应改为 `printk` 报错而非 `return 0` 退出 main。

### 坑 6: TIM3 预分频器未设置导致 pwm_set_pulse_dt 返回 -ENOTSUP

**现象**:
```
ERR: pwm set pulse failed
```

**根因**: DTS 中 `st,prescaler = <0>` (SoC 默认值)，TIM3 以 84 MHz 全速计数。20ms 周期需要 1,680,000 个计数，远超 16 位定时器的最大值 65,535。STM32 PWM 驱动在 `pwm_stm32_set_cycles()` 中检查到溢出后返回 `-ENOTSUP`。

**修复**: 设置合适的预分频器:
```dts
&timers3 {
    st,prescaler = <83>;   /* 84MHz / 84 = 1MHz, 20ms = 20,000 计数 < 65,535 */
};
```

**计算公式**:
```
prescaler = ceil(定时器时钟 × 目标周期 / 65536) - 1

例: prescaler = ceil(84,000,000 × 0.02 / 65,536) - 1 = ceil(25.6) - 1 = 25
    但 prescaler=25 → 计数频率=84MHz/26=3.23MHz → 20ms=64,615 计数 (接近上限)
    选 prescaler=83 → 计数频率=1MHz → 20ms=20,000 计数 (安全裕量)
```

### 坑 7: PWM 占空比太小看不见

**现象**: 切换到 PWM 模式后 LED 不亮或亮度极低。

**根因**: 原代码 `pwm_duty += 1` 的单位是纳秒:
```c
pwm_set_pulse_dt(&pwm_led0, 1);  /* 1ns / 20ms = 0.000005% ← 看不见! */
```

**修复**: 按百分比计算脉宽:
```c
uint32_t pulse = pwm_led0.period * pwm_step / 100;
pwm_set_pulse_dt(&pwm_led0, pulse);
```

### 坑 8: volatile 缺失导致跨线程变量不可见

**现象**: Input 回调中修改 `led0_mode`，但 main 线程看不到变化。

**根因**: `led0_mode` 在 Input 回调线程中修改，在 main 线程中读取。没有 `volatile`，编译器可能将值缓存在寄存器中，导致 main 线程永远读到旧值。

**修复**:
```c
volatile uint8_t led0_mode = 0;              /* 被回调修改，被 main 读取 */
static volatile uint8_t led0_statebak = 0;   /* 同理 */
```

---

## 8. DTS 别名规则详解

### 8.1 三种引用方式对比

| 引用方式 | DTS 定义 | C 代码 | 特点 |
|----------|----------|--------|------|
| 别名 (alias) | `aliases { pwm-led0 = &pwm_led0; }` | `DT_ALIAS(pwm_led0)` | 标准化，板卡间可移植 |
| 标签 (label) | `pwm_led0: pwm_led_0 { ... }` | `DT_NODELABEL(pwm_led0)` | 直接引用，不经过 aliases |
| 路径 (path) | 节点路径 `/pwmleds/pwm_led_0` | `DT_PATH(pwmleds, pwm_led_0)` | 最底层，不依赖标签或别名 |

三种方式最终得到同一个节点标识符。

### 8.2 别名转换规则

```
DTS 别名 (只能用 -)     构建系统自动转换      C 代码 (只能用 _)
─────────────────      ────────────────     ──────────────────
pwm-led0          →    DT_N_ALIAS_pwm_led0  ←  DT_ALIAS(pwm_led0)
volt-sensor0      →    DT_N_ALIAS_volt_sensor0 ← DT_ALIAS(volt_sensor0)
led0              →    DT_N_ALIAS_led0      ←  DT_ALIAS(led0)
```

### 8.3 节点名 vs 标签 vs 别名

以 `pwm_led0: pwm_led_0 { ... }` 为例:

| 概念 | 代码 | 位置 | 用途 |
|------|------|------|------|
| 节点名 | `pwm_led_0` | 冒号右边 | 设备树路径: `/pwmleds/pwm_led_0` |
| 标签 | `pwm_led0` | 冒号左边 | DTS 内 `&` 引用 + C 代码 `DT_NODELABEL()` |
| 别名 | `pwm-led0` | aliases 中定义 | C 代码 `DT_ALIAS()`，板卡间可移植 |

**关键规则**:
- DTS 中 `&` 只能引用标签，不能引用节点名或路径
- 别名只能用 `[0-9a-z-]`，C 代码中连字符换下划线
- 如果节点没有标签，就无法被 `&` 引用，必须先加标签

---

## 9. 配置速查表

### DTS 层

```
timers 节点:
  status = "okay";                  启用定时器
  st,prescaler = <83>;              预分频器 (实际分频 = 值 + 1)

pwm 子节点:
  status = "okay";                  启用 PWM
  pinctrl-0 = <&tim3_ch1_pa6 ...>;  引脚复用配置
  pinctrl-names = "default";        pinctrl 状态名

pwm-leds 子节点:
  pwms = <&pwm3 1 PWM_MSEC(20) PWM_POLARITY_NORMAL>;
        │     │  │            │
        │     │  │            └── 极性
        │     │  └── 周期 (纳秒)
        │     └── 通道号
        └── PWM 设备引用

aliases:
  pwm-led0 = &pwm_led0;            C 代码: DT_ALIAS(pwm_led0)
```

### Kconfig 层

```
CONFIG_PWM=y                        PWM 子系统总开关
CONFIG_PWM_LOG_LEVEL_DBG=y          调试日志
```

### C 代码层

```
PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0))    获取 PWM 描述符
pwm_is_ready_dt(&spec)                  检查设备就绪
pwm_set_pulse_dt(&spec, pulse_ns)       设置脉宽 (纳秒)
pwm_set_dt(&spec, period_ns, pulse_ns)  设置周期+脉宽
pwm_set(dev, ch, period, pulse, flags)  底层 API

PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3))            文件顶层定义 pinctrl 数据
PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3))    获取 pinctrl 配置
pinctrl_apply_state(config, PINCTRL_STATE_DEFAULT) 应用 pinctrl 配置

gpio_pin_configure_dt(&spec, GPIO_OUTPUT_ACTIVE)   引脚→GPIO 输出模式
```

---

## 10. 源码路径索引

| 文件 | 路径 | 说明 |
|------|------|------|
| STM32 PWM 驱动 | `drivers/pwm/pwm_stm32.c` | STM32 PWM 驱动实现，含 16 位溢出检查 |
| PWM API 头文件 | `include/zephyr/drivers/pwm.h` | pwm_set_pulse_dt 等内联函数 |
| PWM 绑定 | `dts/bindings/pwm/st,stm32-pwm.yaml` | STM32 PWM DTS 属性定义 |
| timers 绑定 | `dts/bindings/timer/st,stm32-timers.yaml` | STM32 定时器 DTS 属性定义 |
| pwm-leds 绑定 | `dts/bindings/led/pwm-leds.yaml` | PWM LED DTS 属性定义 |
| pinctrl API | `include/zephyr/drivers/pinctrl.h` | pinctrl_apply_state 等函数 |
| STM32 pinctrl | `drivers/pinctrl/pinctrl_stm32.c` | STM32 引脚复用实现 |
| STM32F4 dtsi | `dts/arm/st/f4/stm32f4.dtsi` | TIM3 等定时器基础定义 |
| STM32F4 pinctrl | `dts/arm/st/f4/stm32f407v(e-g)tx-pinctrl.dtsi` | tim3_ch1_pa6 等引脚定义 |
| 官方示例 | `samples/basic/blinky_pwm/` | PWM 呼吸灯示例 |
