/**
 * @file    modbus_ser_a.c
 * @brief   Modbus 服务器 A (modbus0, USART1, RS485-1) 业务实现
 *
 * 功能:
 *   - 注册为 Modbus 服务器, unit_id=1
 *   - 提供 2 个保持寄存器 (温度/湿度) 的读写回调
 *   - 提供线圈读取回调 (示例, 返回 false)
 *
 * 服务器工作模型 (非阻塞):
 *   Modbus 子系统在系统工作队列线程中处理请求,
 *   通过用户回调函数访问寄存器数据.
 *   回调函数应尽快返回, 不应执行耗时操作或阻塞.
 *
 * 回调与功能码对应关系:
 *   回调函数              | 功能码 | 客户端 API
 *   ---------------------|-------|---------------------------
 *   coil_rd              | FC01  | modbus_read_coils()
 *   coil_wr              | FC05  | modbus_write_coil()
 *                        | FC15  | modbus_write_coils()
 *   holding_reg_rd       | FC03  | modbus_read_holding_regs()
 *   holding_reg_wr       | FC06  | modbus_write_holding_reg()
 *                        | FC16  | modbus_write_holding_regs()
 */

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>
#include "modbus_driver.h"
#include "modbus_ser_a.h"

LOG_MODULE_REGISTER(modbus_ser_a, LOG_LEVEL_INF);

/* ========================================================================
 * 保持寄存器定义
 * ========================================================================
 * 地址 0: 温度值 (uint16, 单位 0.1°C, 即 250 = 25.0°C)
 * 地址 1: 湿度值 (uint16, 单位 0.1%, 即 600 = 60.0%)
 */

#define HOLDING_REG_COUNT 2

static uint16_t holding_regs[HOLDING_REG_COUNT] = { 250, 600 };

/* ========================================================================
 * 服务器回调函数
 * ========================================================================
 * 这些函数在系统工作队列线程上下文中被调用, 应尽快返回.
 * addr 参数是客户端请求的寄存器/线圈起始地址.
 */

/** 读取线圈 (FC01) */
static int coil_rd(uint16_t addr, bool *state)
{
	*state = false;
	LOG_INF("ServerA: coil read addr=%u", addr);
	return 0;
}

/** 写入线圈 (FC05/FC15) */
static int coil_wr(uint16_t addr, bool state)
{
	LOG_INF("ServerA: coil write addr=%u state=%d", addr, (int)state);
	return 0;
}

/** 读取保持寄存器 (FC03) */
static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= HOLDING_REG_COUNT) {
		/* 地址越界, 返回 -ENOTSUP 会让服务器回复异常码 02 (Illegal Data Address) */
		return -ENOTSUP;
	}
	*reg = holding_regs[addr];
	LOG_INF("ServerA: holding reg read addr=%u val=%u", addr, *reg);
	return 0;
}

/** 写入保持寄存器 (FC06/FC16) */
static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	if (addr >= HOLDING_REG_COUNT) {
		return -ENOTSUP;
	}
	holding_regs[addr] = reg;
	LOG_INF("ServerA: holding reg write addr=%u val=%u", addr, reg);
	return 0;
}

/* 用户回调结构体 (需持久存在, 定义为 static) */
static struct modbus_user_callbacks server_cbs = {
	.coil_rd = coil_rd,
	.coil_wr = coil_wr,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

/* ========================================================================
 * 公共接口
 * ========================================================================
 */

int modbus_ser_a_register(void)
{
	/* unit_id=1, 使用默认串口参数 (115200, 8N1) */
	return modbus_drv_register_server(MODBUS_SRV_A, &server_cbs,
					  1, NULL);
}

uint16_t modbus_ser_a_get_holding_reg(uint16_t addr)
{
	if (addr >= HOLDING_REG_COUNT) {
		return 0;
	}
	return holding_regs[addr];
}

void modbus_ser_a_set_holding_reg(uint16_t addr, uint16_t val)
{
	if (addr < HOLDING_REG_COUNT) {
		holding_regs[addr] = val;
	}
}
