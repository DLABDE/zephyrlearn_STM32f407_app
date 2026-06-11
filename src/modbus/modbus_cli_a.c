/**
 * @file    modbus_cli_a.c
 * @brief   Modbus 客户端 A (modbus2, USART6, RS485-3) 业务实现
 *
 * 功能:
 *   - 注册为 Modbus 客户端, rx_timeout=50ms
 *   - 创建独立线程周期性读取远端设备的保持寄存器
 *   - 读取结果通过日志输出
 *
 * 客户端线程工作流程:
 *   1. 从 modbus_drv_get_iface() 获取接口索引
 *   2. 循环调用 modbus_read_holding_regs() (阻塞, 等待响应或超时)
 *   3. 成功时打印读取的温度/湿度值
 *   4. 等待 CLI_A_READ_INTERVAL_MS 后继续
 *
 * 测试说明:
 *   本示例中 modbus2 (客户端, RS485-3) 读取 unit_id=1 的设备,
 *   与 modbus0 (服务器, RS485-1) 的 unit_id 相同.
 *   测试时需将 RS485-1 和 RS485-3 总线物理连接.
 */

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include "modbus_driver.h"
#include "modbus_cli_a.h"

LOG_MODULE_REGISTER(modbus_cli_a, LOG_LEVEL_INF);

/* ========================================================================
 * 客户端线程配置
 * ========================================================================
 */

#define CLI_A_STACK_SIZE	     2048  /* 线程栈大小 (字节) */
#define CLI_A_PRIORITY		     5    /* 线程优先级 */
#define CLI_A_READ_INTERVAL_MS    1000  /* 读取间隔 (毫秒) */
#define CLI_A_TARGET_UNIT_ID      1     /* 目标服务器从站地址 */
#define CLI_A_START_DELAY_MS      500   /* 启动延迟, 等待服务器就绪 */

/* 客户端线程栈 */
K_THREAD_STACK_DEFINE(cli_a_stack, CLI_A_STACK_SIZE);
static struct k_thread cli_a_thread_data;

/* ========================================================================
 * 客户端线程
 * ========================================================================
 * 独立线程中周期性读取远端 Modbus 设备数据.
 *
 * modbus_read_holding_regs() 是阻塞调用:
 *   - 发送请求帧后通过 k_sem_take 等待响应
 *   - 超时值由注册时的 rx_timeout_us (50ms) 决定
 *   - 超时返回 -ETIMEDOUT, 成功返回 0
 *   - 此线程独立运行, 不影响主逻辑线程
 *
 * 互斥保护:
 *   Zephyr Modbus 客户端 API 内部使用 k_mutex 保护,
 *   同一接口不会被多个线程同时使用.
 *   如果需要多个线程访问同一接口, 需在应用层加锁.
 */

static void cli_a_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int iface = modbus_drv_get_iface(MODBUS_CLI_A);
	if (iface < 0) {
		LOG_ERR("ClientA: iface not available (%d)", iface);
		return;
	}

	/* 等待服务器就绪 */
	k_msleep(CLI_A_START_DELAY_MS);

	LOG_INF("ClientA: start reading from unit_id=%u", CLI_A_TARGET_UNIT_ID);

	uint16_t reg_buf[2];

	while (1) {
		/*
		 * FC03: 读取保持寄存器
		 * 从目标 unit_id 的设备, 读取地址 0 开始的 2 个寄存器
		 * (温度 + 湿度, 与 modbus_ser_a 的寄存器映射对应)
		 */
		int err = modbus_read_holding_regs(iface,
						   CLI_A_TARGET_UNIT_ID,
						   0,    /* 起始地址 */
						   reg_buf,
						   2);   /* 寄存器数量 */
		if (err != 0) {
			LOG_ERR("ClientA: FC03 read failed (%d)", err);
		} else {
			int16_t temp = (int16_t)reg_buf[0];
			int16_t humi = (int16_t)reg_buf[1];
			//LOG_INF("ClientA: temp=%d.%d°C  humi=%d.%d%%",
			//	temp / 10, abs(temp) % 10,
			//	humi / 10, abs(humi) % 10);
		}

		k_msleep(CLI_A_READ_INTERVAL_MS);
	}
}

/* ========================================================================
 * 公共接口
 * ========================================================================
 */

int modbus_cli_a_register(void)
{
	/* rx_timeout=50ms, 使用默认串口参数 (115200, 8N1) */
	return modbus_drv_register_client(MODBUS_CLI_A, 50000, NULL);
}

int modbus_cli_a_start(void)
{
	if (!modbus_drv_is_ready(MODBUS_CLI_A)) {
		LOG_ERR("ClientA: node not initialized");
		return -ENODEV;
	}

	k_thread_create(&cli_a_thread_data, cli_a_stack,
			K_THREAD_STACK_SIZEOF(cli_a_stack),
			cli_a_thread_fn,
			NULL, NULL, NULL,
			CLI_A_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&cli_a_thread_data, "modbus_cli_a");

	LOG_INF("ClientA: thread started");
	return 0;
}
