/**
 * @file    modbus_driver.h
 * @brief   多路 Modbus 驱动模块 — 可扩展架构
 *
 * 硬件映射:
 *   MODBUS_SRV_A  (modbus0) -> USART1, RS485-1, DE=PD11, 服务器
 *   MODBUS_SRV_B  (modbus1) -> USART3, RS485-2, DE=PD10, 服务器 (预留)
 *   MODBUS_CLI_A  (modbus2) -> USART6, RS485-3, DE=PG8,  客户端
 *
 * 架构:
 *   1. 业务模块 (modbus_ser_a.c, modbus_cli_a.c 等) 通过 register 接口
 *      向本驱动注册回调和参数
 *   2. modbus_drv_init_all() 统一初始化所有已注册节点
 *   3. 客户端业务模块通过 modbus_drv_get_iface() 获取接口索引,
 *      调用 Zephyr Modbus 客户端 API (modbus_read_holding_regs 等)
 *
 * DE方向控制:
 *   由设备树 modbus 节点的 de-gpios 属性配置,
 *   Modbus 子系统自动在发送前拉低DE、发送完成后拉高DE.
 *   GPIO_ACTIVE_LOW 标志匹配 TD301D485H-E 极性 (DE低=发送, DE高=接收).
 *
 * 新增节点步骤:
 *   1. 在设备树对应 UART 节点下添加 modbus 子节点 (含 de-gpios)
 *   2. 在 modbus_node_id 枚举中添加新节点
 *   3. 在 modbus_driver.c 的节点表中添加 DTS 引用和默认配置
 *   4. 新建业务模块文件, 调用 register 接口注册
 */

#ifndef MODBUS_DRIVER_H
#define MODBUS_DRIVER_H

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>

/**
 * Modbus 节点索引
 *
 * 每个节点对应设备树中的一个 zephyr,modbus-serial 子节点.
 * 新增节点时在此枚举中添加, 并在 modbus_driver.c 的节点表中添加对应配置.
 */
enum modbus_node_id {
	MODBUS_SRV_A = 0,	/* modbus0, 服务器 */
	MODBUS_CLI_A,		/* modbus1, 客户端 */
	MODBUS_NODE_COUNT	/* 节点总数 */
};



/**
 * @brief 注册服务器节点
 *
 * 业务模块在 modbus_drv_init_all() 之前调用此函数,
 * 提供回调函数和服务器参数.
 *
 * @param id      节点索引
 * @param cbs     用户回调结构体 (需持久存在, 不能是栈变量)
 * @param unit_id Modbus 从站地址 (1~247)
 * @param serial  串口参数, NULL 则使用默认值 (115200, 8N1)
 *
 * @return 0 成功, 负值 失败
 */
int modbus_drv_register_server(enum modbus_node_id id,
			       struct modbus_user_callbacks *cbs,
			       uint8_t unit_id,
			       const struct modbus_serial_param *serial);

/**
 * @brief 注册客户端节点
 *
 * 业务模块在 modbus_drv_init_all() 之前调用此函数,
 * 提供客户端参数.
 *
 * @param id            节点索引
 * @param rx_timeout_us 等待响应超时 (微秒), 建议 50000 (50ms)
 * @param serial        串口参数, NULL 则使用默认值 (115200, 8N1)
 *
 * @return 0 成功, 负值 失败
 */
int modbus_drv_register_client(enum modbus_node_id id,
			       uint32_t rx_timeout_us,
			       const struct modbus_serial_param *serial);

/**
 * @brief 初始化所有已注册的 Modbus 节点
 *
 * 按节点索引顺序依次初始化:
 *   - 服务器节点: modbus_init_server() — DE引脚由子系统自动控制
 *   - 客户端节点: modbus_init_client()
 *
 * 必须在所有 register_server/register_client 调用之后调用.
 * DE方向控制由设备树 de-gpios 属性配置, 无需手动管理.
 *
 * @return 0 全部成功, 负值 第一个失败的节点错误码
 */
int modbus_drv_init_all(void);

/**
 * @brief 获取节点的 Zephyr Modbus 接口索引
 *
 * 客户端业务代码需要此索引来调用 modbus_read_holding_regs() 等 API.
 *
 * @param id 节点索引
 * @return >=0 接口索引, 负值 节点未初始化
 */
int modbus_drv_get_iface(enum modbus_node_id id);

/**
 * @brief 检查节点是否已初始化就绪
 *
 * @param id 节点索引
 * @return true 已初始化, false 未初始化
 */
bool modbus_drv_is_ready(enum modbus_node_id id);

#endif /* MODBUS_DRIVER_H */
