/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>          // 提供 PRIu32 等格式化宏，用于跨平台打印 uint32_t
#include <string.h>
#include <zephyr/kernel.h>


#include "adc.h"
#include "uart.h"
#include "gpio-ctr.h"
#include "can_driver.h"
#include "modbus_driver.h"
#include "modbus_ser_a.h"
#include "modbus_cli_a.h"
#include "w25qxx.h"
#include "fs_storage.h"
#include "sys_param.h"
#include "iic_board.h"
#include "oled1306.h"

/* 初始化板级资源 */
static int board_init(void)
{
	int ret;
	ret = gpio_init_init();
	if (ret < 0) {
		printk("ERR: gpio init failed\n");
		return 1;
	}
	ret = adc_init();
	if (ret < 0) {
		printk("ERR: adc init failed\n");
		return 1;
	}

	ret = can_drv_init_all();
	if (ret < 0) {
		printk("ERR: can_drv_init_all failed\n");
		return 1;
	}

	/* 串口初始化，已经用作modbus通信，这里注释掉
	ret = uart_init();
	if (ret < 0) {
		printk("ERR: uart init failed\n");
		return 1;
	}
	*/

	ret = modbus_ser_a_register();//注册服务器节点
	if (ret < 0) {
		printk("ERR: modbus_ser_a_register failed\n");
		return 1;
	}
	ret = modbus_cli_a_register();//注册客户端节点
	if (ret < 0) {
		printk("ERR: modbus_cli_a_register failed\n");
		return 1;
	}
	ret = modbus_drv_init_all();//初始化所有modbus驱动
	if (ret < 0) {
		printk("ERR: modbus_drv_init_all failed\n");
		return 1;
	}
	
	/* 初始化 W25Q16 SPI NOR Flash 已经用作文件系统，这里注释掉
	ret = w25qxx_init();
	if (ret < 0) {
		printk("ERR: w25qxx init failed\n");
		return 1;
	}
	/* w25qxx_test(); */

	/* 初始化文件系统（LittleFS 自动挂载检查） */
	ret = fs_storage_init();
	if (ret < 0) {
		printk("ERR: fs_storage init failed\n");
		return 1;
	}

	/* 运行文件系统读写测试 
	ret = fs_storage_test();
	if (ret < 0) {
		printk("ERR: fs_storage test failed\n");
	}
	*/

	/* 初始化参数系统（依赖 LittleFS 已挂载） */
	ret = sys_param_init();
	if (ret < 0) {
		printk("ERR: sys_param init failed\n");
		return 1;
	}

	/* 运行参数系统测试 
	ret = sys_param_test();
	if (ret < 0) {
		printk("ERR: sys_param test failed\n");
	}
	*/
	
	/* 初始化 I2C1 总线并运行测试 */
	ret = i2c_board_test();
	if (ret < 0) {
		printk("ERR: i2c board test failed\n");
	}

	oled_test();

}

/* ========================================================================== *
 * LED 控制线程                                                                *
 *                                                                            *
 * Zephyr 线程创建三要素：                                                     *
 *   1. 线程栈空间 — 必须通过 K_THREAD_STACK_DEFINE 宏静态分配，保证对齐要求    *
 *   2. 线程结构体 — struct k_thread，内核用于管理线程的内部对象                 *
 *   3. 线程入口函数 — 签名必须为 void func(void *p1, void *p2, void *p3)      *
 *                                                                            *
 * 线程优先级说明（数值越小优先级越高）：                                        *
 *   - 0 为最高优先级（仅限零延迟中断处理），一般应用不应使用                     *
 *   - 1~7 为协作式线程优先级，除非主动让出 CPU（k_yield/k_sleep），否则不会被打断 *
 *   - 8~14 为抢占式线程优先级，调度器会按优先级抢占调度                         *
 *   - 15 为最低优先级（idle 线程），一般不使用                                  *
 *   本线程优先级设为 5（协作式），LED 控制无需实时抢占，主动 sleep 让出 CPU 即可 *
 * ========================================================================== */

/* 线程栈大小（字节）
 * LED 控制逻辑简单，无大数组或深层调用栈，1024 字节足够
 * 可通过 k_thread_stack_space_get() 运行时查看剩余栈空间
 */
#define LED_THREAD_STACK_SIZE 1024

/* 线程优先级：5（协作式，不会抢占其他协作式线程） */
#define LED_THREAD_PRIORITY   5

/* 静态分配线程栈
 * K_THREAD_STACK_DEFINE 会处理 Zephyr 要求的栈对齐（通常 32 字节对齐）
 * 不能用普通数组代替，否则可能导致栈溢出或对齐异常
 */
K_THREAD_STACK_DEFINE(led_thread_stack, LED_THREAD_STACK_SIZE);

/* 线程结构体 — 内核通过此结构体管理线程状态、优先级、栈指针等 */
static struct k_thread led_thread_data;

/* 线程入口函数
 * Zephyr 线程入口函数固定签名为 void func(void *p1, void *p2, void *p3)
 * 三个 void* 参数通过 k_thread_create() 的 p1/p2/p3 传入
 * 本线程不需要参数，用 ARG_UNUSED 消除编译器未使用警告
 */
static void led_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		led_ctr_ser();
		k_msleep(1); /* 休眠 1ms，主动让出 CPU，允许其他线程运行 */
	}
}

int main(void)
{
	int ret;
	static int cnt = 0;
	static int uart_cnt = 0;
	static uint8_t rec_can_flag = 0;
	char uart_msg[128];

	ret = board_init();
	if (ret < 0) {
		printk("ERR: board init failed\n");
		return 1;
	}

	
	modbus_cli_a_start();//启动客户端节点

	/* 启动 LED 控制线程
	 *
	 * k_thread_create 参数说明：
	 *   参数1 &led_thread_data  — 线程结构体指针，内核用此管理线程
	 *   参数2 led_thread_stack  — 线程栈起始地址（K_THREAD_STACK_DEFINE 分配）
	 *   参数3 K_THREAD_STACK_SIZEOF(led_thread_stack) — 栈大小
	 *          注意：必须用 K_THREAD_STACK_SIZEOF 宏而非 LED_THREAD_STACK_SIZE，
	 *          因为 K_THREAD_STACK_DEFINE 可能因对齐而多分配空间
	 *   参数4 led_thread_entry  — 线程入口函数
	 *   参数5/6/7 NULL, NULL, NULL — 传给入口函数的 p1/p2/p3 参数
	 *   参数8 LED_THREAD_PRIORITY — 线程优先级（5，协作式）
	 *   参数9 0 — 线程选项，0 表示无特殊选项
	 *          可选：K_ESSENTIAL（关键线程，异常时触发系统致命错误）
	 *                K_FP_REGS（使用浮点寄存器，STM32F4 的 FPU 需要此选项）
	 *                K_USER（用户模式线程，需启用 CONFIG_USERSPACE）
	 *   参数10 K_NO_WAIT — 延迟启动时间，K_NO_WAIT 表示立即调度运行
	 *          也可用 K_MSEC(x) 延迟 x 毫秒后再调度
	 */
	k_thread_create(&led_thread_data, led_thread_stack,
			K_THREAD_STACK_SIZEOF(led_thread_stack),
			led_thread_entry, NULL, NULL, NULL,
			LED_THREAD_PRIORITY, 0, K_NO_WAIT);

	/* 设置线程名称，配合 CONFIG_THREAD_NAME=y
	 * 在 GDB/OpenOCD 调试时可直接看到 "led_ctr" 而非线程编号
	 * 也可通过 k_thread_name_get() 在运行时获取线程名
	 */
	k_thread_name_set(&led_thread_data, "led_ctr");

	printk("=== Zephyr Hello World ===\n");
	printk("Board: %s\n", CONFIG_BOARD);

	while (1) {
        //
		int32_t val0_mv = 0, val1_mv = 0, temp_c = 0;
		cnt++;

		
		if(cnt % 1000 == 0)
		{
			/*
			 * ADC 读取 — 两种方式演示：
			 *
			 * 【当前使用】方式二（DMA 扫描模式）：
			 *   - 一次 adc_read 触发硬件扫描转换两个通道
			 *   - DMA 自动搬运数据，效率高
			 *   - 需要 CONFIG_ADC_STM32_DMA=y
			 *   - 需要 DTS 中配置 dmas 属性
			 *
			 * 【备选】方式一（中断逐通道模式）：
			 *   - 每次只读一个通道，分两次调用
			 *   - 不需要 DMA，纯中断即可
			 *   - 将下面 adc_read_all_scan 替换为：
			 *     ret  = adc_read_channel(1, &val0_mv);
			 *     ret |= adc_read_channel(2, &val1_mv);
			 */
			
			ret = adc_read_all_scan(&val0_mv, &val1_mv, &temp_c);
			if(ret < 0)
			{
				printk("ERR: adc read failed: %d\n", ret);
			}
			else
			{
				//printk("ADC ch1: %d mV, ch2: %d mV, temp: %d C\n",
				//       val0_mv, val1_mv, temp_c);
			}
		}

		if (cnt % 1000 == 0 || rec_can_flag) {
			struct can_frame frame = {0};
			frame.flags = CAN_FRAME_IDE;
			frame.id = 0x123;
			frame.dlc = 8;
			frame.data[0] = 0xF1;
			frame.data[1] = 0x22;
			frame.data[2] = 0x33;
			frame.data[3] = 0x44;
			frame.data[4] = 0x55;
			frame.data[5] = 0x66;
			frame.data[6] = 0x77;
			frame.data[7] = 0x88;
			//非阻塞发送帧 (K_NO_WAIT)
			can_drv_send(CAN_DEV_CAN1, &frame, K_NO_WAIT);
			rec_can_flag = 0;
		}

		//非阻塞检查是否有收到的帧 (K_NO_WAIT)
		struct can_frame frame;
		if (can_drv_recv(CAN_DEV_CAN1, &frame, K_NO_WAIT) == 0) {
			rec_can_flag = 1;
			/printk("[CAN1] RX: ID=0x%08X, DLC=%d, Data=[%02X %02X %02X %02X %02X %02X %02X %02X]\n",
			/       frame.id, frame.dlc, frame.data[0], frame.data[1], frame.data[2], frame.data[3], frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
		}

		/* 串口测试，已经用作modbus通信，这里注释掉（二选一）
		if(cnt % 8 == 0)
		{
			uart_cnt++;
			sprintf(uart_msg, "Hello, UART!\t%d", uart_cnt);
			//uart3_tx_async((const uint8_t *)uart_msg, strlen(uart_msg));
		}
		//非阻塞检查是否有收到的帧 (K_NO_WAIT)
		//有数据就处理, 没有就继续跑主循环, 不阻塞
		struct uart_rx_frame rx_frame;
			while (uart4_rx_frame_get(&rx_frame, K_NO_WAIT) == 0) {
				printk("UART4: %.*s\n", (int)rx_frame.len, rx_frame.data);
			}
		*/
		

		/*
		uint64_t uptime_ms = k_uptime_get();// 获取系统运行时间（毫秒）
		k_sleep(K_MSEC(1));//休眠1ms
		K_MSEC() 宏将毫秒转换为内核时间单位
		k_sleep() 休眠，单位s
		k_sleep(K_SECONDS(1));休眠1s
		*/

		k_msleep(1);//休眠1ms
	}
	return 0;
}

