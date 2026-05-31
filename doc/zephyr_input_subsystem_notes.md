# Zephyr Input 子系统学习笔记

> 硬件: STM32F407VET6 DevEBox (2 个 GPIO 按键: PE4, PE3)
> Zephyr: v4.4.0
> 目的: 从零理解 Input 子系统的架构、配置、事件流和常见陷阱

---

## 目录

1. [架构总览](#1-架构总览)
2. [设备树配置 (DTS)](#2-设备树配置-dts)
3. [Kconfig 配置 (prj.conf)](#3-kconfig配置-prjconf)
4. [C 代码 API](#4-c-代码-api)
5. [伪设备: longpress / double-tap / keymap](#5-伪设备-longpress--double-tap--keymap)
6. [踩坑记录](#6-踩坑记录)
7. [配置速查表](#7-配置速查表)
8. [事件流图解](#8-事件流图解)
9. [源码路径索引](#9-源码路径索引)

---

## 1. 架构总览

Zephyr Input 子系统借鉴了 Linux 内核 input 子系统的设计理念:

```
┌─────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  硬件驱动    │     │  Input 子系统核心  │     │  应用回调         │
│             │     │                  │     │                  │
│ gpio-keys   │────→│ input_report_key │────→│ INPUT_CALLBACK   │
│ 触摸屏      │     │ input_report_abs │     │ _DEFINE()        │
│ 编码器      │     │ input_report_rel │     │                  │
│ ADC按键     │     │       ...        │     │                  │
└─────────────┘     └──────────────────┘     └──────────────────┘
                            ↑
                     ┌──────┴──────┐
                     │  伪设备      │
                     │ (事件处理链)  │
                     │ longpress   │
                     │ double-tap  │
                     │ keymap      │
                     └─────────────┘
```

**核心概念**:

- **硬件驱动**: 直接操作硬件 (GPIO/ADC/SPI/I2C)，通过 `input_report_*()` 上报事件
- **Input 核心**: 管理事件分发，维护回调链表，支持同步/异步两种模式
- **伪设备**: 不对应硬件，消费上游事件后以自己的设备身份重新上报新事件
- **应用回调**: 通过 `INPUT_CALLBACK_DEFINE()` 注册，接收指定设备 (或所有设备) 的事件

**关键设计**: 每个驱动/伪设备都是独立的 `struct device`，事件从发出设备分发给监听该设备的回调。伪设备消费上游事件后，以**自己的设备身份**重新上报，因此下游回调必须监听伪设备才能收到转换后的事件。

---

## 2. 设备树配置 (DTS)

### 2.1 gpio-keys 节点

`gpio-keys` 是最常用的按键输入驱动，绑定文件: `dts/bindings/input/gpio-keys.yaml`

```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>

/ {
    gpio_keys: gpio_keys {                          /* 必须有 label! 否则 &gpio_keys 引用失败 */
        compatible = "gpio-keys";
        debounce-interval-ms = <50>;                /* 去抖间隔，默认 30ms */
        /* polling-mode; */                         /* 启用轮询模式(不使用中断) */
        /* no-disconnect; */                        /* 挂起时不断开 GPIO */

        user_button0: button0 {                     /* 子节点: 一个按键 */
            gpios = <&gpioe 4 GPIO_ACTIVE_LOW>;     /* 必需: GPIO 描述 */
            zephyr,code = <INPUT_KEY_0>;            /* 按键事件码 */
            label = "Key0";                         /* 可选: 描述性名称 */
        };

        user_button1: button1 {
            gpios = <&gpioe 3 GPIO_ACTIVE_LOW>;
            zephyr,code = <INPUT_KEY_1>;
        };
    };
};
```

#### 父节点属性

| 属性 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `debounce-interval-ms` | int | 30 | 去抖间隔 (ms)。中断模式: 延迟去抖时间; 轮询模式: 轮询周期 |
| `polling-mode` | bool | 无 | 不使用中断，改为按 debounce-interval-ms 周期轮询 |
| `no-disconnect` | bool | 无 | 挂起(suspend)时不断开 GPIO，用于不支持 GPIO_DISCONNECTED 的控制器 |

#### 子节点属性

| 属性 | 类型 | 必需 | 说明 |
|------|------|------|------|
| `gpios` | phandle-array | **是** | GPIO 描述 (控制器+引脚号+标志) |
| `zephyr,code` | int | 否 | 按键事件码，如 `INPUT_KEY_0` |
| `label` | string | 否 | 描述性名称 |

#### 设备层级关系 (重要!)

```
gpio_keys (父节点, compatible = "gpio-keys")
  ├── button0 (子节点)
  └── button1 (子节点)
```

**驱动只为父节点创建 `struct device`**。子节点只是配置数据 (引脚号、按键码)，不会生成独立的设备实例。

这意味着:
- `DEVICE_DT_GET(DT_ALIAS(bt0))` → **链接失败!** (bt0 指向子节点，无设备实例)
- `DEVICE_DT_GET(DT_PARENT(DT_ALIAS(bt0)))` → **正确!** (获取父节点设备)

所有按键事件都从同一个父设备发出，靠 `evt->code` 区分具体按键。

### 2.2 事件码

事件码定义在 `zephyr/dt-bindings/input/input-event-codes.h`，常用分类:

| 前缀 | 用途 | 示例 |
|------|------|------|
| `INPUT_KEY_*` | 键盘按键 | INPUT_KEY_0, INPUT_KEY_A, INPUT_KEY_ENTER |
| `INPUT_BTN_*` | 鼠标/游戏手柄按钮 | INPUT_BTN_LEFT, BTN_A |
| `INPUT_REL_*` | 相对位移 | INPUT_REL_X, INPUT_REL_Y (鼠标/编码器) |
| `INPUT_ABS_*` | 绝对坐标 | INPUT_ABS_X, INPUT_ABS_Y (触摸屏/摇杆) |
| `INPUT_MSC_*` | 杂项 | INPUT_MSC_SCAN |

事件类型 (`evt->type`):

| 类型 | 说明 |
|------|------|
| `INPUT_EV_KEY` | 按键事件 (按下/松开) |
| `INPUT_EV_REL` | 相对位移事件 |
| `INPUT_EV_ABS` | 绝对坐标事件 |
| `INPUT_EV_MSC` | 杂项事件 |

### 2.3 aliases 约定

```dts
aliases {
    led0 = &led0;           /* LED */
    led1 = &led1;
    bt0 = &user_button0;    /* 按键 (button) */
    bt1 = &user_button1;
    sw0 = &user_button0;    /* 按键 (switch，另一种命名) */
};
```

C 代码中通过 `DT_ALIAS(bt0)` 引用别名。

---

## 3. Kconfig 配置 (prj.conf)

### 3.1 核心配置

```ini
CONFIG_INPUT=y                          /* Input 子系统总开关 */
CONFIG_INPUT_GPIO_KEYS=y                /* GPIO 按键驱动 (DTS 有 gpio-keys 时默认 y) */
```

### 3.2 事件处理模式

```ini
/* 二选一，默认为线程模式 */
CONFIG_INPUT_MODE_THREAD=y              /* 异步: 事件通过消息队列在专用线程中处理 (默认) */
# CONFIG_INPUT_MODE_SYNCHRONOUS=y       /* 同步: 事件在 input_report_*() 调用上下文中处理 */
```

### 3.3 线程模式参数

```ini
CONFIG_INPUT_THREAD_STACK_SIZE=1024     /* Input 线程栈大小 (字节) */
CONFIG_INPUT_QUEUE_MAX_MSGS=16          /* 事件队列最大消息数 */
CONFIG_INPUT_THREAD_PRIORITY_OVERRIDE=y /* 覆盖默认线程优先级 */
CONFIG_INPUT_THREAD_PRIORITY=0          /* 自定义线程优先级 */
```

### 3.4 调试与工具

```ini
CONFIG_INPUT_EVENT_DUMP=y               /* 记录所有 input 事件到日志 (需 CONFIG_LOG) */
CONFIG_INPUT_SHELL=y                    /* 启用 input shell 命令 (需 CONFIG_SHELL) */
```

`CONFIG_INPUT_EVENT_DUMP` 是调试利器，启用后在串口可以看到所有 input 事件的原始数据，无需在代码中加 printk。

### 3.5 伪设备配置

```ini
/* 以下选项在 DTS 中定义对应 compatible 节点时自动启用 (default y) */
CONFIG_INPUT_LONGPRESS=y                /* 长按检测 (依赖 DT_HAS_ZEPHYR_INPUT_LONGPRESS_ENABLED) */
CONFIG_INPUT_DOUBLE_TAP=y               /* 双击检测 (依赖 DT_HAS_ZEPHYR_INPUT_DOUBLE_TAP_ENABLED) */
CONFIG_INPUT_KEYMAP=y                   /* 键盘矩阵映射 (依赖 DT_HAS_INPUT_KEYMAP_ENABLED) */
```

---

## 4. C 代码 API

### 4.1 INPUT_CALLBACK_DEFINE — 注册事件回调

```c
INPUT_CALLBACK_DEFINE(dev, callback, user_data);
```

| 参数 | 说明 |
|------|------|
| `dev` | 要监听的设备，`NULL` 表示监听所有设备 |
| `callback` | 回调函数，签名为 `void cb(struct input_event *evt, void *user_data)` |
| `user_data` | 传递给回调的用户数据 |

### 4.2 struct input_event

```c
struct input_event {
    uint8_t  type;    /* 事件类型: INPUT_EV_KEY, INPUT_EV_REL, INPUT_EV_ABS ... */
    uint16_t code;    /* 事件码: INPUT_KEY_0, INPUT_BTN_LEFT ... */
    int32_t  value;   /* 事件值: 按键 1=按下/0=松开, 坐标=绝对值, 位移=相对值 */
    uint8_t  sync;    /* 同步标志: 1=一组事件结束, 0=还有后续事件 */
    uint64_t timestamp;/* 时间戳 */
};
```

### 4.3 三种监听方式

#### 方式 1: 全局监听 (NULL)

```c
static void all_input_cb(struct input_event *evt, void *user_data)
{
    if (evt->sync == 0) return;     /* 等待 sync=1 的一组完整事件 */

    printk("code=%d value=%d\n", evt->code, evt->value);
}
INPUT_CALLBACK_DEFINE(NULL, all_input_cb, NULL);
```

- 监听所有 input 设备的所有事件
- 适合系统只有一个 input 设备的简单场景
- 多设备时需要自行过滤

#### 方式 2: 指定设备监听 (DT_PARENT)

```c
static void gpio_keys_cb(struct input_event *evt, void *user_data)
{
    if (evt->sync == 0) return;

    switch (evt->code) {
    case INPUT_KEY_0: /* 按键0 */ break;
    case INPUT_KEY_1: /* 按键1 */ break;
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(bt0))), gpio_keys_cb, NULL);
```

- 只监听指定设备的事件
- `DT_PARENT(DT_ALIAS(bt0))` 获取父节点 (因为子节点没有设备实例)
- 区分按键靠 `evt->code`

#### 方式 3: 监听伪设备

```c
static void longpress_cb(struct input_event *evt, void *user_data)
{
    if (evt->sync == 0) return;

    switch (evt->code) {
    case INPUT_KEY_X: /* 长按按键0 */ break;
    case INPUT_KEY_A: /* 短按按键0 */ break;
    }
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(longpress)), longpress_cb, NULL);
```

- 监听伪设备 (如 longpress) 发出的事件
- 伪设备是独立的 `struct device`，必须单独注册回调
- `DT_NODELABEL()` 通过 DTS 标签引用节点

### 4.4 DT_NODELABEL vs DT_ALIAS vs DT_PARENT

| 宏 | 用途 | 示例 |
|------|------|------|
| `DT_NODELABEL(label)` | 通过 DTS 标签引用节点 | `DT_NODELABEL(longpress)` → `longpress: longpress { ... }` |
| `DT_ALIAS(alias)` | 通过 aliases 引用节点 | `DT_ALIAS(bt0)` → `aliases { bt0 = &user_button0; }` |
| `DT_PARENT(node_id)` | 获取父节点 | `DT_PARENT(DT_ALIAS(bt0))` → gpio_keys 父节点 |

---

## 5. 伪设备: longpress / double-tap / keymap

伪设备是 Input 子系统的精华——**纯 DTS 配置，零 C 代码**即可实现复杂的事件处理。

### 5.1 longpress — 长按/短按检测

绑定文件: `dts/bindings/input/zephyr,input-longpress.yaml`

```dts
longpress: longpress {
    compatible = "zephyr,input-longpress";
    input = <&gpio_keys>;                          /* 监听哪个设备，不填则监听所有 */
    input-codes = <INPUT_KEY_0>, <INPUT_KEY_1>;    /* 要检测的按键码 */
    short-codes = <INPUT_KEY_A>, <INPUT_KEY_B>;    /* 短按时输出的码 */
    long-codes = <INPUT_KEY_X>, <INPUT_KEY_Y>;     /* 长按时输出的码 */
    long-delay-ms = <2000>;                         /* 超过此时间算长按 */
};
```

| 属性 | 类型 | 必需 | 说明 |
|------|------|------|------|
| `input` | phandle | 否 | 输入设备句柄，不填则监听所有设备 |
| `input-codes` | array | **是** | 要检测的输入事件码 |
| `short-codes` | array | 否 | 短按时输出的码 (不填则短按不产生事件) |
| `long-codes` | array | **是** | 长按时输出的码 |
| `long-delay-ms` | int | **是** | 长按判定时间 (ms) |

**行为**:

- 按键按下: 启动定时器
- 按键松开 (未超时): 短按 → 上报 `short-codes` 的 pressed + released
- 定时器到期 (仍按住): 长按 → 上报 `long-codes` 的 pressed
- 按键松开 (已长按): 上报 `long-codes` 的 released

### 5.2 double-tap — 双击检测

绑定文件: `dts/bindings/input/zephyr,input-double-tap.yaml`

```dts
double_tap: double_tap {
    compatible = "zephyr,input-double-tap";
    input = <&gpio_keys>;
    input-codes = <INPUT_KEY_0>, <INPUT_KEY_1>;
    double-tap-codes = <INPUT_KEY_C>, <INPUT_KEY_D>;   /* 双击时输出的码 */
    double-tap-delay-ms = <300>;                         /* 两次点击间隔阈值 */
};
```

| 属性 | 类型 | 必需 | 说明 |
|------|------|------|------|
| `input` | phandle | 否 | 输入设备句柄 |
| `input-codes` | array | **是** | 要检测的输入事件码 |
| `double-tap-codes` | array | **是** | 双击时输出的码 |
| `double-tap-delay-ms` | int | **是** | 双击判定时间 (ms) |

### 5.3 keymap — 键盘矩阵映射

绑定文件: `dts/bindings/input/input-keymap.yaml`

```dts
keymap {
    compatible = "input-keymap";
    keymap = <
        MATRIX_KEY(0, 0, INPUT_KEY_1)
        MATRIX_KEY(0, 1, INPUT_KEY_2)
        MATRIX_KEY(1, 0, INPUT_KEY_4)
        MATRIX_KEY(1, 1, INPUT_KEY_5)
    >;
    row-size = <2>;
    col-size = <2>;
};
```

用于矩阵键盘，将行列坐标映射为按键码。`MATRIX_KEY(row, col, code)` 宏定义映射关系。

### 5.4 伪设备链式组合

伪设备可以链式组合，实现更复杂的功能:

```dts
/* 示例: 按键 → longpress → double-tap (理论可行) */
longpress: longpress {
    compatible = "zephyr,input-longpress";
    input = <&gpio_keys>;
    input-codes = <INPUT_KEY_0>;
    short-codes = <INPUT_KEY_A>;
    long-codes = <INPUT_KEY_X>;
    long-delay-ms = <1000>;
};

double_tap: double_tap {
    compatible = "zephyr,input-double-tap";
    input = <&longpress>;                     /* 监听 longpress 的输出! */
    input-codes = <INPUT_KEY_A>;              /* 检测短按的输出 */
    double-tap-codes = <INPUT_KEY_C>;         /* 双击短按 → INPUT_KEY_C */
    double-tap-delay-ms = <300>;
};
```

---

## 6. 踩坑记录

### 坑 1: `DEVICE_DT_GET(DT_ALIAS(bt0))` 链接失败

**现象**:
```
undefined reference to `__device_dts_ord_38'
```

**根因**: `bt0` 别名指向 `gpio_keys` 的子节点 `button0`，而驱动只为**父节点**创建设备实例，子节点没有设备。

**修复**: 使用 `DT_PARENT()` 获取父节点:
```c
/* 错误 */
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_ALIAS(bt0)), cb, NULL);

/* 正确 */
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(bt0))), cb, NULL);
```

### 坑 2: `&gpio_keys` 报 undefined node label

**现象**:
```
devicetree error: /longpress: undefined node label 'gpio_keys'
```

**根因**: DTS 节点定义时没有加标签 (label):
```dts
gpio_keys { ... }         /* 只有 node-name，没有 label */
```
`&gpio_keys` 引用的是标签，不是节点名称。

**修复**: 加上标签:
```dts
gpio_keys: gpio_keys { ... }   /* label: node-name */
```

**规则**: 任何需要被 `&` 引用的节点，都必须有 `label:` 标签。建议养成习惯: 所有节点都写上标签。

### 坑 3: longpress 事件收不到

**现象**: 长按按键后，`INPUT_KEY_X` 的回调没有触发。

**根因**: longpress 伪设备是独立的 `struct device`，它消费 `gpio_keys` 的原始事件后，以**自己的设备身份**重新上报新事件。如果回调只监听 `gpio_keys`，就收不到 `longpress` 发出的事件。

**修复**: 为 longpress 设备单独注册回调:
```c
/* 监听原始按键 */
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(bt0))), gpio_keys_cb, NULL);

/* 监听长按事件 — 必须单独注册! */
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(longpress)), longpress_cb, NULL);
```

### 坑 4: 一个按钮一个父节点？

**想法**: 把每个按钮放在独立的 `gpio-keys` 父节点中，这样 `DEVICE_DT_GET(DT_ALIAS(bt0))` 就能工作。

**技术上可行**: 驱动通过 `DT_INST_FOREACH_STATUS_OKAY` 遍历所有 `gpio-keys` 节点，每个都会创建设备实例。

**但不推荐**: 搜索 Zephyr 全部 463 个使用 `gpio-keys` 的板卡 DTS，462 个将所有按键放在同一父节点下，仅 1 个 (Wio Terminal) 拆分为两个父节点 (因为需要不同的 polling-mode 配置)。拆分会导致:
- 资源浪费 (每个父节点一套驱动数据结构)
- 违背社区惯例
- 没有必要 (`evt->code` 已足够区分按键)

---

## 7. 配置速查表

### DTS 层

```
gpio-keys 父节点:
  debounce-interval-ms = <30>;     去抖时间 (默认 30ms)
  polling-mode;                     轮询模式 (不使用中断)
  no-disconnect;                    挂起时不断开 GPIO

gpio-keys 子节点:
  gpios = <&port pin flags>;       GPIO 描述 (必需)
  zephyr,code = <INPUT_KEY_x>;     按键事件码

longpress:
  input = <&device>;               输入设备 (可选)
  input-codes = <...>;             检测的按键码 (必需)
  short-codes = <...>;             短按输出码 (可选)
  long-codes = <...>;              长按输出码 (必需)
  long-delay-ms = <1000>;          长按阈值 (必需)

double-tap:
  input = <&device>;               输入设备 (可选)
  input-codes = <...>;             检测的按键码 (必需)
  double-tap-codes = <...>;        双击输出码 (必需)
  double-tap-delay-ms = <300>;     双击阈值 (必需)
```

### Kconfig 层

```
CONFIG_INPUT=y                     总开关
CONFIG_INPUT_GPIO_KEYS=y           GPIO 按键驱动 (自动)
CONFIG_INPUT_MODE_THREAD=y         异步模式 (默认)
CONFIG_INPUT_MODE_SYNCHRONOUS=y    同步模式
CONFIG_INPUT_THREAD_STACK_SIZE=1024 线程栈大小
CONFIG_INPUT_QUEUE_MAX_MSGS=16     队列深度
CONFIG_INPUT_EVENT_DUMP=y          调试日志
CONFIG_INPUT_SHELL=y               Shell 命令
CONFIG_INPUT_LONGPRESS=y           长按检测 (自动)
CONFIG_INPUT_DOUBLE_TAP=y          双击检测 (自动)
CONFIG_INPUT_KEYMAP=y              矩阵映射 (自动)
```

### C 代码层

```
INPUT_CALLBACK_DEFINE(NULL, cb, data)              监听所有设备
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(node), cb, data) 监听指定设备

DT_ALIAS(name)           通过别名引用节点
DT_NODELABEL(name)       通过标签引用节点
DT_PARENT(node_id)       获取父节点

evt->type                事件类型 (INPUT_EV_KEY ...)
evt->code                事件码 (INPUT_KEY_0 ...)
evt->value               事件值 (1=按下, 0=松开)
evt->sync                同步标志 (1=一组事件结束)
```

---

## 8. 事件流图解

### 简单场景: 只有 gpio-keys

```
按键按下 (PE4)
    │
    ▼
gpio_keys 驱动 (GPIO 中断 + 去抖)
    │
    │ input_report_key(gpio_keys_dev, INPUT_KEY_0, 1)
    ▼
Input 核心 (事件分发)
    │
    ├──→ 回调 (监听 gpio_keys_dev): 收到 INPUT_KEY_0, value=1
    └──→ 回调 (监听 NULL): 收到 INPUT_KEY_0, value=1
```

### 完整场景: gpio-keys + longpress

```
按键按下 (PE4)
    │
    ▼
gpio_keys 驱动
    │ input_report_key(gpio_keys_dev, INPUT_KEY_0, 1)
    ▼
Input 核心 ──→ 回调 1 (监听 gpio_keys_dev): [GPIO-KEYS] Button 0 pressed
    │
    ▼
longpress 伪设备 (内部回调监听 gpio_keys_dev)
    │ 按住超过 2 秒
    │ input_report_key(longpress_dev, INPUT_KEY_X, 1)
    ▼
Input 核心 ──→ 回调 2 (监听 longpress_dev): [LONGPRESS] Button 0 long press started
    │
    │ 按键松开
    │ input_report_key(longpress_dev, INPUT_KEY_X, 0)
    ▼
Input 核心 ──→ 回调 2 (监听 longpress_dev): [LONGPRESS] Button 0 long press ended
```

### 短按场景

```
按键按下 → 按键松开 (未超时)
    │
    ▼
gpio_keys 驱动
    │ input_report_key(gpio_keys_dev, INPUT_KEY_0, 1)
    │ input_report_key(gpio_keys_dev, INPUT_KEY_0, 0)
    ▼
Input 核心 ──→ 回调 1: [GPIO-KEYS] Button 0 pressed / released
    │
    ▼
longpress 伪设备
    │ 取消定时器 (未超时), 判定为短按
    │ input_report_key(longpress_dev, INPUT_KEY_A, 1)
    │ input_report_key(longpress_dev, INPUT_KEY_A, 0)
    ▼
Input 核心 ──→ 回调 2: [LONGPRESS] Button 0 short press
```

---

## 9. 源码路径索引

| 文件 | 路径 | 说明 |
|------|------|------|
| gpio-keys 驱动 | `drivers/input/input_gpio_keys.c` | GPIO 按键驱动实现 |
| gpio-keys 绑定 | `dts/bindings/input/gpio-keys.yaml` | DTS 属性定义 |
| gpio-keys Kconfig | `drivers/input/Kconfig.gpio_keys` | 驱动配置选项 |
| longpress 实现 | `subsys/input/input_longpress.c` | 长按检测伪设备 |
| longpress 绑定 | `dts/bindings/input/zephyr,input-longpress.yaml` | DTS 属性定义 |
| double-tap 实现 | `subsys/input/input_double_tap.c` | 双击检测伪设备 |
| double-tap 绑定 | `dts/bindings/input/zephyr,input-double-tap.yaml` | DTS 属性定义 |
| keymap 实现 | `subsys/input/input_keymap.c` | 键盘矩阵映射 |
| keymap 绑定 | `dts/bindings/input/input-keymap.yaml` | DTS 属性定义 |
| Input 核心 | `subsys/input/input.c` | 事件分发、回调管理 |
| Input 头文件 | `include/zephyr/input/input.h` | API 声明 |
| 事件码定义 | `include/zephyr/dt-bindings/input/input-event-codes.h` | INPUT_KEY_* 等宏 |
| Input Kconfig | `subsys/input/Kconfig` | 子系统配置选项 |
| Input CMake | `drivers/input/CMakeLists.txt` | 驱动编译入口 |
