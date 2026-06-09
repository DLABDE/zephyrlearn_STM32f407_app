#ifndef __IIC_BOARD_H__
#define __IIC_BOARD_H__

#include <stdint.h>

/*
 * I2C 初始化
 *
 * 获取设备树中定义的 I2C 总线设备句柄，并验证总线是否就绪。
 * 必须在使用任何 I2C 操作之前调用。
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_init(void);

/*
 * I2C 总线扫描
 *
 * 扫描 I2C 总线上所有可能的 7 位地址 (0x03 ~ 0x77)，
 * 打印检测到的设备地址。常用于硬件调试，确认设备连接正确。
 *
 * I2C 地址范围说明:
 *   0x00~0x02: 保留（广播/起始字节/CBUS）
 *   0x03~0x07: 保留
 *   0x08~0x77: 用户可用地址（共 112 个）
 *   0x78~0x7F: 10 位地址扩展保留
 *
 * 返回值: 扫描到的设备数量, 被负的错误码失败
 */
int i2c_board_scan(void);

/*
 * I2C 写寄存器（单字节寄存器地址 + 单字节数据）
 *
 * 这是 I2C 最常见的操作模式: 先发寄存器地址，再发数据。
 * 时序: [START] [设备地址+W] [寄存器地址] [数据] [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址（不含 R/W 位）
 *   reg_addr  — 寄存器地址（1 字节）
 *   value     — 要写入的数据（1 字节）
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t value);

/*
 * I2C 读寄存器（单字节寄存器地址 + 单字节数据）
 *
 * 时序: [START] [设备地址+W] [寄存器地址]
 *       [RESTART] [设备地址+R] [读取数据] [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址（不含 R/W 位）
 *   reg_addr  — 寄存器地址（1 字节）
 *   value     — 输出: 读取到的数据
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value);

/*
 * I2C 写多字节（寄存器地址 + 多字节数据）
 *
 * 常用于: 写 FIFO、批量配置寄存器、OLED 显存写入等。
 * 时序: [START] [设备地址+W] [寄存器地址] [数据0] [数据1] ... [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址
 *   reg_addr  — 寄存器地址（1 字节）
 *   data      — 要写入的数据缓冲区
 *   len       — 数据长度（字节）
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_write_burst(uint8_t dev_addr, uint8_t reg_addr,
			  const uint8_t *data, uint16_t len);

/*
 * I2C 读多字节（寄存器地址 + 多字节数据）
 *
 * 常用于: 读 FIFO、批量读取传感器数据等。
 * 时序: [START] [设备地址+W] [寄存器地址]
 *       [RESTART] [设备地址+R] [数据0] [数据1] ... [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址
 *   reg_addr  — 寄存器地址（1 字节）
 *   data      — 输出: 读取到的数据缓冲区
 *   len       — 要读取的字节数
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_read_burst(uint8_t dev_addr, uint8_t reg_addr,
			 uint8_t *data, uint16_t len);

/*
 * I2C 纯写（无寄存器地址，直接发数据）
 *
 * 常用于: OLED SSD1306 等设备的命令/数据写入（通过控制字节区分）。
 * 时序: [START] [设备地址+W] [数据0] [数据1] ... [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址
 *   data      — 要发送的数据缓冲区
 *   len       — 数据长度（字节）
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len);

/*
 * I2C 纯读（无寄存器地址，直接读数据）
 *
 * 常用于: 读取设备 ID、读取 FIFO 等无需指定寄存器地址的场景。
 * 时序: [START] [设备地址+R] [数据0] [数据1] ... [STOP]
 *
 * 参数:
 *   dev_addr  — 7 位设备地址
 *   data      — 输出: 读取到的数据缓冲区
 *   len       — 要读取的字节数
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len);

/*
 * I2C 综合测试
 *
 * 执行: 初始化 → 总线扫描 → 对 0x3C 设备尝试读取
 * 用于验证 I2C 硬件连接和驱动配置是否正确。
 *
 * 返回值: 0 成功, 被负的错误码失败
 */
int i2c_board_test(void);

#endif /* __IIC_BOARD_H__ */
