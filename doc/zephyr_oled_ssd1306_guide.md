# Zephyr OLED SSD1306 显示驱动教程 — 基于 STM32F407VET6

> **验证状态**: 本文档所有配置已在 STM32F407VET6 DevEBox 开发板上验证通过。
> **环境**: Zephyr 4.4.1-rc1
> **芯片**: STM32F407VET6 (I2C1, PB8-SCL / PB9-SDA)
> **显示模块**: 0.96" OLED 128x64 (SSD1306/SSD1315, I2C 地址 0x3C)
> **项目路径**: `code/blinky_test`
> **驱动源码**: `code/blinky_test/src/oled1306.c` / `oled1306.h`
> **板级 DTS**: `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts`

---

## 目录

1. [SSD1306 OLED 显示原理](#1-ssd1306-oled-显示原理)
2. [Zephyr Display 子系统架构](#2-zephyr-display-子系统架构)
3. [帧缓冲区格式 — VTILED 详解](#3-帧缓冲区格式--vtiled-详解)
4. [设备树配置](#4-设备树配置)
5. [Kconfig 配置](#5-kconfig-配置)
6. [Display API 详解](#6-display-api-详解)
7. [驱动代码实现](#7-驱动代码实现)
8. [显示方向调整](#8-显示方向调整)
9. [SSD1315 兼容性](#9-ssd1315-兼容性)
10. [常见问题排查](#10-常见问题排查)
11. [移植指南](#11-移植指南)
12. [快速参考卡](#12-快速参考卡)

---

## 1. SSD1306 OLED 显示原理

### 1.1 SSD1306 是什么

SSD1306 是 Solomon Systech 公司设计的 **单色 OLED 显示驱动芯片**，常见于 0.96"/1.3" 小型 OLED 模块。

关键参数：
- 分辨率: 最大 128 x 64 像素
- 颜色: 单色（白/蓝/黄蓝双色）
- 接口: I2C / SPI（本教程使用 I2C）
- 显存: 128 x 64 bit = 1024 字节 GDDRAM（内嵌）
- 工作电压: 2.4V ~ 3.6V（模块通常自带 3.3V 稳压）

### 1.2 GDDRAM 组织方式

SSD1306 的显存 (GDDRAM) 采用**页式（Page）组织**：

```
128 x 64 像素 = 128 列 x 8 页

         列0  列1  列2  ...  列127
       ┌────┬────┬────┬...┬────┐
Page 0 │ D0 │ D0 │ D0 │   │ D0 │  ← 行 0
(行0~7)│ D1 │ D1 │ D1 │   │ D1 │  ← 行 1
       │ D2 │ D2 │ D2 │   │ D2 │  ← 行 2
       │ D3 │ D3 │ D3 │   │ D3 │  ← 行 3
       │ D4 │ D4 │ D4 │   │ D4 │  ← 行 4
       │ D5 │ D5 │ D5 │   │ D5 │  ← 行 5
       │ D6 │ D6 │ D6 │   │ D6 │  ← 行 6
       │ D7 │ D7 │ D7 │   │ D7 │  ← 行 7
       ├────┼────┼────┼...┼────┤
Page 1 │    │    │    │   │    │  ← 行 8~15
       ├────┼────┼────┼...┼────┤
 ...   │    │    │    │   │    │
       ├────┼────┼────┼...┼────┤
Page 7 │    │    │    │   │    │  ← 行 56~63
       └────┴────┴────┴...┴────┘

每个 Page 的一列 = 1 字节:
  bit0 (D0) = 最上行
  bit7 (D7) = 最下行
```

**关键理解**：每个字节代表一列中的 8 个**垂直**像素，不是 8 个水平像素！这是与行优先帧缓冲区最根本的区别。

### 1.3 I2C 通信协议

SSD1306 的 I2C 通信使用**控制字节**区分命令和数据：

```
写命令:
  [START] [0x78] [0x00] [CMD] [STOP]
          地址   控制字节  命令字节
                 D/C=0(命令)

写数据:
  [START] [0x78] [0x40] [DATA0] [DATA1] ... [STOP]
          地址   控制字节  数据字节流
                 D/C=1(数据)

控制字节格式:
  bit 7:   Co = 0 (后续都是数据/命令)
  bit 6:   D/C = 0 命令 / 1 数据
  bit 5~0: 000000
```

---

## 2. Zephyr Display 子系统架构

### 2.1 分层架构

```
┌─────────────────────────────────────────────────┐
│            应用代码 (oled1306.c)                  │
│    oled_set_pixel() / oled_draw_rect() / ...    │
├─────────────────────────────────────────────────┤
│            帧缓冲区 (fb[1024])                    │
│    应用维护，VTILED 格式                          │
├─────────────────────────────────────────────────┤
│           Zephyr Display API                     │
│     display_write() / display_set_contrast()     │
├─────────────────────────────────────────────────┤
│          SSD1306 驱动                             │
│   drivers/display/display_ssd1306.c              │
│   将 fb[] 直接发送给硬件（不做格式转换！）          │
├─────────────────────────────────────────────────┤
│           I2C 驱动                                │
│   drivers/i2c/i2c_stm32_v1.c                     │
├─────────────────────────────────────────────────┤
│           SSD1306 硬件                            │
└─────────────────────────────────────────────────┘
```

### 2.2 关键设计决策：驱动不做格式转换

SSD1306 驱动的 `write()` 函数将应用提供的缓冲区**直接发送**给硬件，不做任何格式转换：

```c
/* drivers/display/display_ssd1306.c — ssd1306_write_default() */
static int ssd1306_write_default(...)
{
    /* 设置写入区域命令 */
    ssd1306_write_bus(dev, cmd_buf, sizeof(cmd_buf), true);
    /* 直接发送 buf，无格式转换！ */
    return ssd1306_write_bus(dev, buf, buf_len, false);
}
```

这意味着：**应用必须提供与 SSD1306 GDDRAM 格式一致的缓冲区**，否则显示内容会混乱。

### 2.3 SCREEN_INFO_MONO_VTILED 标志

SSD1306 驱动在 `get_capabilities()` 中返回：

```c
caps->screen_info = SCREEN_INFO_MONO_VTILED;
```

这个标志告诉应用：**缓冲区使用垂直分页格式**，每个字节代表 8 个垂直像素。

---

## 3. 帧缓冲区格式 — VTILED 详解

### 3.1 两种格式对比

这是本教程最关键的部分，也是最容易踩坑的地方。

#### 行优先 MONO01（常见于 LCD 驱动）

```
每个字节 = 8 个水平像素，MSB 在左

fb[0] = 第0行, 列0~7    bit7=x0, bit6=x1, ..., bit0=x7
fb[1] = 第0行, 列8~15
...
fb[15] = 第0行, 列120~127
fb[16] = 第1行, 列0~7

索引: byte_idx = y * (WIDTH / 8) + x / 8
位号: bit_idx  = 7 - (x % 8)
```

#### 垂直分页 VTILED（SSD1306 使用）

```
每个字节 = 8 个垂直像素，bit0 在上

fb[0]   = Page 0, 列0    bit0=y0, bit1=y1, ..., bit7=y7
fb[1]   = Page 0, 列1
...
fb[127] = Page 0, 列127
fb[128] = Page 1, 列0

索引: byte_idx = (y / 8) * WIDTH + x
位号: bit_idx  = y % 8
```

### 3.2 格式错误的表现

如果误用行优先格式，`display_write()` 不会报错（它不知道格式对不对），但显示内容会完全混乱：

| 现象 | 原因 |
|------|------|
| 像素位置完全错乱 | 行优先数据被当作列优先发送 |
| 文字变成竖条纹 | 水平 8 像素被解释为垂直 8 像素 |
| 边框能显示但内容乱 | 边框恰好跨越多个页面，看起来"差不多" |

### 3.3 正确的像素操作

```c
/* VTILED 格式下设置像素 (x, y) */
void oled_set_pixel(uint16_t x, uint16_t y, bool on)
{
    uint16_t page = y / 8;          /* 哪个页面 */
    uint8_t bit_idx = y % 8;        /* 页面内的哪一位 */
    uint16_t byte_idx = page * OLED_WIDTH + x;  /* fb[] 中的字节索引 */

    if (on) {
        fb[byte_idx] |= (1 << bit_idx);
    } else {
        fb[byte_idx] &= ~(1 << bit_idx);
    }
}
```

### 3.4 为什么字体数据可以直接写入

SSD1306 的字体数据通常以**列优先**格式存储（每字节 bit0=最上行），这与 VTILED 格式天然匹配：

```
字体数据:  glyph[col] 的 bit0 = 最上行
VTILED:    fb[page * WIDTH + x + col] 的 bit0 = 最上行

两者完全一致！可以直接 memcpy:
  fb[page * WIDTH + x] = glyph[0];
  fb[page * WIDTH + x + 1] = glyph[1];
  ...
```

---

## 4. 设备树配置

### 4.1 SSD1306 节点完整配置

```dts
&i2c1 {
    pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
    pinctrl-names = "default";
    clock-frequency = <I2C_BITRATE_FAST>;
    status = "okay";

    ssd1306: ssd1306@3c {
        compatible = "solomon,ssd1306";
        reg = <0x3c>;
        width = <128>;
        height = <64>;
        segment-offset = <0>;
        page-offset = <0>;
        display-offset = <0>;
        multiplex-ratio = <63>;
        prechargep = <0x22>;
        segment-remap;
        com-invdir;
    };
};

chosen {
    zephyr,display = &ssd1306;  /* 指定默认显示设备 */
};
```

### 4.2 属性详解

| 属性 | 必需 | 说明 | 128x64 典型值 |
|------|------|------|--------------|
| `compatible` | 是 | 驱动匹配字符串 | `"solomon,ssd1306"` |
| `reg` | 是 | 7 位 I2C 地址 | `<0x3c>` |
| `width` | 是 | 显示宽度（像素） | `<128>` |
| `height` | 是 | 显示高度（像素） | `<64>` |
| `segment-offset` | 是 | 列起始偏移 | `<0>` |
| `page-offset` | 是 | 页起始偏移 | `<0>` |
| `display-offset` | 是 | 显示行偏移 | `<0>` |
| `multiplex-ratio` | 是 | 复用比 = height - 1 | `<63>` (64-1) |
| `prechargep` | 是 | 预充电周期 | `<0x22>` |
| `segment-remap` | 否 | 水平镜像 | 存在即启用 |
| `com-invdir` | 否 | 垂直镜像 | 存在即启用 |
| `com-sequential` | 否 | 顺序 COM 配置 | 默认交替 |
| `inversion-on` | 否 | 颜色反转 | 默认关闭 |
| `reset-gpios` | 否 | RESET 引脚 | 可省略 |
| `use-internal-iref` | 否 | 使用内部 Iref | 默认外部 |

### 4.3 multiplex-ratio 计算规则

```
multiplex-ratio = height - 1

128x64 → multiplex-ratio = <63>
128x32 → multiplex-ratio = <31>
72x40  → multiplex-ratio = <39>
```

### 4.4 128x32 屏幕配置

如果你的 OLED 是 128x32 分辨率：

```dts
ssd1306: ssd1306@3c {
    compatible = "solomon,ssd1306";
    reg = <0x3c>;
    width = <128>;
    height = <32>;
    segment-offset = <0>;
    page-offset = <0>;
    display-offset = <0>;
    multiplex-ratio = <31>;
    prechargep = <0x22>;
    segment-remap;
    com-invdir;
    com-sequential;
    inversion-on;
};
```

---

## 5. Kconfig 配置

### 5.1 必需配置

```ini
# prj.conf
CONFIG_DISPLAY=y       # Display 子系统总开关
CONFIG_SSD1306=y       # SSD1306 驱动（DTS 中有节点时默认 y）
CONFIG_I2C=y           # I2C 子系统（SSD1306 驱动会自动 select）
```

### 5.2 可选配置

```ini
# 默认对比度（0~255，默认 128）
CONFIG_SSD1306_DEFAULT_CONTRAST=128

# 显示初始化优先级（默认 85，一般不需要改）
# CONFIG_DISPLAY_INIT_PRIORITY=85
```

### 5.3 配置依赖链

```
CONFIG_DISPLAY=y
  └── CONFIG_SSD1306=y
        ├── select I2C (如果 DTS 中 SSD1306 在 I2C 总线上)
        └── 驱动自动初始化 (SYS_INIT POST_KERNEL)
```

---

## 6. Display API 详解

### 6.1 获取设备句柄

```c
/* 方式 1: 通过 chosen 节点（推荐） */
static const struct device *oled = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* 方式 2: 通过节点标签 */
static const struct device *oled = DEVICE_DT_GET(DT_NODELABEL(ssd1306));
```

### 6.2 核心 API

#### display_write — 写入帧数据

```c
int display_write(const struct device *dev, uint16_t x, uint16_t y,
                  const struct display_buffer_descriptor *desc,
                  const void *buf);
```

**描述符结构体**：

```c
struct display_buffer_descriptor {
    uint16_t buf_size;  /* 缓冲区大小（字节） */
    uint16_t width;     /* 写入区域宽度（像素） */
    uint16_t height;    /* 写入区域高度（像素） */
    uint16_t pitch;     /* 行间距（像素），SSD1306 必须 = width */
};
```

**约束**：
- `pitch` 必须等于 `width`（驱动会检查）
- `y` 必须是 8 的倍数
- `height` 必须是 8 的倍数
- `buf_size` = width * height / 8

**整屏刷新示例**：

```c
struct display_buffer_descriptor desc = {
    .buf_size = 1024,   /* 128 * 64 / 8 */
    .width = 128,
    .height = 64,
    .pitch = 128,
};
display_write(oled, 0, 0, &desc, fb);
```

#### display_set_contrast — 设置对比度

```c
int display_set_contrast(const struct device *dev, uint8_t contrast);
```

- 范围: 0~255，默认 128
- 值越大越亮，但功耗也越高
- **注意**: 某些 OLED 模块在极端对比度值下可能不稳定

#### display_blanking_on/off — 显示开关

```c
int display_blanking_on(const struct device *dev);   /* 关显示（省电） */
int display_blanking_off(const struct device *dev);  /* 开显示 */
```

- 关显示后 GDDRAM 数据保留，重新开启后恢复显示
- 省电模式下 OLED 不发光，功耗极低

#### display_get_capabilities — 获取显示能力

```c
void display_get_capabilities(const struct device *dev,
                              struct display_capabilities *caps);
```

返回信息包括：分辨率、像素格式、screen_info（VTILED 标志）、方向等。

### 6.3 API 阻塞特性

`display_write()` 是**同步阻塞**的。内部调用 I2C 写入，整屏刷新需要传输 1024 字节：

```
400 KHz I2C 传输 1024 字节 ≈ 25ms
100 KHz I2C 传输 1024 字节 ≈ 100ms
```

在实时性要求高的场景中，需要注意此延迟。

---

## 7. 驱动代码实现

### 7.1 帧缓冲区设计

```c
/* 128 x 64 像素，VTILED 格式 = 1024 字节 */
static uint8_t fb[OLED_WIDTH * OLED_HEIGHT / 8];
```

**为什么需要自己的帧缓冲区？**

- `display_write()` 每次调用触发 I2C 传输
- 逐像素 write 需要 8192 次 I2C 传输
- 使用本地帧缓冲区，只需 1 次 `display_write()` 刷新整屏

### 7.2 像素操作

```c
void oled_set_pixel(uint16_t x, uint16_t y, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;

    uint16_t page = y / 8;
    uint8_t bit_idx = y % 8;
    uint16_t byte_idx = page * OLED_WIDTH + x;

    if (on) {
        fb[byte_idx] |= (1 << bit_idx);
    } else {
        fb[byte_idx] &= ~(1 << bit_idx);
    }
}
```

### 7.3 刷新屏幕

```c
void oled_refresh(void)
{
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(fb),
        .width = OLED_WIDTH,
        .height = OLED_HEIGHT,
        .pitch = OLED_WIDTH,
    };
    display_write(oled_dev, 0, 0, &desc, fb);
}
```

### 7.4 绘图操作

```c
/* 水平线: 逐像素设置 */
void oled_draw_hline(uint16_t x, uint16_t y, uint16_t len, bool on)
{
    for (uint16_t i = 0; i < len && (x + i) < OLED_WIDTH; i++)
        oled_set_pixel(x + i, y, on);
}

/* 垂直线: 逐像素设置 */
void oled_draw_vline(uint16_t x, uint16_t y, uint16_t len, bool on)
{
    for (uint16_t i = 0; i < len && (y + i) < OLED_HEIGHT; i++)
        oled_set_pixel(x, y + i, on);
}

/* 矩形边框: 四条线 */
void oled_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on)
{
    oled_draw_hline(x, y, w, on);
    oled_draw_hline(x, y + h - 1, w, on);
    oled_draw_vline(x, y, h, on);
    oled_draw_vline(x + w - 1, y, h, on);
}
```

### 7.5 VTILED 格式下的高效操作

由于 VTILED 格式中每个字节是 8 个垂直像素，某些操作可以大幅优化：

```c
/* 高效填充整页（8 行） */
void oled_fill_page(uint8_t page, uint8_t pattern)
{
    memset(&fb[page * OLED_WIDTH], pattern, OLED_WIDTH);
}

/* 高效画水平线（如果 y 恰好在页面边界） */
void oled_draw_hline_fast(uint16_t x, uint16_t y, uint16_t len)
{
    if (y % 8 == 0) {
        /* 整行填充，一次 memset */
        uint8_t bit = 1 << (y % 8);  /* = 1, 因为 y%8==0 */
        uint16_t base = (y / 8) * OLED_WIDTH + x;
        for (uint16_t i = 0; i < len; i++)
            fb[base + i] |= bit;
    }
}
```

---

## 8. 显示方向调整

### 8.1 segment-remap 和 com-invdir 的组合

| segment-remap | com-invdir | 效果 |
|:---:|:---:|------|
| 不设 | 不设 | 正常方向 |
| 设置 | 设置 | 旋转 180° |
| 设置 | 不设 | 水平镜像 |
| 不设 | 设置 | 垂直镜像 |

### 8.2 如何判断需要调整

1. 烧录后观察屏幕
2. 如果文字/图案上下颠倒 → 同时设置 `segment-remap` 和 `com-invdir`
3. 如果左右镜像 → 只设置 `segment-remap`
4. 如果上下镜像 → 只设置 `com-invdir`

### 8.3 旋转 180° 的 DTS 配置

```dts
ssd1306: ssd1306@3c {
    /* ... 其他属性 ... */
    segment-remap;    /* 添加此行 */
    com-invdir;       /* 添加此行 */
};
```

---

## 9. SSD1315 兼容性

### 9.1 SSD1315 是什么

SSD1315 是 SSD1306 的**国产替代芯片**，指令集高度兼容。很多廉价 OLED 模块实际使用的是 SSD1315。

### 9.2 Zephyr 是否支持

Zephyr **没有** SSD1315 的专用驱动。但可以通过 SSD1306 驱动兼容使用：

- `compatible = "solomon,ssd1306"` — 直接使用 SSD1306 驱动
- 大多数情况下可以正常工作
- 如果显示异常，尝试调整 `prechargep` 或对比度

### 9.3 已知差异

| 特性 | SSD1306 | SSD1315 |
|------|---------|---------|
| 基本指令集 | 相同 | 相同 |
| 预充电参数 | 0x22 典型 | 可能需要 0xF1 |
| 对比度范围 | 0~255 | 可能范围不同 |
| 初始化序列 | 标准兼容 | 大部分兼容 |

---

## 10. 常见问题排查

### 10.1 显示内容完全混乱

**原因**: 帧缓冲区使用了行优先格式，而非 VTILED 格式

**判断**: `display_get_capabilities()` 返回的 `screen_info` 包含 `SCREEN_INFO_MONO_VTILED`

**解决**: 使用正确的 VTILED 索引计算：
```c
/* 错误（行优先） */
byte_idx = y * (WIDTH / 8) + x / 8;
bit_idx = 7 - (x % 8);

/* 正确（VTILED） */
byte_idx = (y / 8) * WIDTH + x;
bit_idx = y % 8;
```

### 10.2 display_write 返回负值

| 错误码 | 原因 | 解决 |
|--------|------|------|
| -EINVAL | pitch != width | 设置 pitch = width |
| -EINVAL | y 不是 8 的倍数 | y 对齐到 8 |
| -EINVAL | height 不是 8 的倍数 | height 对齐到 8 |
| -ENODATA | buf_size 太小 | buf_size = width * height / 8 |

### 10.3 屏幕无显示

**排查步骤**：
1. I2C 总线扫描是否发现 0x3C 设备？
2. DTS 中 `status = "okay"` ？
3. `device_is_ready()` 返回 true？
4. `display_blanking_off()` 是否调用？
5. 帧缓冲区是否有非零数据？

### 10.4 对比度调节导致死机

**原因**: 某些 OLED 模块在快速连续设置对比度时可能不稳定

**解决**: 避免在循环中快速调节对比度，或每次调节后添加延迟

### 10.5 I2C 底层 API 与 Display API 冲突

**原因**: SSD1306 驱动和 iic_board 底层代码共享同一 I2C 地址

**解决**: 不要同时使用两种 API 操作同一设备。如果需要底层测试，将 SSD1306 节点设为 `status = "disabled"`

---

## 11. 移植指南

### 11.1 移植到其他分辨率

| 分辨率 | fb 大小 | multiplex-ratio | 页数 |
|--------|---------|-----------------|------|
| 128x64 | 1024 B | 63 | 8 |
| 128x32 | 512 B | 31 | 4 |
| 96x16 | 192 B | 15 | 2 |
| 64x48 | 384 B | 47 | 6 |

修改步骤：
1. DTS 修改 `width`、`height`、`multiplex-ratio`
2. 代码修改 `OLED_WIDTH`、`OLED_HEIGHT` 宏
3. `display_buffer_descriptor` 的参数自动跟随宏

### 11.2 移植到 SPI 接口

SSD1306 也支持 SPI 接口，速度更快（可达 10MHz）：

```dts
&spi1 {
    ssd1306: ssd1306@0 {
        compatible = "solomon,ssd1306";
        reg = <0>;
        spi-max-frequency = <10000000>;
        data-cmd-gpios = <&gpioa 0 GPIO_ACTIVE_LOW>;  /* D/C# 引脚，必需 */
        reset-gpios = <&gpioa 1 GPIO_ACTIVE_LOW>;
        width = <128>;
        height = <64>;
        /* ... 其他属性相同 ... */
    };
};
```

应用层代码完全不变，Display API 屏蔽了底层接口差异。

### 11.3 移植到其他显示驱动芯片

| 芯片 | compatible | 说明 |
|------|-----------|------|
| SSD1306 | `solomon,ssd1306` | 本教程 |
| SSD1309 | `solomon,ssd1309` | 大屏版，驱动共用 |
| SH1106 | `sinowealth,sh1106` | 兼容芯片，驱动共用 |
| ILI9340 | `ilitek,ili9340` | TFT 彩屏，非 VTILED |
| ST7789V | `sitronix,st7789v` | TFT 彩屏，非 VTILED |

**注意**: TFT 彩屏驱动使用不同的像素格式（RGB565 等），帧缓冲区格式与 SSD1306 完全不同。

### 11.4 使用 LVGL 图形库

Zephyr 集成了 LVGL，可以在 SSD1306 上使用：

```ini
# prj.conf
CONFIG_LVGL=y
CONFIG_LV_Z_VDB_SIZE=10
CONFIG_LV_Z_BITS_PER_PIXEL=1
```

LVGL 会自动处理 VTILED 格式转换，无需手动管理帧缓冲区。

---

## 12. 快速参考卡

### DTS 模板（128x64 I2C）

```dts
&i2c1 {
    ssd1306: ssd1306@3c {
        compatible = "solomon,ssd1306";
        reg = <0x3c>;
        width = <128>;
        height = <64>;
        segment-offset = <0>;
        page-offset = <0>;
        display-offset = <0>;
        multiplex-ratio = <63>;
        prechargep = <0x22>;
        segment-remap;
        com-invdir;
    };
};

chosen {
    zephyr,display = &ssd1306;
};
```

### C 代码模板

```c
#include <zephyr/drivers/display.h>

static const struct device *oled = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static uint8_t fb[128 * 64 / 8];  /* VTILED 格式帧缓冲区 */

/* 设置像素 */
static void set_pixel(int x, int y, bool on)
{
    uint16_t idx = (y / 8) * 128 + x;
    if (on) fb[idx] |= (1 << (y % 8));
    else    fb[idx] &= ~(1 << (y % 8));
}

/* 刷新屏幕 */
static void refresh(void)
{
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(fb), .width = 128,
        .height = 64, .pitch = 128,
    };
    display_write(oled, 0, 0, &desc, fb);
}
```

### VTILED vs 行优先 速查

| 项目 | VTILED (SSD1306) | 行优先 MONO01 |
|------|------------------|--------------|
| 每字节含义 | 8 个垂直像素 | 8 个水平像素 |
| bit0 对应 | 最上行 | 最右列 |
| 字节索引 | `(y/8)*W + x` | `y*(W/8) + x/8` |
| 位索引 | `y % 8` | `7 - (x % 8)` |
| fb 大小 | W*H/8 | W*H/8 |
| screen_info | `SCREEN_INFO_MONO_VTILED` | 无 VTILED 标志 |
