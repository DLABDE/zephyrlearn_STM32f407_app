/**
 * @file    modbus_driver.c
 * @brief   多路 Modbus 驱动模块 — 可扩展架构实现
 *
 * 职责:
 *   - 管理所有 Modbus 节点的注册和初始化
 *   - 将设备树 DTS 节点映射到运行时接口索引
 *   - 提供 get_iface / is_ready 查询接口
 *
 * DE方向控制:
 *   由设备树 de-gpios 属性 + GPIO_ACTIVE_LOW 标志实现,
 *   Modbus 子系统在 modbus_serial_tx_on() 时拉低DE (发送),
 *   在 modbus_serial_tx_off() 时拉高DE (接收).
 *   无需在此处手动控制DE引脚.
 *
 * 线程模型:
 *   - 服务器: 由系统工作队列处理请求, 通过用户回调访问寄存器, 不阻塞用户线程
 *   - 客户端: modbus_read_holding_regs() 等API是阻塞调用,
 *             业务模块应创建独立线程进行周期性读取
 */

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>
#include "modbus_driver.h"

/*
 * 向 Zephyr 日志子系统注册一个名为 "modbus_driver" 的日志模块,
 * 默认日志级别为 LOG_LEVEL_INF (只输出 INF/WARN/ERR 级别).
 *
 * 使用方法:
 *   LOG_INF("温度=%d", temp);    → 输出 [inf] modbus_test: 温度=25
 *   LOG_WRN("警告");             → 输出 [wrn] modbus_test: 警告
 *   LOG_ERR("错误码 %d", err);   → 输出 [err] modbus_test: 错误码 -5
 *   LOG_DBG("调试信息");         → INF 级别下不会输出, 需 LOG_LEVEL_DBG
 *
 * 日志级别从低到高: DBG → INF → WRN → ERR
 * 级别越高, 输出越少; 设为 INF 表示只显示 INF 及以上级别
 *
 * 运行时可通过 shell 动态修改: log level set modbus_driver debug
 */
LOG_MODULE_REGISTER(modbus_driver, LOG_LEVEL_INF);


/** 默认串口参数: 115200bps, 8N1 */
#define MODBUS_SERIAL_DEFAULT { \
	.baud = 115200, \
	.parity = UART_CFG_PARITY_NONE, \
	.stop_bits = UART_CFG_STOP_BITS_1, \
}


/* ========================================================================
 * 设备树节点引用
 * ========================================================================
 * 每个节点对应 DTS 中 UART 下的 modbus-serial 子节点.
 * 新增节点时在此添加 DTS 宏定义.
 */
/*
 * DTS 中定义了两个 Modbus Serial 节点:
 *   usart3 { modbus0 { compatible = "zephyr,modbus-serial"; status = "okay"; }; };
 *   uart4  { modbus1 { compatible = "zephyr,modbus-serial"; status = "okay"; }; };
 *
 * DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial) 只会返回
 * 第一个 status="okay" 的节点 (即 modbus0 / usart3),
 * 无法区分 modbus0 和 modbus1.
 *
 * 正确做法: 用 DT_NODELABEL() 或 DT_PATH() 精确指定节点.
 * 但 DTS 中 modbus0/modbus1 是子节点名而非 node_label,
 * 所以使用 DT_CHILD(parent, child_name) 来获取:
 *   UART3 server -> DT_CHILD(DT_NODELABEL(usart3), modbus0)
 *   UART4 client -> DT_CHILD(DT_NODELABEL(uart4),  modbus1)
 *
 * 也可以在 DTS 中给节点加 label, 然后用 DT_NODELABEL(label).
 */
#define MODBUS_SERVER_NODE DT_CHILD(DT_NODELABEL(usart3), modbus0)
#define MODBUS_CLIENT_NODE DT_CHILD(DT_NODELABEL(uart4), modbus1)

/* ========================================================================
 * 节点配置表
 * ========================================================================
 * 静态定义所有节点的 DTS 映射和默认属性.
 * 运行时状态 (registered, initialized, iface) 由注册和初始化流程填充.
 */

struct modbus_node {
	const char *name;	/* DTS 设备名, 用于 modbus_iface_get_by_name() */
	bool is_server;		/* true=服务器, false=客户端 */
	bool registered;	/* 业务代码是否已注册 */
	bool initialized;	/* 是否初始化成功 */
	int iface;		/* 运行时接口索引 (<0 表示无效) */
	union {
		struct {
			struct modbus_user_callbacks *cbs;
			uint8_t unit_id;
		} server;
		struct {
			uint32_t rx_timeout_us;
		} client;
	};
	struct modbus_serial_param serial;
};

/* 默认串口参数 */
static const struct modbus_serial_param serial_default = MODBUS_SERIAL_DEFAULT;

/*
 * 节点表: 索引与 modbus_node_id 枚举一一对应
 *
 * name 字段通过 DEVICE_DT_NAME() 宏从 DTS 节点获取设备名,
 * 该名称在 modbus_iface_get_by_name() 中用于查找接口索引.
 *
 * 新增节点时在此数组中添加一项.
 */
static struct modbus_node nodes[MODBUS_NODE_COUNT] = {
	[MODBUS_SRV_A] = {
		.name = DEVICE_DT_NAME(MODBUS_SERVER_NODE),
		.is_server = true,
	},
	[MODBUS_CLI_A] = {
		.name = DEVICE_DT_NAME(MODBUS_CLIENT_NODE),
		.is_server = false,
	},
};

/* ========================================================================
 * 注册接口
 * ========================================================================
 * 业务模块在 modbus_drv_init_all() 之前调用, 提供回调和参数.
 */

int modbus_drv_register_server(enum modbus_node_id id,
			       struct modbus_user_callbacks *cbs,
			       uint8_t unit_id,
			       const struct modbus_serial_param *serial)
{
	if (id < 0 || id >= MODBUS_NODE_COUNT) {
		return -EINVAL;
	}
	if (!nodes[id].is_server) {
		LOG_ERR("Node %d is not a server node", id);
		return -EINVAL;
	}
	if (cbs == NULL) {
		return -EINVAL;
	}

	nodes[id].server.cbs = cbs;
	nodes[id].server.unit_id = unit_id;
	nodes[id].serial = serial ? *serial : serial_default;
	nodes[id].registered = true;

	LOG_INF("Server node %d registered (unit_id=%u, baud=%u)",
		id, unit_id, nodes[id].serial.baud);
	return 0;
}

int modbus_drv_register_client(enum modbus_node_id id,
			       uint32_t rx_timeout_us,
			       const struct modbus_serial_param *serial)
{
	if (id < 0 || id >= MODBUS_NODE_COUNT) {
		return -EINVAL;
	}
	if (nodes[id].is_server) {
		LOG_ERR("Node %d is not a client node", id);
		return -EINVAL;
	}

	nodes[id].client.rx_timeout_us = rx_timeout_us;
	nodes[id].serial = serial ? *serial : serial_default;
	nodes[id].registered = true;

	LOG_INF("Client node %d registered (rx_timeout=%uus, baud=%u)",
		id, rx_timeout_us, nodes[id].serial.baud);
	return 0;
}

/* ========================================================================
 * 初始化
 * ========================================================================
 * 遍历节点表, 对已注册的节点调用 Zephyr Modbus 初始化 API.
 *
 * 服务器初始化流程:
 *   modbus_iface_get_by_name() → modbus_init_server()
 *   初始化后服务器自动在工作队列线程中处理请求, 无需用户代码参与.
 *
 * 客户端初始化流程:
 *   modbus_iface_get_by_name() → modbus_init_client()
 *   初始化后客户端可调用 modbus_read_holding_regs() 等阻塞API.
 *
 * DE引脚控制:
 *   由 modbus_serial 驱动自动管理:
 *     - 发送前: gpio_pin_set_dt(de, 1) → GPIO_ACTIVE_LOW → 物理低电平 → 发送模式
 *     - 发送后: gpio_pin_set_dt(de, 0) → GPIO_ACTIVE_LOW → 物理高电平 → 接收模式
 *     - 初始:   GPIO_OUTPUT_INACTIVE   → GPIO_ACTIVE_LOW → 物理高电平 → 接收模式
 */

int modbus_drv_init_all(void)
{
	int first_err = 0;

	for (int i = 0; i < MODBUS_NODE_COUNT; i++) {
		struct modbus_node *n = &nodes[i];

		/* 跳过未注册的节点 (如预留的 MODBUS_SRV_B) */
		if (!n->registered) {
			LOG_INF("Node %d not registered, skipping", i);
			continue;
		}

		/* 通过 DTS 设备名获取 Zephyr Modbus 接口索引 */
		n->iface = modbus_iface_get_by_name(n->name);
		if (n->iface < 0) {
			LOG_ERR("Node %d: failed to get iface for '%s' (%d)",
				i, n->name, n->iface);
			if (first_err == 0) {
				first_err = n->iface;
			}
			continue;
		}

		/* 根据节点类型调用对应的初始化函数 */
		int ret;

		if (n->is_server) {
			const struct modbus_iface_param param = {
				.mode = MODBUS_MODE_RTU,
				.server = {
					.user_cb = n->server.cbs,
					.unit_id = n->server.unit_id,
				},
				.serial = n->serial,
			};
			ret = modbus_init_server(n->iface, param);
		} else {
			const struct modbus_iface_param param = {
				.mode = MODBUS_MODE_RTU,
				.rx_timeout = n->client.rx_timeout_us,
				.serial = n->serial,
			};
			ret = modbus_init_client(n->iface, param);
		}

		if (ret != 0) {
			LOG_ERR("Node %d: init failed (%d)", i, ret);
			n->iface = ret;
			if (first_err == 0) {
				first_err = ret;
			}
		} else {
			n->initialized = true;
			LOG_INF("Node %d: initialized as %s (iface=%d, unit_id=%u)",
				i, n->is_server ? "server" : "client",
				n->iface,
				n->is_server ? n->server.unit_id : 0);
		}
	}

	return first_err;
}

/* ========================================================================
 * 查询接口
 * ========================================================================
 */

int modbus_drv_get_iface(enum modbus_node_id id)
{
	if (id < 0 || id >= MODBUS_NODE_COUNT) {
		return -EINVAL;
	}
	if (!nodes[id].initialized) {
		return -ENODEV;
	}
	return nodes[id].iface;
}

bool modbus_drv_is_ready(enum modbus_node_id id)
{
	if (id < 0 || id >= MODBUS_NODE_COUNT) {
		return false;
	}
	return nodes[id].initialized;
}
