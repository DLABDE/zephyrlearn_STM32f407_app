#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include "modbus_test.h"

/*
 * LOG_MODULE_REGISTER(modbus_test, LOG_LEVEL_INF):
 *
 * 向 Zephyr 日志子系统注册一个名为 "modbus_test" 的日志模块,
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
 * 运行时可通过 shell 动态修改: log level set modbus_test debug
 */
LOG_MODULE_REGISTER(modbus_test, LOG_LEVEL_INF);

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

/*
 * Modbus 寄存器地址映射 (服务端)
 *
 * 地址 0: 温度值 (uint16, 单位 0.1°C, 即 255 = 25.5°C)
 * 地址 1: 湿度值 (uint16, 单位 0.1%)
 */
#define REG_ADDR_TEMP 0
#define REG_ADDR_HUMI 1

static uint16_t holding_regs[2] = { 250, 600 };

/*
 * 服务端回调函数与 Modbus 功能码的对应关系:
 *
 * 回调函数              | 功能码 | 名称                          | 客户端 API
 * ---------------------|-------|-------------------------------|---------------------------
 * coil_rd              | FC01  | Read Coils                    | modbus_read_coils()
 * coil_wr              | FC05  | Write Single Coil             | modbus_write_coil()
 *                      | FC15  | Write Multiple Coils          | modbus_write_coils()
 * discrete_input_rd    | FC02  | Read Discrete Inputs          | modbus_read_discrete_inputs()
 * input_reg_rd         | FC04  | Read Input Registers          | modbus_read_input_regs()
 * holding_reg_rd       | FC03  | Read Holding Registers        | modbus_read_holding_regs()
 * holding_reg_wr       | FC06  | Write Single Holding Register | modbus_write_holding_reg()
 *                      | FC16  | Write Multiple Holding Regs   | modbus_write_holding_regs()
 * holding_reg_rd_fp    | FC03  | Read Holding Registers (float)| modbus_read_holding_regs_fp()
 * holding_reg_wr_fp    | FC16  | Write Holding Registers (float)| modbus_write_holding_regs_fp()
 *
 * 注意: 一个回调可能被多个功能码触发.
 * 例如 coil_rd 同时响应 FC01, coil_wr 同时响应 FC05 和 FC15.
 * 回调中的 addr 参数就是客户端请求的寄存器/线圈起始地址.
 */

static int coil_rd(uint16_t addr, bool *state)
{
	*state = false;
	LOG_INF("Server: coil read addr=%u", addr);
	return 0;
}

static int coil_wr(uint16_t addr, bool state)
{
	LOG_INF("Server: coil write addr=%u state=%d", addr, (int)state);
	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(holding_regs)) {
		return -ENOTSUP;
	}
	*reg = holding_regs[addr];
	LOG_INF("Server: holding reg read addr=%u val=%u", addr, *reg);
	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_regs)) {
		return -ENOTSUP;
	}
	holding_regs[addr] = reg;
	LOG_INF("Server: holding reg write addr=%u val=%u", addr, reg);
	return 0;
}

static struct modbus_user_callbacks mbs_cbs = {
	.coil_rd = coil_rd,
	.coil_wr = coil_wr,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,

	/*
	 * Q: 如果需要自定义其他功能码怎么办?
	 *
	 * A: Zephyr Modbus 提供两种机制:
	 *
	 * 1) 标准功能码: 填充 modbus_user_callbacks 中对应的回调即可,
	 *    子系统会自动根据收到的 FC 码分发到对应回调.
	 *
	 * 2) 自定义功能码 (FC >= 128, 即 bit7=1):
	 *    使用 MODBUS_CUSTOM_FC_DEFINE() + modbus_register_user_fc() 注册.
	 *    示例:
	 *      static bool my_custom_fc_cb(const int iface,
	 *                                  const struct modbus_adu *rx_adu,
	 *                                  struct modbus_adu *tx_adu,
	 *                                  uint8_t *excep_code,
	 *                                  void *user_data)
	 *      {
	 *          // 解析 rx_adu, 填充 tx_adu, 返回 true 表示要发送响应
	 *          return true;
	 *      }
	 *      MODBUS_CUSTOM_FC_DEFINE(my_fc, my_custom_fc_cb, 0x80, NULL);
	 *      modbus_register_user_fc(iface, &modbus_cfg_my_fc);
	 *
	 * Q: 服务器和客户端数据会进行CRC校验吗? 如果校验失败有回调函数吗?
	 *
	 * A: CRC 校验由 Modbus RTU 子系统自动完成, 无需手动处理:
	 *    - 发送时: modbus_serial_tx_adu() 自动计算 CRC16 并追加到帧尾
	 *    - 接收时: modbus_rtu_rx_adu() 自动校验 CRC16
	 *      - CRC 正确 → rx_adu_err = 0 → 正常处理
	 *      - CRC 错误 → rx_adu_err = -EIO → 丢弃帧, 不响应, 不触发回调
	 *    - 客户端: CRC 错误时 modbus_read_holding_regs() 返回 -EIO
	 *    - 服务端: CRC 错误时 modbus_server_handler() 直接 return false,
	 *      不会调用任何 user_cb, 帧被静默丢弃
	 *    - 没有 CRC 校验失败的回调, 这是 Modbus 协议规范要求的:
	 *      CRC 错误 = 静默丢弃, 不做任何响应
	 */
};

const static struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &mbs_cbs,
		.unit_id = 1,
	},
	.serial = {
		.baud = 115200,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

const static struct modbus_iface_param client_param = {
	.mode = MODBUS_MODE_RTU,
	.rx_timeout = 50000,
	.serial = {
		.baud = 115200,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

static int server_iface;
static int client_iface;

static int init_modbus_server(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_SERVER_NODE)};

	server_iface = modbus_iface_get_by_name(iface_name);
	if (server_iface < 0) {
		LOG_ERR("Failed to get server iface for %s", iface_name);
		return server_iface;
	}

	LOG_INF("Server iface '%s' -> index %d", iface_name, server_iface);
	return modbus_init_server(server_iface, server_param);
}

static int init_modbus_client(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_CLIENT_NODE)};

	client_iface = modbus_iface_get_by_name(iface_name);
	if (client_iface < 0) {
		LOG_ERR("Failed to get client iface for %s", iface_name);
		return client_iface;
	}

	LOG_INF("Client iface '%s' -> index %d", iface_name, client_iface);
	return modbus_init_client(client_iface, client_param);
}

/*
 * 客户端周期读取线程
 *
 * 每 1 秒通过 UART4 (client) 向 UART3 (server, unit_id=1)
 * 读取 holding register 地址 0 (温度),
 * 然后打印温度值.
 *
 * 物理连线: UART3_TX -> UART4_RX, UART3_RX -> UART4_TX, 共地
 */
static void modbus_client_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/*
	 * server_unit_id: Modbus 从站地址 (也叫 unit_id / slave_addr)
	 *
	 * Modbus 是一主多从的总线协议, 每个从站有唯一的 unit_id (1~247).
	 * 客户端请求帧中携带 unit_id, 总线上所有从站都会收到,
	 * 但只有 unit_id 匹配的从站才会响应.
	 * 这里设为 1, 必须与服务端 server_param.server.unit_id 一致.
	 */
	uint8_t server_unit_id = 1;

	/*
	 * REG_ADDR_TEMP: 寄存器起始地址
	 *
	 * Modbus 寄存器按地址寻址, 客户端指定从哪个地址开始读、读多少个.
	 * 这里从地址 0 开始读 2 个寄存器 (温度 + 湿度),
	 * 服务端 holding_reg_rd() 会被调用两次: addr=0 和 addr=1.
	 */
	uint16_t reg_buf[2];
	int err;

	/*
	 * Q: 为什么需要 k_sleep(K_SECONDS(2)) ?
	 *
	 * A: 因为 K_THREAD_DEFINE 会在内核启动时立即创建并运行此线程,
	 *    此时 init_modbus_test() 可能还没执行完 (服务端/客户端还没初始化).
	 *    延迟 2 秒确保 Modbus 子系统初始化完成后再开始通信.
	 *
	 *    更严谨的做法是用 k_event / k_sem 让 init_modbus_test() 完成后
	 *    通知此线程, 而不是盲目等待固定时间.
	 */
	k_sleep(K_SECONDS(2));

	while (1) {
		err = modbus_read_holding_regs(client_iface,
					       server_unit_id,
					       REG_ADDR_TEMP,
					       reg_buf,
					       2);
		if (err != 0) {
			LOG_ERR("Client: FC03 read failed (%d)", err);
		} else {
			int16_t temp_01c = (int16_t)reg_buf[0];
			int16_t humi_01p = (int16_t)reg_buf[1];
			LOG_INF("Client: temp=%d.%d  humi=%d.%d%%",
				temp_01c / 10, abs(temp_01c) % 10,
				humi_01p / 10, abs(humi_01p) % 10);
		}

		holding_regs[0] += 1;

		k_sleep(K_SECONDS(1));
	}
}

/*
 * K_THREAD_DEFINE: 在编译时静态定义一个内核线程
 *
 * 参数含义:
 *   modbus_client_tid  - 线程标识符 (全局变量名, 可用于 k_wakeup 等)
 *   1024               - 栈大小 (字节), Modbus 客户端需要足够栈空间
 *   modbus_client_thread - 线程入口函数
 *   NULL, NULL, NULL   - 传递给入口函数的 p1, p2, p3 参数
 *   7                  - 线程优先级 (数字越小优先级越高, 0=最高, 15=最低)
 *   0                  - 线程选项 (0=无特殊选项; 可用 K_ESSENTIAL 等)
 *   0                  - 启动延迟 (ms, 0=系统启动后立即运行)
 *
 * 与 k_thread_create() 的区别:
 *   K_THREAD_DEFINE  → 编译时静态定义, 无需手动分配栈内存
 *   k_thread_create  → 运行时动态创建, 需要提供栈数组
 */

//modbus线程 --测试
//bus_client_tid, 1024,
//nt_thread, NULL, NULL, NULL,
//

int init_modbus_test(void)
{
	const struct device *const srv_dev =
		DEVICE_DT_GET(DT_PARENT(MODBUS_SERVER_NODE));
	const struct device *const cli_dev =
		DEVICE_DT_GET(DT_PARENT(MODBUS_CLIENT_NODE));

	if (!device_is_ready(srv_dev)) {
		LOG_ERR("Modbus server UART device not ready");
		return -1;
	}

	if (!device_is_ready(cli_dev)) {
		LOG_ERR("Modbus client UART device not ready");
		return -1;
	}

	if (init_modbus_server() != 0) {
		LOG_ERR("Modbus RTU server init failed!");
		return -1;
	}

	if (init_modbus_client() != 0) {
		LOG_ERR("Modbus RTU client init failed!");
		return -1;
	}

	LOG_INF("Modbus RTU initialized (srv=UART3, cli=UART4)");
	return 0;
}

void set_modbus_temp(uint16_t temp)
{
	holding_regs[REG_ADDR_TEMP] = temp;
}
