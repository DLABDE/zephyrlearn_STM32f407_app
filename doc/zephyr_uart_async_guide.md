# Zephyr UART Async API 实战指南

> 基于 STM32F407VET6 (stm32f4_devebox) 平台的完整学习记录，涵盖架构设计、DMA 配置、
> 坑点排查与 Modbus 集成思路。适用于未来移植板卡和快速参考。

---

## 目录

1. [架构总览](#1-架构总览)
2. [DTS 硬件配置](#2-dts-硬件配置)
3. [Kconfig 配置](#3-kconfig-配置)
4. [核心实现](#4-核心实现)
5. [timeout 参数深度解析](#5-timeout-参数深度解析)
6. [踩坑记录：RX 收不到数据](#6-踩坑记录rx-收不到数据)
7. [踩坑记录：帧被截断（DMA 缓冲区满分割）](#7-踩坑记录帧被截断dma-缓冲区满分割)
8. [DMA Burst Size 警告](#8-dma-burst-size-警告)
9. [Modbus RTU 集成思路](#9-modbus-rtu-集成思路)
10. [文件索引](#10-文件索引)

---

## 1. 架构总览

### 1.1 设计目标

```
┌─────────────────────────────────────────────────────────┐
│                    线程 A (业务逻辑)                      │
│  uart3_tx_async() ──→ 消息队列 ──→ 后台 DMA 发送         │
│  uart4_rx_frame_get() ←── 消息队列 ←── DMA接收+IDLE中断  │
└─────────────────────────────────────────────────────────┘
```

- **发送**：线程 A 调用 `uart_tx()` 将数据交给 DMA 后台发送，立即返回不阻塞。
- **接收**：UART4 使用 DMA 双缓冲 + IDLE 空闲中断自动按帧分割，ISR 将帧推入消息队列，
  线程 C 从队列取出处理。

### 1.2 Async API vs 中断 API

| 特性 | 中断 API (`uart_irq_*`) | Async API (`uart_tx` / `uart_rx_enable`) |
|---|---|---|
| 数据搬运 | CPU 逐字节处理 | DMA 自动搬运 |
| 发送阻塞 | 可能阻塞 | 非阻塞 |
| 帧分割 | 手动处理 | IDLE 中断自动分割 |
| 缓冲管理 | 自行管理 | 驱动双缓冲 ping-pong |
| 依赖 | 仅需中断 | 需要 DMA |

### 1.3 数据流

```
UART RX 引脚
    │
    ▼
┌──────────┐    DMA stream    ┌────────────┐
│  USART   │ ════════════════ │  rx_buf_a   │  ← DMA 线性写入的靶子
│  硬件    │     (ping-pong)  │  rx_buf_b   │     写满一个切换到另一个
└──────────┘                  └────────────┘
    │                               │
    │ IDLE/TC 中断                  │ ISR 中 memcpy
    ▼                               ▼
┌──────────┐                  ┌──────────────┐
│  ISR回调  │── RX_RDY事件 ──→│ rx_reasm_buf │  ← 帧重组缓冲区
│  (uart_   │                  │  (拼接碎片)   │     拼接被 DMA 边界截断的帧
│   async_  │                  └──────┬───────┘
│   call-   │                         │ 定时器到期 (线路空闲=帧结束)
│   back)   │                         ▼
└──────────┘                   k_msgq_put()
                                     │
                                     ▼
                             ┌──────────────┐
                             │ uart_rx_msgq │  ← 消息队列
                             └──────┬───────┘
                                    │ k_msgq_get()
                                    ▼
                             ┌──────────────┐
                             │ 线程 C 读取   │
                             │ uart4_rx_     │
                             │ frame_get()   │
                             └──────────────┘
```

---

## 2. DTS 硬件配置

### 2.1 DMA 控制器使能

STM32F4 有两个 DMA 控制器，UART3/UART4 的 DMA 请求都在 DMA1 上，必须显式启用：

```dts
&dma1 {
    status = "okay";
};
```

### 2.2 UART DMA 通道配置

```dts
&usart3 {
    pinctrl-0 = <&usart3_tx_pb10 &usart3_rx_pb11>;
    pinctrl-names = "default";
    current-speed = <115200>;
    dmas = <&dma1 3 4 STM32_DMA_PERIPH_TX STM32_DMA_FIFO_FULL>,
           <&dma1 1 4 STM32_DMA_PERIPH_RX STM32_DMA_FIFO_FULL>;
    dma-names = "tx", "rx";
    status = "okay";
};

&uart4 {
    pinctrl-0 = <&uart4_tx_pc10 &uart4_rx_pc11>;
    pinctrl-names = "default";
    current-speed = <115200>;
    dmas = <&dma1 4 4 STM32_DMA_PERIPH_TX STM32_DMA_FIFO_FULL>,
           <&dma1 2 4 STM32_DMA_PERIPH_RX STM32_DMA_FIFO_FULL>;
    dma-names = "tx", "rx";
    status = "okay";
};
```

### 2.3 `dmas` 属性参数详解

语法：`<&dma_controller stream channel config features>`

| Cell | 名称 | 说明 |
|---|---|---|
| 0 | **stream** | DMA Stream 编号 (0-7)。STM32F4 每个 DMA 控制器有 8 个 Stream |
| 1 | **channel** | DMA 请求通道号 (0-7)。每个外设在 DMA 上有固定的请求线 |
| 2 | **config** | 通道配置：方向 + 数据宽度 + 地址增量模式 |
| 3 | **features** | FIFO 阈值配置 |

**STM32F407 USART/UART DMA 映射表：**

| 外设 | TX Stream | RX Stream | Channel |
|---|---|---|---|
| USART1 | DMA2 Stream 7 | DMA2 Stream 2/5 | 4 |
| USART2 | DMA1 Stream 6 | DMA1 Stream 5 | 4 |
| USART3 | DMA1 Stream 3 | DMA1 Stream 1 | 4 |
| UART4 | DMA1 Stream 4 | DMA1 Stream 2 | 4 |

**config 常用宏：**

| 宏 | 值 | 含义 |
|---|---|---|
| `STM32_DMA_PERIPH_TX` | `MEM_TO_PERIPH \| MEM_INC` | 内存→外设，内存地址自增 |
| `STM32_DMA_PERIPH_RX` | `PERIPH_TO_MEM \| MEM_INC` | 外设→内存，内存地址自增 |
| `STM32_DMA_16BITS` | `PERIPH_16BITS \| MEM_16BITS` | 16 位数据宽度 |

> **参考文件**：`zephyr/include/zephyr/dt-bindings/dma/stm32_dma.h`

---

## 3. Kconfig 配置

```ini
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y    # 中断驱动（Async API 的前置依赖）
CONFIG_UART_ASYNC_API=y           # Async API (uart_tx / uart_rx_enable)
CONFIG_DMA=y                      # DMA 子系统
```

**依赖链**：`CONFIG_UART_ASYNC_API` → `CONFIG_UART_INTERRUPT_DRIVEN` → `CONFIG_DMA`

---

## 4. 核心实现

### 4.1 数据结构 (uart.h)

```c
#define UART_RX_BUF_SIZE 256
#define UART_FRAME_TIMEOUT_US 1000   /* 帧重组超时，115200bps 下 1ms */

struct uart_rx_frame {
    uint8_t data[UART_RX_BUF_SIZE];
    size_t len;
};
```

### 4.2 为什么需要双缓冲而不是一个 FIFO？

这是从 CPU 中断模式切换到 DMA 模式时最常遇到的困惑。

**CPU 中断模式（你之前用的方式）：**
```
UART 收到 1 字节 → CPU 中断 → 你在中断里把字节写入环形 FIFO
→ 线程从 FIFO 读出

FIFO 能用是因为：写数据的是 CPU（你的中断代码），CPU 知道怎么维护
写指针、读指针、环形回绕。你完全掌控写入逻辑。
```

**DMA 模式（Async API 的方式）：**
```
UART 收到数据 → DMA 硬件自动搬运到内存 → CPU 不参与

DMA 的工作方式：你给它一个"起始地址 + 长度"，它就线性地往这个地址范围
持续写入，直到写满为止。DMA 不知道什么是"队列"、什么是"环形缓冲区"，
它只会线性写入一块连续内存。

问题：DMA 写满这块内存后怎么办？它需要立刻有一个新地方继续写，
否则新来的 UART 数据就丢了。

双缓冲的解决方案：
  DMA 正在写 buf_a → 我们提前准备好 buf_b
  buf_a 写满 → DMA 无缝切换到 buf_b → 零间隔，不丢数据
  同时 buf_a 被释放给 CPU 处理
  下次 buf_b 写满 → 切回 buf_a → 循环

一句话总结：FIFO 是 CPU 模式下的方案，双缓冲是 DMA 模式下的方案。
两者解决同一个问题（接收时不丢数据），但适配不同的数据搬运者。
```

| 对比项 | CPU 中断 + FIFO | DMA + 双缓冲 |
|---|---|---|
| 数据搬运者 | CPU（中断代码） | DMA 硬件 |
| 写入方式 | 逐字节，可维护环形指针 | 线性写入固定长度，写满即停 |
| 缓冲区结构 | 环形 FIFO（读写指针） | 两块线性缓冲区（ping-pong） |
| 切换时机 | 每字节都可切换 | 缓冲区满或 IDLE 中断时切换 |
| CPU 开销 | 每字节一次中断 | 零 CPU 参与（DMA 自动搬运） |

### 4.3 为什么还需要第三个缓冲区（reasm_buf）？

双缓冲解决了"DMA 不丢数据"的问题，但引入了新问题：

```
DMA 缓冲区边界可能正好切在一帧数据的中间：

  buf_a: [帧1][帧2][帧3][帧4前12字节] ← DMA 写满 → 切换
  buf_b: [帧4后6字节]...              ← 继续接收

  驱动产生两个 RX_RDY：
    RX_RDY #1: "Hello, UAR"   ← buf_a 的尾部
    RX_RDY #2: "T! 5\r\n"     ← buf_b 的头部

  一个逻辑帧变成了两个碎片！

reasm_buf 的作用：把碎片拼回来
  RX_RDY #1 → 追加 "Hello, UAR" 到 reasm_buf → 启动定时器
  RX_RDY #2 → 追加 "T! 5\r\n" 到 reasm_buf → 重启定时器
  定时器到期（线路空闲=帧结束）→ 提交完整帧 "Hello, UART! 5\r\n"
```

### 4.4 三个缓冲区各司其职

| 缓冲区 | 谁写入 | 谁读取 | 作用 |
|---|---|---|---|
| `rx_buf_a/b` | DMA 硬件 | 回调（ISR） | DMA 线性写入的靶子，ping-pong 轮换 |
| `rx_reasm_buf` | 回调（ISR） | 定时器回调（线程） | 拼接被 DMA 边界截断的帧碎片 |
| `uart_rx_msgq` | 定时器回调 | 用户线程 | 最终交付完整帧的队列 |

数据流：`UART → DMA → buf_a/b → ISR复制到reasm_buf → 定时器到期 → msgq → 用户线程`

### 4.5 双缓冲 (Ping-Pong) 机制

双缓冲是 Async RX 的核心机制，避免 DMA 正在写入时数据被覆盖：

```
时间线:
  init:   提供 buf_a → DMA 往 buf_a 写
  事件1:  RX_BUF_REQUEST → 提供 buf_b 作为"下一块"
  数据来: 空闲中断 → RX_RDY(buf_a) → DMA 自动切换到 buf_b
  事件2:  RX_BUF_REQUEST → 提供 buf_a 作为"下一块"
  数据来: 空闲中断 → RX_RDY(buf_b) → DMA 自动切换到 buf_a
  事件3:  ...循环往复...
```

```c
static uint8_t rx_buf_a[UART_RX_BUF_SIZE];
static uint8_t rx_buf_b[UART_RX_BUF_SIZE];
static uint8_t *rx_active_buf = rx_buf_a;
```

### 4.6 消息队列 (k_msgq)

```c
K_MSGQ_DEFINE(uart_rx_msgq, sizeof(struct uart_rx_frame), 8, 4);
```

参数含义：
- `uart_rx_msgq` — 队列名称
- `sizeof(struct uart_rx_frame)` — 每条消息的大小（一帧数据）
- `8` — 队列容量，最多存 8 条消息（8 帧）
- `4` — 消息内容的对齐字节数

工作方式：生产者-消费者模式
- 生产者：定时器回调中 `k_msgq_put()` 放入一帧
- 消费者：用户线程中 `k_msgq_get()` 取出一帧
- 队列满时 put 丢弃（`K_NO_WAIT`），队列空时 get 阻塞或返回错误

为什么用消息队列而不是全局变量？消息队列是内核提供的线程安全 IPC，
内部有自旋锁保护。ISR 写入 + 线程读取，不需要手动加锁。

### 4.7 帧重组机制 (Reassembly)

帧重组缓冲区 + 延迟定时器，解决 DMA 缓冲区边界截断帧的问题：

```c
static uint8_t rx_reasm_buf[UART_RX_BUF_SIZE];  /* 重组缓冲区 */
static size_t rx_reasm_len = 0;                  /* 已追加的数据长度 */
static struct k_work_delayable rx_reasm_work;     /* 延迟工作项 */
```

**k_work_delayable** 是内核提供的"延迟执行的函数"机制：
- `k_work_init_delayable(&work, fn)` — 初始化，绑定回调函数
- `k_work_reschedule(&work, K_USEC(N))` — N 微秒后执行 fn，如果之前已调度则重新计时
- `k_work_cancel_delayable(&work)` — 取消定时器

重组的核心逻辑：
```
每次 RX_RDY → 追加数据到 reasm_buf → reschedule 定时器
如果很快又来 RX_RDY（DMA 截断的碎片）→ 重新计时，第一个不会触发提交
直到线路真正空闲 → 定时器到期 → 提交完整帧到消息队列
```

### 4.8 回调函数 — 事件分发中枢

所有 UART 异步事件都在回调中处理，**回调运行在中断上下文**，因此：

- 不能做耗时操作
- 数据通过 `k_msgq_put` 传递到线程上下文（`K_NO_WAIT` 非阻塞）

```c
static void uart_async_callback(const struct device *dev,
                                struct uart_event *evt,
                                void *user_data)
{
    switch (evt->type) {

    case UART_TX_DONE:
        /* 发送完成，DMA 已完成数据搬运 */
        break;

    case UART_RX_RDY: {
        /* 接收到一段数据，追加到重组缓冲区 */
        /* 注意：这不是"收到完整一帧"！一个逻辑帧可能产生 1~2 个 RX_RDY */
        size_t chunk_len = evt->data.rx.len;
        memcpy(&rx_reasm_buf[rx_reasm_len],
               &evt->data.rx.buf[evt->data.rx.offset],
               chunk_len);
        rx_reasm_len += chunk_len;
        k_work_reschedule(&rx_reasm_work, K_USEC(UART_FRAME_TIMEOUT_US));
        break;
    }

    case UART_RX_BUF_REQUEST:
        /* 驱动请求下一块缓冲区，切换 ping-pong */
        if (rx_active_buf == rx_buf_a) {
            rx_active_buf = rx_buf_b;
        } else {
            rx_active_buf = rx_buf_a;
        }
        uart_rx_buf_rsp(dev, rx_active_buf, UART_RX_BUF_SIZE);
        break;

    case UART_RX_BUF_RELEASED:
        /* 缓冲区已释放，数据已在 RX_RDY 中复制到 reasm_buf */
        break;

    case UART_RX_DISABLED:
        /* 接收被禁用：提交残余数据 + 重新启动接收 */
        k_work_cancel_delayable(&rx_reasm_work);
        rx_reasm_submit();  /* 提交 reasm_buf 中残余的帧 */
        uart_rx_enable(dev, rx_buf_a, UART_RX_BUF_SIZE, 0);
        break;
    }
}
```

### 4.9 初始化

```c
int uart_init(void)
{
    if (!device_is_ready(uart3) || !device_is_ready(uart4)) {
        return -1;
    }

    k_work_init_delayable(&rx_reasm_work, rx_reasm_timeout_fn);

    uart_callback_set(uart3, uart_async_callback, NULL);
    uart_callback_set(uart4, uart_async_callback, NULL);

    int ret = uart_rx_enable(uart4, rx_buf_a, UART_RX_BUF_SIZE, 0);
    return ret;
}
```

### 4.10 线程接口

```c
/*
 * 异步发送 — 线程 A 不被阻塞
 *
 * uart_tx() 将数据交给 DMA 后台发送，立即返回。
 * 发送完成后通过 UART_TX_DONE 回调通知。
 */
int uart3_tx_async(const uint8_t *data, size_t len)
{
    return uart_tx(uart3, data, len, SYS_FOREVER_US);
}

/*
 * 获取一帧 — 线程 C 阻塞等待
 *
 * K_NO_WAIT 非阻塞轮询，有帧则返回 0，无帧返回 -ENOMSG
 * K_FOREVER 则阻塞直到有数据
 */
int uart4_rx_frame_get(struct uart_rx_frame *frame, k_timeout_t timeout)
{
    return k_msgq_get(&uart_rx_msgq, frame, timeout);
}
```

### 4.11 主循环调用示例

```c
/* 发送：不阻塞 */
const char *msg = "Hello, UART!\r\n";
uart3_tx_async((const uint8_t *)msg, strlen(msg));

/* 接收：非阻塞轮询 */
struct uart_rx_frame rx_frame;
while (uart4_rx_frame_get(&rx_frame, K_NO_WAIT) == 0) {
    printk("UART4 RX: %.*s\n", (int)rx_frame.len, rx_frame.data);
}
```

---

## 5. timeout 参数深度解析

### 5.1 函数签名

```c
int uart_rx_enable(const struct device *dev, uint8_t *buf,
                   size_t len, int32_t timeout);
```

> 声明位置：`zephyr/include/zephyr/drivers/uart.h:834`

### 5.2 参数含义

**timeout**：接收到至少 1 个字节后，如果线路持续空闲超过此时间（微秒），
驱动触发 `UART_RX_RDY` 事件并上报已收到的数据。

### 5.3 三种取值的底层行为

```
                    timeout = 0              timeout = 正数           timeout = SYS_FOREVER_US
                    ────────────             ─────────────────        ───────────────────────
UART收到数据
  → DMA写入缓冲区
  → IDLE中断触发
       │
       ├─ timeout==0? ──YES──→ 立即 flush    1. 启动定时器             1. async_timer_start()
       │                     → RX_RDY ✓        等待 N μs               检查 timeout != FOREVER
       │                                       到期后 flush            → 条件为 false
       │                     → RX_RDY ✓        什么都不做
       │
       └─ timeout!=0? ──→ async_timer_start()                          → 数据永远留在DMA缓冲
                                                                       → RX_RDY 永不产生 ✗
```

驱动源码关键路径（`zephyr/drivers/serial/uart_stm32.c`）：

**IDLE 中断处理（L1482-1488）：**
```c
if (data->dma_rx.timeout == 0) {
    uart_stm32_dma_rx_flush(dev, STM32_ASYNC_STATUS_TIMEOUT); // 立即刷新
} else {
    async_timer_start(&data->dma_rx.timeout_work,
                      data->dma_rx.timeout);                   // 启动定时器
}
```

**定时器启动函数（L1314-1322）：**
```c
static inline void async_timer_start(struct k_work_delayable *work,
                                     int32_t timeout)
{
    // 只有有限正数才真正启动定时器
    if ((timeout != SYS_FOREVER_US) && (timeout != 0)) {
        k_work_reschedule(work, K_USEC(timeout));
    }
    // SYS_FOREVER_US → 什么都不做
    // 0             → 也不做（上面的 IDLE 分支已先 flush 了）
}
```

### 5.4 如何选择 timeout

| timeout 值 | 适用场景 | 示例 |
|---|---|---|
| **0** | 按帧接收，IDLE 立即上报，应用层判断帧完整性 | Modbus RTU、AT 指令响应 |
| **正数** | 容忍帧内短暂空闲抖动，一个完整帧在多段数据中 | 带延迟的长协议帧 |
| **SYS_FOREVER_US** | 固定长度协议，仅靠缓冲区满触发 RX_RDY | 定长传感器数据包 |

### 5.5 Modbus RTU 波特率参考

Modbus RTU 帧间隔 = 3.5 字符时间。不同波特率下对应微秒值：

| 波特率 | 1 字符时间 | 3.5 字符 | 建议 timeout |
|---|---|---|---|
| 9600 | ~1042 μs | ~3646 μs | `0` 或 `4000` |
| 19200 | ~521 μs | ~1823 μs | `0` 或 `2000` |
| 115200 | ~87 μs | ~304 μs | `0` 或 `300` |

用 `0` 最简单可靠——硬件 IDLE 中断本身就是"帧间隔"的信号，
驱动立即刷新即可。用具体微秒值可以过滤极短暂的线路噪声。

---

## 6. 踩坑记录：RX 收不到数据

### 6.1 现象

```
UART init: uart_rx_enable ret=0   ← 初始化成功
UART CB: RX_BUF_REQUEST           ← 第一次缓冲请求正常
UART TX: uart_tx ret=0            ← 发送正常
UART CB: TX_DONE                  ← 发送完成正常
...
(永远没有 UART CB: RX_RDY)       ← 接收事件从未触发 ✗
```

### 6.2 排查过程

1. **怀疑 DMA 通道映射错误** → 查阅 STM32F4 Reference Manual 确认 DTS 配置正确
2. **怀疑缓存一致性问题** → 查阅 `stm32_cache.h`，确认 CONFIG_DCACHE 未启用时 `stm32_buf_in_nocache()` 永远返回 true
3. **怀疑 DMA 配置失败** → 检查 `uart_rx_enable` 返回 0，说明初始化成功
4. **分析驱动 ISR 代码** → 找到根因

### 6.3 根因

`uart_rx_enable()` 使用了 `SYS_FOREVER_US` 作为 timeout 参数。

在驱动 ISR 中，IDLE 中断触发后走 "非零 timeout" 分支，调用 `async_timer_start()`。
但该函数内部对 `SYS_FOREVER_US` 做了特殊处理——**不启动定时器，不做任何事情**。
导致 DMA 缓冲区数据永远不被刷新，`RX_RDY` 事件永不产生。

```
修复前: uart_rx_enable(uart4, rx_buf_a, UART_RX_BUF_SIZE, SYS_FOREVER_US);
修复后: uart_rx_enable(uart4, rx_buf_a, UART_RX_BUF_SIZE, 0);
```

### 6.4 教训

- **`SYS_FOREVER_US` 不是"无限等待"的意思**，而是"禁用 timeout"，即不靠超时触发数据上报。
  只有缓冲区被填满时才会触发 `RX_RDY`。
- **对于按帧接收的场景，必须使用 `timeout = 0`**，让 IDLE 中断立即刷新 DMA 数据。
- 阅读驱动源码是解决问题的终极手段。函数文档（`uart.h` 的注释）只说了
  "SYS_FOREVER_US disables timeout"，但没有说明这会导致数据永不刷新。

---

## 7. 踩坑记录：帧被截断（DMA 缓冲区满分割）

### 7.1 现象

高频率发送时，接收到的帧被截断成两段，且截断频率与缓冲区大小成正比：

```
UART4: Hello, U           ← 帧前半部分
UART4: ART! 5             ← 帧后半部分
UART CB: RX_BUF_RELEASED  ← DMA 缓冲区切换
UART CB: RX_BUF_REQUEST
```

| BUF_SIZE | 每N帧截断一次 | N = BUF_SIZE / 帧长 |
|---|---|---|
| 64 | ~5 帧 | 64/16 = **4** |
| 128 | ~10 帧 | 128/16 = **8** |
| 256 | ~16 帧 | 256/16 = **16** |

### 7.2 根因

DMA NORMAL 模式有固定传输计数（= `buffer_length`）。当 DMA 写满整个缓冲区后，
触发 TC（Transfer Complete）中断，驱动切换到下一个 ping-pong 缓冲区。

**如果此时正好有一帧数据正在接收中，该帧就会被截断在两个缓冲区之间：**

```
buf_a: [帧1][帧2][帧3][帧4前12字节] ← DMA写满 → TC中断
                                        ↓ 切换到 buf_b
buf_b: [帧4后6字节]...                ← IDLE中断 → RX_RDY

结果: 一个逻辑帧变成两个 RX_RDY 事件
      "Hello, UAR" + "T! 5"
```

这是 DMA NORMAL + ping-pong 的**固有行为**，无法通过调整 timeout 消除。
增大缓冲区只能降低截断频率，但不能根治。

### 7.3 解决方案：帧重组（Reassembly）

在回调中增加重组缓冲区 + 定时器：

```
RX_RDY 事件 → 追加数据到 reasm_buf → 重启定时器
                                        ↓ 定时器到期（线路空闲=帧结束）
                                    提交完整帧到消息队列
```

**工作原理**：
1. 每次 `RX_RDY` 将数据追加到 `rx_reasm_buf`，并重启 `UART_FRAME_TIMEOUT_US` 定时器
2. 如果帧被 DMA 截断，第二个 `RX_RDY` 到达时重启定时器，数据继续追加
3. 线路空闲超过 `UART_FRAME_TIMEOUT_US` 后，定时器到期 → 完整帧推入消息队列

```c
/* 重组缓冲区 */
static uint8_t rx_reasm_buf[UART_RX_BUF_SIZE];
static size_t rx_reasm_len = 0;
static struct k_work_delayable rx_reasm_work;

/* RX_RDY 回调：追加数据 + 重启定时器 */
case UART_RX_RDY: {
    memcpy(&rx_reasm_buf[rx_reasm_len],
           &evt->data.rx.buf[evt->data.rx.offset],
           evt->data.rx.len);
    rx_reasm_len += evt->data.rx.len;
    k_work_reschedule(&rx_reasm_work, K_USEC(UART_FRAME_TIMEOUT_US));
    break;
}

/* 定时器到期：提交完整帧 */
static void rx_reasm_timeout_fn(struct k_work *work)
{
    unsigned int key = irq_lock();
    /* 复制 reasm_buf → uart_rx_frame → k_msgq_put */
    rx_reasm_submit();
    irq_unlock(key);
}
```

### 7.4 `UART_FRAME_TIMEOUT_US` 选择

定时器值 = 帧间空闲时间。需大于一个字符传输时间，小于帧间隔：

| 波特率 | 1字符时间 | 建议 `UART_FRAME_TIMEOUT_US` |
|---|---|---|
| 9600 | ~1042 μs | `4000` |
| 19200 | ~521 μs | `2000` |
| 115200 | ~87 μs | `1000` |

---

## 8. DMA Burst Size 警告

```
<err> dma_stm32_v1: Memory burst size error, using single burst as default
<err> dma_stm32_v1: Peripheral burst size error, using single burst as default
```

### 8.1 原因

UART 数据宽度为 8-bit，DMA 单次传输 1 字节，对应的 burst length = 1 beat。
而 STM32F4 DMA 的 burst 模式最小需要 4 beats（16 bytes）。

### 8.2 影响

**无功能影响**。驱动检测到 burst size 不匹配后，自动回退到 single transfer 模式。
UART 单个字节传输本身也不需要 burst，功能和性能完全正常。

### 8.3 消除警告（可选）

在 `dmas` 属性的 config 中显式指定不使用 burst，或调整 DMA FIFO 阈值。
但对于学习项目没有必要处理。

---

## 9. Modbus RTU 集成思路

基于上述 Async API 架构，可以这样集成 Modbus：

```
                        ┌─────────────────────┐
UART4 RX                │   Modbus 线程       │
  │                     │                     │
  ▼                     │  uart4_rx_frame_get │
DMA 双缓冲              │       │             │
  │                     │       ▼             │
  ▼                     │  modbus_parse()     │
IDLE 中断分割帧         │  校验 CRC           │
  │                     │  解析功能码          │
  ▼                     │  构造响应            │
k_msgq_put()            │       │             │
  │                     │       ▼             │
  ▼               ┌─────┤  uart3_tx_async() ──→ UART3 TX
uart_rx_msgq ─────┘     └─────────────────────┘
```

### 关键设计要点

1. **帧分割**：`timeout = 0` 让硬件 IDLE 中断自动按帧分割，天然符合 Modbus 3.5 字符帧间隔
2. **非阻塞发送**：响应通过 `uart3_tx_async()` 非阻塞发送，不阻塞 Modbus 处理线程
3. **超时处理**：接收端使用 `K_MSEC(timeout)` 实现 Modbus 协议层的响应超时

### 为什么这个架构适合 Modbus

- Modbus RTU 的帧边界就是 3.5 字符空闲 → UART hardware IDLE 检测天然匹配
- 半双工通信，RX 和 TX 不会同时进行 → 双缓冲绰绰有余
- 消息队列解耦了 ISR 和业务线程 → 中断不会被 Modbus 处理阻塞

---

## 10. 文件索引

| 文件 | 说明 |
|---|---|
| `code/blinky_test/src/uart.h` | UART 模块接口定义，缓冲区大小、帧结构体 |
| `code/blinky_test/src/uart.c` | UART 模块实现，双缓冲、回调、消息队列 |
| `code/blinky_test/src/main.c` | 主循环调用示例 |
| `code/blinky_test/prj.conf` | Kconfig 配置 |
| `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts` | DTS DMA 通道配置 |
| `zephyr/drivers/serial/uart_stm32.c` | STM32 UART 驱动源码（Async API 核心实现） |
| `zephyr/include/zephyr/drivers/uart.h` | UART Async API 声明与文档 |
| `zephyr/include/zephyr/dt-bindings/dma/stm32_dma.h` | DMA 配置宏定义 |
| `zephyr/soc/st/stm32/common/stm32_cache.h` | 缓存一致性工具函数 |