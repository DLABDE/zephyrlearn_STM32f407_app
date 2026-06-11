/**
 * @file    modbus_ser_a.h
 * @brief   Modbus 服务器 A (modbus0, USART1, RS485-1) 业务模块
 *
 * 服务器角色: 响应其他设备的读取/写入访问
 * 从站地址: unit_id = 1
 *
 * 保持寄存器映射:
 *   地址 0: 温度值 (uint16, 单位 0.1°C, 即 250 = 25.0°C)
 *   地址 1: 湿度值 (uint16, 单位 0.1%, 即 600 = 60.0%)
 */

#ifndef MODBUS_SER_A_H
#define MODBUS_SER_A_H

#include <zephyr/kernel.h>

/**
 * @brief 注册 modbus0 服务器 (USART1, RS485-1)
 *
 * 向 modbus_driver 注册服务器回调和参数 (unit_id=1, 115200 8N1).
 * 必须在 modbus_drv_init_all() 之前调用.
 *
 * @return 0 成功, 负值 失败
 */
int modbus_ser_a_register(void);

/**
 * @brief 获取保持寄存器值
 *
 * @param addr 寄存器地址 (0=温度, 1=湿度)
 * @return 寄存器值, 地址越界返回 0
 */
uint16_t modbus_ser_a_get_holding_reg(uint16_t addr);

/**
 * @brief 设置保持寄存器值
 *
 * 可由主逻辑线程或其他模块调用, 更新服务器寄存器数据.
 * 服务器回调读取时会自动获取最新值.
 *
 * @param addr 寄存器地址 (0=温度, 1=湿度)
 * @param val  寄存器值
 */
void modbus_ser_a_set_holding_reg(uint16_t addr, uint16_t val);

#endif /* MODBUS_SER_A_H */
