/**
 * @file    modbus_cli_a.h
 * @brief   Modbus 客户端 A (modbus2, USART6, RS485-3) 业务模块
 *
 * 客户端角色: 周期性读取远端 Modbus 设备的保持寄存器
 * 目标设备: unit_id=1 (与 modbus0 服务器同地址, 需物理连接 RS485-1 和 RS485-3)
 *
 * 线程模型:
 *   客户端读取 API (modbus_read_holding_regs 等) 是阻塞调用,
 *   通过 k_sem_take 等待响应, 超时由 rx_timeout_us 决定.
 *   因此使用独立线程进行周期性读取, 不阻塞主逻辑线程.
 */

#ifndef MODBUS_CLI_A_H
#define MODBUS_CLI_A_H

#include <zephyr/kernel.h>

/**
 * @brief 注册 modbus2 客户端 (USART6, RS485-3)
 *
 * 向 modbus_driver 注册客户端参数 (rx_timeout=50ms, 115200 8N1).
 * 必须在 modbus_drv_init_all() 之前调用.
 *
 * @return 0 成功, 负值 失败
 */
int modbus_cli_a_register(void);

/**
 * @brief 启动 modbus2 客户端读取线程
 *
 * 创建独立线程, 周期性读取远端设备的保持寄存器.
 * 必须在 modbus_drv_init_all() 之后调用.
 *
 * @return 0 成功, 负值 失败
 */
int modbus_cli_a_start(void);

#endif /* MODBUS_CLI_A_H */
