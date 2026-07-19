# SPDX-License-Identifier: Apache-2.0

# =============================================================================
# Zephyr board runner 配置 — STM32F407VET6 DevEBox
# =============================================================================
#
# 本文件定义 west flash / west debug 使用的烧录器和调试器参数。
# Zephyr 支持多种 runner，按优先级从上到下依次为：
#   stm32cubeprogrammer → openocd → jlink → stlink_gdbserver
# 可通过 west flash --runner <name> 指定使用哪个。
#
# ---- STM32CubeProgrammer 参数说明 ----
# --port=swd          : 使用 SWD 接口连接（可选: swd, jtag, /dev/ttyS0, usb1）
# --frequency=480     : SWD 通信频率 480KHz
#                       默认值通常为 1800KHz 或 2400KHz，ST-LINK V2 克隆版
#                       在高频下通信不稳定，降至 480KHz 可提高可靠性
#                       可选值: 100~4800 (KHz)，需为 100 的整数倍
# --reset-mode=sw     : 软件复位模式
#                       sw  = SWrst  (软件复位，通过 SWD 协议发送复位命令)
#                       hw  = HWrst  (硬件复位，需要 NRST 物理连线)
#                       core = Crst  (内核复位，仅复位 CPU 内核)
#                       【重要】使用 hw 模式需要 ST-LINK 连接 NRST 引脚，
#                       若只接了 SWCLK/SWDIO/GND 三根线，必须使用 sw 模式。
#                       但 sw 模式要求固件中启用 CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y，
#                       否则 CPU 进入 Sleep 后 SWD 端口不可用，软件复位会失败。
#
# ---- 其他可选参数 (通过 --conn-modifiers 传递) ----
# --conn-modifiers=mode=hotplug   : 热插拔模式，不 halt CPU 直接连接
#                                    【注意】hotplug 模式下 CPU 仍在运行，
#                                    擦除 Flash 会冲突导致 "failed to erase memory"，
#                                    不推荐用于烧录，仅适合读取芯片信息
# --conn-modifiers=mode=UR        : Under Reset 模式，连接时保持复位
#                                    需要 NRST 连线才有效
# --conn-modifiers=mode=normal    : 默认模式，连接时 halt CPU
#
# ---- 完整命令示例 ----
# west flash
# west flash --runner openocd
# west flash --runner stm32cubeprogrammer -- --frequency=1800
# west flash --runner stm32cubeprogrammer -- --conn-modifiers=mode=UR
# =============================================================================

# keep first
board_runner_args(stm32cubeprogrammer "--port=swd" "--frequency=480" "--reset-mode=sw")
board_runner_args(jlink "--device=STM32F407VE" "--speed=4000")

# keep first
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stlink_gdbserver.board.cmake)
