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
#include "modbus_test.h"
#include "w25qxx.h"
#include "fs_storage.h"



int main(void)
{
	int ret;
	static int cnt = 0;
	static int uart_cnt = 0;
	char uart_msg[128];
	
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
	/*ret = uart_init();
	if (ret < 0) {
		printk("ERR: uart init failed\n");
		return 1;
	}*/

	ret = init_modbus_test();
	if(ret < 0)
	{
		printk("ERR: modbus init failed\n");
		return 1;
	}

	/* 初始化 W25Q16 SPI NOR Flash */
	ret = w25qxx_init();
	if (ret < 0) {
		printk("ERR: w25qxx init failed\n");
		return 1;
	}
	/* w25qxx_test() 保留作学习记录，不再自动调用 */
	/* ret = w25qxx_test(); */

	/* 初始化文件系统（LittleFS 自动挂载检查） */
	ret = fs_storage_init();
	if (ret < 0) {
		printk("ERR: fs_storage init failed\n");
		return 1;
	}
	/* 运行文件系统读写测试 */
	ret = fs_storage_test();
	if (ret < 0) {
		printk("ERR: fs_storage test failed\n");
	}


	printk("=== Zephyr Hello World ===\n");
	printk("Board: %s\n", CONFIG_BOARD);

	while (1) {
        uint64_t uptime_ms = k_uptime_get();// 获取系统运行时间（毫秒）
		int32_t val0_mv = 0, val1_mv = 0, temp_c = 0;
		cnt++;

		led_ctr_ser();

		
		
		if(cnt % 1000 == 0)
		{
			//printk("System Uptime: %u seconds\n", uptime_ms / 1000);
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
			set_modbus_temp(temp_c * 10);
		}

		if(cnt % 8 == 0)
		{
			uart_cnt++;
			sprintf(uart_msg, "Hello, UART!\t%d", uart_cnt);
			uart3_tx_async((const uint8_t *)uart_msg, strlen(uart_msg));
			
		}

		/*
		* 非阻塞检查是否有收到的帧 (K_NO_WAIT)
		* 有数据就处理, 没有就继续跑主循环, 不阻塞
		*/
		struct uart_rx_frame rx_frame;
			while (uart4_rx_frame_get(&rx_frame, K_NO_WAIT) == 0) {
				printk("UART4: %.*s\n", (int)rx_frame.len, rx_frame.data);
			}

		

		//k_sleep(K_MSEC(1));//休眠1ms
		//K_MSEC() 宏将毫秒转换为内核时间单位
		//k_sleep() 休眠，单位s
		//k_sleep(K_SECONDS(1));休眠1s
		k_msleep(1);//休眠1ms
	}
	return 0;
}

