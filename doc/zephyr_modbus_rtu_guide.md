# Zephyr Modbus RTU 子系统实战指南

> 基于 STM32F407VET6 (stm32f4_devebox) 平台的完整学习记录，涵盖 Kconfig 踩坑、
> DTS 多接口配置、服务端/客户端通信、CRC 校验机制、自定义功能码等。
> 适用于未来移植板卡和快速参考。

---

## 目录

1. [Modbus RTU 协议基础](#1-modbus-rtu-协议基础)
2. [Zephyr Modbus 子系统架构](#2-zephyr-modbus-子系统架构)
3. [Kconfig 配置与踩坑](#3-kconfig-配置与踩坑)
4. [DTS 多接口配置](#4-dts-多接口配置)
5. [服务端实现](#5-服务端实现)
6. [客户端实现](#6-客户端实现)
7. [初始化流程](#7-初始化流程)
8. [CRC 校验机制](#8-crc-校验机制)
9. [自定义功能码](#9-自定义功能码)
10. [LOG_MODULE_REGISTER 日志系统](#10-log_module_register-日志系统)
11. [踩坑记录](#11-踩坑记录)
12. [文件索引](#12-文件索引)

---

## 1. Modbus RTU 协议基础

### 1.1 什么是 Modbus RTU

Modbus 是工业通信领域最广泛使用的协议之一，RTU（Remote Terminal Unit）是其串行线传输模式。

核心特点：
- **一主多从**：总线上一个主站（Client/Master），多个从站（Server/Slave）
- **从站寻址**：每个从站有唯一 unit_id（1~247），0 为广播地址
- **帧间隔**：3.5 个字符时间的空闲作为帧边界
- **CRC16 校验**：每帧自动附加 CRC16-ANSI 校验码

### 1.2 Modbus 数据模型

Modbus 定义了四类数据对象：

| 数据对象 | 类型 | 访问方式 | 地址范围 |
|---------|------|---------|---------|
| Coils (线圈) | 单个 bit | 读写 | 00001~09999 |
| Discrete Inputs (离散输入) | 单个 bit | 只读 | 10001~19999 |
| Input Registers (输入寄存器) | 16-bit 字 | 只读 | 30001~39999 |
| Holding Registers (保持寄存器) | 16-bit 字 | 读写 | 40001~49999 |

### 1.3 功能码一览

| 功能码 | 名称 | 操作对象 | 方向 |
|-------|------|---------|------|
| FC01 | Read Coils | 线圈 | Client → Server |
| FC02 | Read Discrete Inputs | 离散输入 | Client → Server |
| FC03 | Read Holding Registers | 保持寄存器 | Client → Server |
| FC04 | Read Input Registers | 输入寄存器 | Client → Server |
| FC05 | Write Single Coil | 线圈 | Client → Server |
| FC06 | Write Single Register | 保持寄存器 | Client → Server |
| FC15 | Write Multiple Coils | 线圈 | Client → Server |
| FC16 | Write Multiple Registers | 保持寄存器 | Client → Server |

### 1.4 RTU 帧结构

```
┌──────────┬──────────┬──────────────┬─────────┬──────┐
│ unit_id  │  FC      │    Data       │ CRC Lo  │ CRC Hi │
│ 1 byte   │ 1 byte   │  N bytes      │ 1 byte  │ 1 byte │
└──────────┴──────────┴──────────────┴─────────┴──────┘
```

- **unit_id**：从站地址
- **FC**：功能码
- **Data**：与功能码相关的数据（寄存器地址、数量、值等）
- **CRC**：CRC16-ANSI 校验，覆盖前面所有字节

---

## 2. Zephyr Modbus 子系统架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                    应用层                                  │
│                                                          │
│  Server 端                        Client 端              │
│  ┌─────────────────┐              ┌─────────────────┐    │
│  │ user_callbacks   │              │ modbus_read_*   │    │
│  │ (coil_rd/wr,    │              │ modbus_write_*  │    │
│  │  holding_reg_   │              │                 │    │
│  │  rd/wr, ...)    │              │                 │    │
│  └────────┬────────┘              └────────┬────────┘    │
│           │                                │              │
├───────────┼────────────────────────────────┼──────────────┤
│           │      Modbus 子系统内核          │              │
│           ▼                                ▼              │
│  ┌─────────────────┐              ┌─────────────────┐    │
│  │ modbus_server_  │              │ modbus_client_  │    │
│  │ handler()       │              │ 请求/响应管理    │    │
│  └────────┬────────┘              └────────┬────────┘    │
│           │                                │              │
│           ▼                                ▼              │
│  ┌─────────────────────────────────────────────────┐     │
│  │              modbus_core (接口管理)               │     │
│  │  mb_ctx_tbl[] → iface 0, iface 1, ...           │     │
│  └───────────────────────┬─────────────────────────┘     │
│                          │                                │
├──────────────────────────┼────────────────────────────────┤
│                          ▼                                │
│  ┌─────────────────────────────────────────────────┐     │
│  │          modbus_serial (串口驱动层)               │     │
│  │  UART 中断接收 → CRC 校验 → 帧解析               │     │
│  │  帧构造 → CRC 追加 → UART 发送                    │     │
│  └───────────────────────┬─────────────────────────┘     │
│                          │                                │
│                          ▼                                │
│                    UART 硬件 (usart3/uart4)               │
└──────────────────────────────────────────────────────────┘
```

### 2.2 关键概念

- **接口（iface）**：每个 DTS 中 `zephyr,modbus-serial` 节点对应一个 Modbus 接口，
  由 `mb_ctx_tbl[]` 数组管理，通过索引号访问
- **接口名（iface_name）**：由 `DEVICE_DT_NAME(node_id)` 生成，通常等于 DTS 子节点名
  （如 `"modbus0"`、`"modbus1"`），用于 `modbus_iface_get_by_name()` 查找索引
- **角色（Role）**：一个接口只能是 Client 或 Server，不能同时兼具，
  由 `modbus_init_server()` / `modbus_init_client()` 决定

---

## 3. Kconfig 配置与踩坑

### 3.1 正确配置

```ini
CONFIG_MODBUS=y
CONFIG_MODBUS_ROLE_CLIENT_SERVER=y
```

### 3.2 踩坑：同时写了两个互斥选项

**错误写法**：

```ini
CONFIG_MODBUS_ROLE_SERVER=y    ← 先写
CONFIG_MODBUS_ROLE_CLIENT=y    ← 后写，覆盖了前者！
```

**现象**：

```
<err> modbus: Modbus server support is not enabled
ERR: modbus init failed
```

**根因**：

Zephyr Kconfig 中，`MODBUS_ROLE_CLIENT`、`MODBUS_ROLE_SERVER`、`MODBUS_ROLE_CLIENT_SERVER`
被放在 `choice` 块里，是**互斥选择**（三选一）：

```kconfig
choice
    prompt "Supported node roles"
    default MODBUS_ROLE_CLIENT_SERVER

config MODBUS_ROLE_CLIENT       # 选项1：仅客户端
config MODBUS_ROLE_SERVER       # 选项2：仅服务端
config MODBUS_ROLE_CLIENT_SERVER # 选项3：客户端+服务端
endchoice
```

当 `prj.conf` 同时写两个 `choice` 成员时，**后写的覆盖前写的**。
所以最终只有 `ROLE_CLIENT` 生效，`ROLE_SERVER` 被注释掉了。

而代码中检查的是内部隐藏选项 `CONFIG_MODBUS_SERVER`：

```c
if (!IS_ENABLED(CONFIG_MODBUS_SERVER)) {
    LOG_ERR("Modbus server support is not enabled");
```

`CONFIG_MODBUS_SERVER` 是自动推导的：

```kconfig
config MODBUS_SERVER
    bool
    default y if MODBUS_ROLE_SERVER || MODBUS_ROLE_CLIENT_SERVER
```

因为 `ROLE_SERVER` 没生效，所以 `CONFIG_MODBUS_SERVER` 也没有启用。

### 3.3 教训

- Zephyr Kconfig 的 `choice` 块是互斥选择，不能同时写两个成员
- 遇到 `choice` 选项时，查看 Kconfig 定义确认是否有"全选"选项
- 可以检查 `build/zephyr/.config` 文件确认最终生效的配置

### 3.4 其他 Kconfig 选项

| 选项 | 说明 |
|------|------|
| `CONFIG_MODBUS_SERIAL=y` | 串口传输支持（默认 y，依赖 SERIAL） |
| `CONFIG_MODBUS_SERIAL_ASYNC_API=y` | 使用 UART Async API（依赖 UART_ASYNC_API） |
| `CONFIG_MODBUS_ASCII_MODE=y` | ASCII 传输模式 |
| `CONFIG_MODBUS_FP_EXTENSIONS=y` | 浮点寄存器支持（默认 y） |
| `CONFIG_MODBUS_RAW_ADU=y` | 原始 ADU 访问 |
| `CONFIG_MODBUS_FC08_DIAGNOSTIC=y` | FC08 诊断支持（依赖 MODBUS_SERVER，默认 y） |
| `CONFIG_MODBUS_NONCOMPLIANT_SERIAL_MODE=y` | 允许非标准停止位/校验位 |

---

## 4. DTS 多接口配置

### 4.1 基本配置

在 UART 节点下添加 `zephyr,modbus-serial` 子节点：

```dts
&usart3 {
    pinctrl-0 = <&usart3_tx_pb10 &usart3_rx_pb11>;
    pinctrl-names = "default";
    current-speed = <115200>;
    dmas = <&dma1 3 4 STM32_DMA_PERIPH_TX STM32_DMA_FIFO_FULL>,
           <&dma1 1 4 STM32_DMA_PERIPH_RX STM32_DMA_FIFO_FULL>;
    dma-names = "tx", "rx";
    status = "okay";

    modbus0 {
        compatible = "zephyr,modbus-serial";
        status = "okay";
    };
};

&uart4 {
    pinctrl-0 = <&uart4_tx_pc10 &uart4_rx_pc11>;
    pinctrl-names = "default";
    current-speed = <115200>;
    dmas = <&dma1 4 4 STM32_DMA_PERIPH_TX STM32_DMA_FIFO_FULL>,
           <&dma1 2 4 STM32_DMA_PERIPH_RX STM32_DMA_FIFO_FULL>;
    dma-names = "tx", "rx";
    status = "okay";

    modbus1 {
        compatible = "zephyr,modbus-serial";
        status = "okay";
    };
};
```

### 4.2 RS485 DE 引脚（可选）

如果使用 RS485 收发器，需要配置 DE（Driver Enable）引脚：

```dts
modbus0 {
    compatible = "zephyr,modbus-serial";
    status = "okay";
    de-gpios = <&gpioa 8 GPIO_ACTIVE_HIGH>;
};
```

TTL 直连（如本项目的回环测试）可省略。

### 4.3 多接口的节点引用问题

**问题**：例程使用 `DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)` 获取节点，
但这个宏**只返回第一个** status="okay" 的节点，无法区分 modbus0 和 modbus1。

**解决方案**：使用 `DT_CHILD()` 精确指定节点：

```c
/* modbus0 是 usart3 的子节点，modbus1 是 uart4 的子节点 */
#define MODBUS_SERVER_NODE DT_CHILD(DT_NODELABEL(usart3), modbus0)
#define MODBUS_CLIENT_NODE DT_CHILD(DT_NODELABEL(uart4), modbus1)
```

**映射关系**：

```
DTS 节点 modbus0/modbus1
    ↓ (DT_INST_FOREACH_STATUS_OKAY 宏展开)
modbus_core.c 的 mb_ctx_tbl[] 数组
    ↓ (DEVICE_DT_NAME(DT_DRV_INST(inst)) 生成 iface_name)
mb_ctx_tbl[0].iface_name = "modbus0"   ← usart3 (INST_0)
mb_ctx_tbl[1].iface_name = "modbus1"   ← uart4  (INST_1)
    ↓
modbus_iface_get_by_name("modbus0") → 返回 index 0
modbus_iface_get_by_name("modbus1") → 返回 index 1
```

**替代方案**：在 DTS 中给节点加 `label`，然后用 `DT_NODELABEL()`：

```dts
modbus0 {
    compatible = "zephyr,modbus-serial";
    status = "okay";
    label = "MODBUS_SRV";
};
```

```c
#define MODBUS_SERVER_NODE DT_NODELABEL(modbus_srv)
```

---

## 5. 服务端实现

### 5.1 回调函数与功能码对应关系

服务端通过回调函数响应客户端请求。每个回调对应一个或多个 Modbus 功能码：

| 回调函数 | 功能码 | 名称 | 客户端 API |
|---------|--------|------|-----------|
| `coil_rd` | FC01 | Read Coils | `modbus_read_coils()` |
| `coil_wr` | FC05 / FC15 | Write Single/Multiple Coil | `modbus_write_coil()` / `modbus_write_coils()` |
| `discrete_input_rd` | FC02 | Read Discrete Inputs | `modbus_read_discrete_inputs()` |
| `input_reg_rd` | FC04 | Read Input Registers | `modbus_read_input_regs()` |
| `holding_reg_rd` | FC03 | Read Holding Registers | `modbus_read_holding_regs()` |
| `holding_reg_wr` | FC06 / FC16 | Write Single/Multiple Holding Reg | `modbus_write_holding_reg()` / `modbus_write_holding_regs()` |
| `holding_reg_rd_fp` | FC03 | Read Holding Registers (float) | `modbus_read_holding_regs_fp()` |
| `holding_reg_wr_fp` | FC16 | Write Holding Registers (float) | `modbus_write_holding_regs_fp()` |

**注意**：一个回调可能被多个功能码触发。例如 `coil_wr` 同时响应 FC05 和 FC15。

### 5.2 回调函数参数

- **addr**：客户端请求的寄存器/线圈起始地址
- **reg/state**：输入参数为指向值的指针（读回调时填入值），或直接传入值（写回调）
- **返回值**：0 表示成功，负数表示错误（如 `-ENOTSUP` 表示地址不支持）

### 5.3 实现示例

```c
/* 寄存器地址映射 */
#define REG_ADDR_TEMP 0
#define REG_ADDR_HUMI 1

static uint16_t holding_regs[2] = { 250, 600 };  /* 25.0°C, 60.0% */

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
    if (addr >= ARRAY_SIZE(holding_regs)) {
        return -ENOTSUP;  /* 地址越界 */
    }
    *reg = holding_regs[addr];
    return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
    if (addr >= ARRAY_SIZE(holding_regs)) {
        return -ENOTSUP;
    }
    holding_regs[addr] = reg;
    return 0;
}
```

### 5.4 注册回调

```c
static struct modbus_user_callbacks mbs_cbs = {
    .coil_rd = coil_rd,
    .coil_wr = coil_wr,
    .holding_reg_rd = holding_reg_rd,
    .holding_reg_wr = holding_reg_wr,
};

const static struct modbus_iface_param server_param = {
    .mode = MODBUS_MODE_RTU,
    .server = {
        .user_cb = &mbs_cbs,
        .unit_id = 1,          /* 从站地址 */
    },
    .serial = {
        .baud = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
    },
};
```

**unit_id 说明**：
- Modbus 是一主多从的总线协议，每个从站有唯一的 unit_id（1~247）
- 客户端请求帧中携带 unit_id，总线上所有从站都会收到
- 但只有 unit_id 匹配的从站才会响应
- 0 为广播地址，所有从站都处理但不响应

---

## 6. 客户端实现

### 6.1 客户端参数

```c
const static struct modbus_iface_param client_param = {
    .mode = MODBUS_MODE_RTU,
    .rx_timeout = 50000,    /* 接收超时 50ms */
    .serial = {
        .baud = 115200,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
    },
};
```

**rx_timeout**：客户端等待服务端响应的超时时间（微秒）。超时后 API 返回错误。

### 6.2 读取 Holding Registers (FC03)

```c
uint16_t reg_buf[2];
int err;

err = modbus_read_holding_regs(
    client_iface,       /* 接口索引 */
    server_unit_id,     /* 目标从站地址 */
    REG_ADDR_TEMP,      /* 寄存器起始地址 */
    reg_buf,            /* 接收缓冲区 */
    2                   /* 读取寄存器数量 */
);
```

参数说明：
- **client_iface**：客户端接口索引，由 `modbus_iface_get_by_name()` 获取
- **server_unit_id**：目标从站的 unit_id，必须与服务端配置一致
- **REG_ADDR_TEMP**：寄存器起始地址，服务端 `holding_reg_rd()` 会被逐个调用
- **reg_buf**：接收缓冲区，需至少能容纳 `num_regs` 个 `uint16_t`
- **2**：读取的寄存器数量

### 6.3 写入 Holding Registers (FC16)

```c
uint16_t values[2] = { 300, 650 };

err = modbus_write_holding_regs(
    client_iface,
    server_unit_id,
    REG_ADDR_TEMP,
    values,
    2
);
```

### 6.4 其他客户端 API

| API | 功能码 | 说明 |
|-----|--------|------|
| `modbus_read_coils()` | FC01 | 读线圈 |
| `modbus_read_discrete_inputs()` | FC02 | 读离散输入 |
| `modbus_read_holding_regs()` | FC03 | 读保持寄存器 |
| `modbus_read_input_regs()` | FC04 | 读输入寄存器 |
| `modbus_write_coil()` | FC05 | 写单个线圈 |
| `modbus_write_holding_reg()` | FC06 | 写单个保持寄存器 |
| `modbus_write_coils()` | FC15 | 写多个线圈 |
| `modbus_write_holding_regs()` | FC16 | 写多个保持寄存器 |
| `modbus_read_holding_regs_fp()` | FC03 | 读浮点保持寄存器 |
| `modbus_write_holding_regs_fp()` | FC16 | 写浮点保持寄存器 |

---

## 7. 初始化流程

### 7.1 完整初始化代码

```c
static int server_iface;
static int client_iface;

static int init_modbus_server(void)
{
    const char iface_name[] = {DEVICE_DT_NAME(MODBUS_SERVER_NODE)};

    server_iface = modbus_iface_get_by_name(iface_name);
    if (server_iface < 0) {
        return server_iface;
    }

    return modbus_init_server(server_iface, server_param);
}

static int init_modbus_client(void)
{
    const char iface_name[] = {DEVICE_DT_NAME(MODBUS_CLIENT_NODE)};

    client_iface = modbus_iface_get_by_name(iface_name);
    if (client_iface < 0) {
        return client_iface;
    }

    return modbus_init_client(client_iface, client_param);
}

int init_modbus_test(void)
{
    /* 1. 检查 UART 设备就绪 */
    const struct device *const srv_dev =
        DEVICE_DT_GET(DT_PARENT(MODBUS_SERVER_NODE));
    const struct device *const cli_dev =
        DEVICE_DT_GET(DT_PARENT(MODBUS_CLIENT_NODE));

    if (!device_is_ready(srv_dev) || !device_is_ready(cli_dev)) {
        return -1;
    }

    /* 2. 初始化服务端 */
    if (init_modbus_server() != 0) {
        return -1;
    }

    /* 3. 初始化客户端 */
    if (init_modbus_client() != 0) {
        return -1;
    }

    return 0;
}
```

### 7.2 初始化顺序

```
1. device_is_ready()          ← 检查 UART 硬件就绪
2. modbus_iface_get_by_name() ← 通过接口名获取索引号
3. modbus_init_server()       ← 初始化服务端角色
4. modbus_init_client()       ← 初始化客户端角色
```

### 7.3 关键说明

- **必须先初始化服务端再初始化客户端**：服务端需要先注册回调，才能正确响应客户端请求
- **iface 索引号**：`modbus_iface_get_by_name()` 返回的是 `mb_ctx_tbl[]` 数组的索引，
  后续所有 API 调用都使用这个索引
- **DT_PARENT**：`DT_PARENT(MODBUS_SERVER_NODE)` 获取 modbus0 的父节点（usart3），
  用于检查 UART 设备是否就绪

---

## 8. CRC 校验机制

### 8.1 自动 CRC

Zephyr Modbus RTU 子系统**自动处理 CRC16 校验**，应用层无需关心：

- **发送时**：`modbus_serial_tx_adu()` 自动计算 CRC16 并追加到帧尾
- **接收时**：`modbus_rtu_rx_adu()` 自动校验 CRC16

### 8.2 CRC 校验失败的行为

```
接收帧 → modbus_rtu_rx_adu() 校验 CRC
    │
    ├─ CRC 正确 → rx_adu_err = 0 → 正常处理
    │
    └─ CRC 错误 → rx_adu_err = -EIO → 帧被静默丢弃
                                        不触发任何回调
                                        不发送任何响应
```

驱动源码（`modbus_serial.c`）：

```c
if (ctx->rx_adu.crc != calc_crc) {
    LOG_WRN("Calculated CRC does not match received CRC");
    return -EIO;
}
```

服务端处理（`modbus_server.c`）：

```c
if (ctx->rx_adu_err != 0) {
    if (ctx->rx_adu_err == -EIO) {
        update_crcerr_ctr(ctx);  /* 仅更新统计计数器 */
    }
    return false;  /* 不响应，静默丢弃 */
}
```

### 8.3 为什么没有 CRC 失败回调？

这是 **Modbus 协议规范**要求的：CRC 错误 = 静默丢弃，不做任何响应。
如果对错误帧做出响应，攻击者可以通过伪造 CRC 来探测总线上的设备。

### 8.4 客户端如何感知 CRC 错误

客户端调用 `modbus_read_holding_regs()` 时：
- CRC 正确 + 服务端正常响应 → 返回 0
- CRC 错误 → 服务端不响应 → 客户端等待 `rx_timeout` 后返回超时错误
- 传输层 CRC 错误对客户端表现为"超时"，而非明确的"CRC 错误"

---

## 9. 自定义功能码

### 9.1 标准功能码

标准功能码（FC01~FC16）通过填充 `modbus_user_callbacks` 结构体即可，
子系统自动根据收到的 FC 码分发到对应回调。

### 9.2 自定义功能码

自定义功能码（FC >= 128，即 bit7=1）需要单独注册：

```c
/* 自定义回调函数 */
static bool my_custom_fc_cb(const int iface,
                            const struct modbus_adu *rx_adu,
                            struct modbus_adu *tx_adu,
                            uint8_t *excep_code,
                            void *user_data)
{
    /* 解析 rx_adu 中的请求数据 */
    /* 填充 tx_adu 中的响应数据 */
    /* 设置 excep_code（出错时） */

    return true;  /* true = 发送响应, false = 不响应 */
}

/* 定义自定义功能码结构体 */
MODBUS_CUSTOM_FC_DEFINE(my_fc, my_custom_fc_cb, 0x80, NULL);

/* 注册到接口 */
modbus_register_user_fc(iface, &modbus_cfg_my_fc);
```

参数说明：
- `0x80`：自定义功能码编号（必须 >= 128）
- `NULL`：传递给回调的 user_data
- `rx_adu`：收到的请求 ADU（Application Data Unit）
- `tx_adu`：要发送的响应 ADU，回调中填充

---

## 10. LOG_MODULE_REGISTER 日志系统

### 10.1 用法

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modbus_test, LOG_LEVEL_INF);
```

向 Zephyr 日志子系统注册一个名为 `"modbus_test"` 的日志模块，
默认日志级别为 `LOG_LEVEL_INF`。

### 10.2 日志级别

| 级别 | 宏 | 说明 |
|------|---|------|
| 1 | `LOG_LEVEL_ERR` | 仅错误 |
| 2 | `LOG_LEVEL_WRN` | 警告 + 错误 |
| 3 | `LOG_LEVEL_INF` | 信息 + 警告 + 错误 |
| 4 | `LOG_LEVEL_DBG` | 全部（调试 + 信息 + 警告 + 错误） |

### 10.3 输出宏

```c
LOG_ERR("错误码 %d", err);    /* [err] modbus_test: 错误码 -5 */
LOG_WRN("警告");               /* [wrn] modbus_test: 警告 */
LOG_INF("温度=%d", temp);      /* [inf] modbus_test: 温度=25 */
LOG_DBG("调试信息");           /* INF 级别下不输出 */
```

### 10.4 运行时修改

如果启用了 shell，可以动态修改日志级别：

```
log level set modbus_test debug    → 切换到 DBG 级别
log level set modbus_test info     → 切回 INF 级别
```

---

## 11. 踩坑记录

### 11.1 Kconfig choice 互斥覆盖

**现象**：`Modbus server support is not enabled`

**原因**：`prj.conf` 同时写了 `CONFIG_MODBUS_ROLE_SERVER=y` 和
`CONFIG_MODBUS_ROLE_CLIENT=y`，后写的覆盖了前者。

**修复**：使用 `CONFIG_MODBUS_ROLE_CLIENT_SERVER=y`

详见 [第 3 节](#3-kconfig-配置与踩坑)。

### 11.2 DT_COMPAT_GET_ANY_STATUS_OKAY 只取第一个节点

**现象**：客户端和服务端都初始化到同一个 UART 接口

**原因**：`DT_COMPAT_GET_ANY_STATUS_OKAY` 只返回第一个 status="okay" 的节点

**修复**：使用 `DT_CHILD(DT_NODELABEL(usart3), modbus0)` 精确指定

详见 [第 4.3 节](#43-多接口的节点引用问题)。

### 11.3 Modbus 与 UART Async API 冲突

如果同时使用 Modbus 子系统和手动配置的 UART Async API（`uart_rx_enable` 等），
两者会争夺同一 UART 设备的控制权。

解决方案：
- 方案一：使用 `CONFIG_MODBUS_SERIAL_ASYNC_API=y`，让 Modbus 子系统使用 Async API
- 方案二：Modbus 占用的 UART 不再手动调用 Async API，由 Modbus 子系统全权管理

---

## 12. 文件索引

| 文件 | 说明 |
|------|------|
| `code/blinky_test/src/modbus_test.c` | Modbus 服务端/客户端实现 |
| `code/blinky_test/src/modbus_test.h` | Modbus 模块接口定义 |
| `code/blinky_test/prj.conf` | Kconfig 配置 |
| `zephyr/boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts` | DTS Modbus 节点配置 |
| `zephyr/subsys/modbus/Kconfig` | Modbus Kconfig 定义（choice 块） |
| `zephyr/subsys/modbus/modbus_core.c` | Modbus 核心：接口管理、初始化 |
| `zephyr/subsys/modbus/modbus_server.c` | Modbus 服务端：请求处理、回调分发 |
| `zephyr/subsys/modbus/modbus_client.c` | Modbus 客户端：请求构造、响应解析 |
| `zephyr/subsys/modbus/modbus_serial.c` | Modbus 串口层：CRC 校验、帧收发 |
| `zephyr/include/zephyr/modbus/modbus.h` | Modbus 公共 API 声明 |
