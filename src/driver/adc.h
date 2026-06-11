#ifndef __ADC_H__
#define __ADC_H__

#include <stdint.h>

int adc_init(void);

/*
 * 方式一（中断逐通道模式）: 每次只读 1 个通道
 *
 * 调用方式:
 *   int32_t val_mv;
 *   adc_read_channel(1, &val_mv);  // 读通道 1
 *   adc_read_channel(2, &val_mv);  // 读通道 2
 *   adc_read_channel(16, &val_mv); // 读通道 16 (温度传感器, 返回 mV)
 *
 * 特点: 不依赖 DMA，纯中断模式
 */
int adc_read_channel(uint8_t channel_id, int32_t *val_mv);

/*
 * 方式二（DMA 扫描模式）: 一次读取 3 个通道 (CH1 + CH2 + CH16)
 *
 * 调用方式:
 *   int32_t ch1_mv, ch2_mv, temp_c;
 *   adc_read_all_scan(&ch1_mv, &ch2_mv, &temp_c);
 *
 *   返回值:
 *     ch1_mv   — 通道 1 (PA1) 电压, 单位 mV
 *     ch2_mv   — 通道 2 (PA2) 电压, 单位 mV
 *     temp_c   — 芯片温度, 单位 °C
 *
 * 前提: CONFIG_ADC_STM32_DMA=y + DTS dmas + TSVREFE 已使能
 */
int adc_read_all_scan(int32_t *val0_mv, int32_t *val1_mv, int32_t *temp_celsius);

#endif