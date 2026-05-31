# Zephyr RTOS 移植踩坑记录：STM32F407VET6 DevEBox 板卡支持

> 硬件: STM32F407VET6 DevEBox (正点原子/战舰V3同系列)
> 调试器: ST-LINK V2 克隆版 (仅 SWCLK/SWDIO/GND 三线)
> Zephyr: v4.4.99
> 参考板卡: stm32f4_disco

---

## 目录

1. [问题一：设备树 aliases 编译错误](#问题一设备树-aliases-编译错误)
2. [问题二：ST-LINK 无法连接芯片](#问题二st-link-无法连接芯片)
3. [问题三：Hot Plug 模式下擦除 Flash 失败](#问题三hot-plug-模式下擦除-flash-失败)
4. [最终配置汇总](#最终配置汇总)
5. [快速排查指南](#快速排查指南)

---

## 问题一：设备树 aliases 编译错误

### 现象

```
devicetree error: /aliases: undefined node label 'led_0'
CMake Error at .../dts.cmake:324 (execute_process):
  execute_process failed command indexes: 1: "Child return code: 1"
```

### 根因

设备树中 **节点标签(label)** 和 **节点名称(node-name)** 是两个不同的概念：

```dts
leds {
    led0: led_0 {          ← label 是 "led0"，node-name 是 "led_0"
        gpios = <&gpioa 6 GPIO_ACTIVE_HIGH>;
    };
};

aliases {
    led0 = &led_0;         ← 错误！& 引用的是 label，不是 node-name
};
```

`&led_0` 试图引用名为 `led_0` 的标签，但实际标签名是 `led0`（没有下划线）。

### 修复

```dts
aliases {
    led0 = &led0;          ← 正确：引用标签 led0
    led1 = &led1;          ← 正确：引用标签 led1
};
```

### 知识点：设备树 label vs node-name

```
label: node-name@unit-address { ... };
  ↑         ↑           ↑
  │         │           └─ 单元地址（可选），用于区分同类型多实例
  │         └─ 节点名称，出现在设备树路径 /leds/led_0 中
  └─ 标签，用于被 & 引用，如 &led0
```

**`&` 语法只能引用标签(label)，不能引用节点名称。**

推荐写法（参考 stm32f4_disco）：让 label 和 node-name 使用不同命名，避免混淆：

```dts
green_led_4: led_4 { ... };     /* label=green_led_4, node-name=led_4 */
aliases {
    led0 = &green_led_4;         /* 清晰：引用标签 green_led_4 */
};
```

### 涉及文件

- `boards/st/stm32f407vet6_devebox/stm32f4_devebox.dts` — aliases 节点

---

## 问题二：ST-LINK 无法连接芯片

### 现象

```
Error: Unable to get core ID
Error: No STM32 target found!
FATAL ERROR: command exited with status 1
```

必须手动按住复位键再松开，趁芯片刚复位的短暂窗口期才能烧录成功。

### 根因分析

这是一个**双层问题**：

#### 层1：board.cmake 配置了硬件复位模式，但没有 NRST 连线

```cmake
# 原配置（从 stm32f4_disco 复制来的）
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")
```

`--reset-mode=hw` (HWrst) 需要 ST-LINK 连接 NRST 引脚才能生效。我的板子只接了 SWCLK/SWDIO/GND 三根线，硬件复位根本无法执行。

#### 层2：固件未启用调试端口保持，CPU Sleep 后 SWD 不可用

这是更根本的原因。Zephyr 的 blinky 程序执行流程：

```
main() → while(1) { k_msleep(1000); toggle_led(); }
                ↓
         idle 线程执行 WFI 指令
                ↓
         CPU 进入 Sleep 模式
                ↓
  CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP 未启用
         → LL_DBGMCU_DisableDBGStopMode()
         → SWD 调试端口关闭
         → ST-LINK 无法连接！
```

关键代码在 Zephyr 的 `soc/st/stm32/common/soc_config.c`：

```c
#if defined(CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP)
    /* STM32F4 走 "all other parts" 分支 */
    LL_DBGMCU_EnableDBGStopMode();    /* 调试端口保持活跃 */
#else
    LL_DBGMCU_DisableDBGStopMode();   /* 低功耗时关闭调试端口 */
#endif
```

### 修复

**固件侧** — 在 defconfig 中启用调试端口保持：

```ini
CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y
```

**烧录器侧** — 改为软件复位 + 降低 SWD 频率：

```cmake
board_runner_args(stm32cubeprogrammer "--port=swd" "--frequency=480" "--reset-mode=sw")
```

- `--reset-mode=sw`：软件复位，通过 SWD 协议发送复位命令，不需要 NRST 连线
- `--frequency=480`：SWD 频率降至 480KHz，ST-LINK V2 克隆版在高频下通信不稳定

### 为什么之前软件复位也失败？

因为旧固件没有 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y`，CPU 进入 Sleep 后 SWD 端口不可用，软件复位命令根本发不出去。启用此选项后，SWD 端口在 Sleep 模式下保持活跃，软件复位就能正常工作了。

### 涉及文件

- `boards/st/stm32f407vet6_devebox/stm32f4_devebox_defconfig` — 新增 CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y
- `boards/st/stm32f407vet6_devebox/board.cmake` — 修改 runner 参数

---

## 问题三：Hot Plug 模式下擦除 Flash 失败

### 现象

在尝试用 `mode=hotplug` 解决问题二时，出现新问题：

```
Connect mode: Hot Plug
...
Erasing internal memory sectors [0 1]
Error: failed to erase memory
```

但紧接着再次执行 `west flash` 就成功了，稳定复现"一次失败一次成功"。

### 根因

Hot Plug 模式连接芯片时**不会 halt CPU**，CPU 仍在从 Flash 中取指令执行。此时擦除 Flash 会产生冲突——CPU 正在读取的 Flash 区域被擦除，导致操作失败。

第二次成功是因为第一次失败后 STM32CubeProgrammer 的错误恢复流程已经 halt 了 CPU。

### STM32CubeProgrammer 连接模式对比

| 模式 | 参数 | 行为 | 适用场景 |
|------|------|------|----------|
| Normal | `mode=normal` | 连接时 halt CPU | **烧录推荐** |
| Hot Plug | `mode=hotplug` | 不 halt CPU，直接连接 | 仅适合读取芯片信息，不适合烧录 |
| Under Reset | `mode=UR` | 连接时保持复位 | 需要 NRST 连线，适合芯片 SWD 被禁用时 |

### 修复

去掉 hotplug，使用软件复位模式（配合 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y`）：

```cmake
board_runner_args(stm32cubeprogrammer "--port=swd" "--frequency=480" "--reset-mode=sw")
```

生成的实际命令：
```
STM32_Programmer_CLI --connect "port=swd freq=480 reset=SWrst" --download zephyr.hex -rst
```

连接流程：`SWD 连接 → SWrst halt CPU → 擦除 Flash → 烧录 → 软件复位重启`

### 涉及文件

- `boards/st/stm32f407vet6_devebox/board.cmake` — runner 参数

---

## 最终配置汇总

### 文件结构

```
zephyr/boards/st/stm32f407vet6_devebox/
├── board.cmake                  ← 烧录器配置
├── board.yml                    ← 板卡元数据
├── Kconfig.stm32f4_devebox      ← 板卡 Kconfig
├── stm32f4_devebox.dts          ← 设备树
├── stm32f4_devebox.yaml         ← 板卡描述
├── stm32f4_devebox_defconfig    ← 默认编译配置
└── support/
    └── openocd.cfg              ← OpenOCD 配置
```

### 关键修改清单

| 文件 | 修改内容 | 原因 |
|------|----------|------|
| `stm32f4_devebox.dts` | aliases 中 `&led_0` → `&led0` | & 引用标签而非节点名 |
| `stm32f4_devebox_defconfig` | 新增 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y` | 保持 SWD 在 Sleep 模式下可用 |
| `board.cmake` | `--reset-mode=hw` → `--reset-mode=sw --frequency=480` | 无 NRST 连线，改用软件复位+低频 |
| `support/openocd.cfg` | 改用通用 stlink.cfg + stm32f4x.cfg | 原配置依赖硬件复位 |

### board.cmake 最终配置

```cmake
board_runner_args(stm32cubeprogrammer "--port=swd" "--frequency=480" "--reset-mode=sw")
board_runner_args(jlink "--device=STM32F407VE" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stlink_gdbserver.board.cmake)
```

---

## 快速排查指南

### 设备树编译错误：undefined node label

```
devicetree error: /aliases: undefined node label 'xxx'
```

**排查步骤：**
1. 找到 aliases 中 `= &xxx` 的引用
2. 搜索设备树中是否有 `xxx:` 标签定义（注意冒号左边才是标签）
3. 确认 `&` 后面跟的是标签名，不是节点名

### ST-LINK 无法连接

```
Error: Unable to get core ID / No STM32 target found
```

**排查步骤：**

```
1. 检查硬件连线
   ├── 只接了 SWCLK/SWDIO/GND？
   │   └── 必须使用 --reset-mode=sw（不能用 hw）
   └── 连接了 NRST？
       └── 可以使用 --reset-mode=hw（更可靠）

2. 检查固件配置
   ├── 是否启用 CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y？
   │   └── 未启用 → CPU Sleep 后 SWD 不可用，软件复位失败
   └── 是否启用了 CONFIG_PM（电源管理）？
       └── PM 会将 SWJ 引脚切换为模拟模式省电
           需要 CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y 或连接 NRST

3. 检查 SWD 频率
   └── ST-LINK V2 克隆版建议 --frequency=480
       正版 ST-LINK 可用更高频率

4. 尝试手动复位
   └── 按住复位键 → 执行 west flash → 松开复位键
       如果能连接，说明是 SWD 端口被固件关闭的问题
```

### Flash 擦除失败

```
Error: failed to erase memory
```

**排查步骤：**
1. 是否使用了 `mode=hotplug`？→ 改为 `mode=normal` 或去掉 conn-modifiers
2. CPU 是否仍在运行？→ 烧录前必须 halt CPU
3. Flash 是否被写保护？→ 检查选项字节(Option Bytes)

### 有 NRST 连线 vs 无 NRST 连线的配置选择

| 场景 | board.cmake 配置 | defconfig 配置 |
|------|-------------------|----------------|
| 有 NRST 连线 | `--reset-mode=hw` | 不需要 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP` |
| 无 NRST 连线 | `--reset-mode=sw --frequency=480` | **必须** `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y` |
| 无 NRST + 低功耗产品 | `--reset-mode=sw` | 发布版禁用 DEBUG_SLEEP_STOP，改用 NRST 连线烧录 |

---

## 附录：Zephyr STM32 调试端口相关代码路径

| 文件 | 作用 |
|------|------|
| `soc/st/stm32/common/soc_config.c` | DBGMCU 启停控制，根据 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP` 决定 |
| `soc/st/stm32/common/Kconfig` | 定义 `CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP` 选项 |
| `soc/st/stm32/common/pm_debug_swj.c` | PM 模式下将 SWJ 引脚切换为模拟模式省电 |
| `scripts/west_commands/runners/stm32cubeprogrammer.py` | STM32CubeProgrammer runner 脚本，构造 CLI 命令 |
| `boards/common/stm32cubeprogrammer.board.cmake` | 通用 STM32CubeProgrammer board cmake 模板 |
