#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <stm32_ll_adc.h>

#include "adc.h"

/*
 * ============================================================================
 * ADC 设备获取
 * ============================================================================
 *
 * Q: GPIO 那边直接定义 gpio_dt_spec 结构体，这里怎么定义的是 const struct device *？
 *
 * A: 因为 Zephyr 的 ADC API 不同。GPIO 有封装好的 GPIO_DT_SPEC_GET 宏，
 *    内部包含 gpio_dt_spec 结构体（dev + pin + dt_flags），一个结构体
 *    就携带了设备指针和引脚号。
 *
 *    但 ADC 没有这种高级封装，它的 API 直接接受 const struct device *，
 *    通道号在每次调用时单独传递（adc_channel_cfg.channel_id 或
 *    adc_sequence.channels），所以只需要保存设备指针。
 *
 *    这是 Zephyr 不同子系统 API 设计风格不一致的表现。
 */
#define ADC_NODE DT_ALIAS(adc1)
const struct device *adc_dev = DEVICE_DT_GET(ADC_NODE);

/*
 * ADC 通道参数宏定义
 *
 * ADC_RESOLUTION = 12
 *   ADC 转换精度，12 位意味着转换结果是 0 ~ 4095（2^12 = 4096 个量化台阶）。
 *   分辨率越高，能区分的电压越精细，但转换时间也越长。
 *   STM32F407 支持 12/10/8/6 位，通过 st,adc-resolutions 属性声明。
 *
 * ADC_GAIN = ADC_GAIN_1
 *   增益因子，即输入信号不放大也不衰减（×1）。
 *   对于有内置 PGA（可编程增益放大器）的 ADC 才需要设置其他值，
 *   STM32F4 不支持 PGA，所以固定为 ADC_GAIN_1。
 *
 * ADC_REFERENCE = ADC_REF_INTERNAL
 *   参考电压源。决定 ADC 满量程对应的电压。
 *     ADC_REF_INTERNAL = 内部参考电压（STM32F407 典型值 VDDA = 3.3V）
 *     ADC_REF_EXTERNAL0 = 外部参考（通过 VREF+ 引脚输入）
 *   ADC 转换结果满量程 (4095) 对应的就是参考电压值。
 *
 * ADC_ACQUISITION_TIME = ADC_ACQ_TIME_DEFAULT
 *   采样保持时间。ADC 内部有一个采样电容，需要一定时间充电到
 *   和被测量电压相同的水平。太久浪费速度，太短采集不准。
 *   ADC_ACQ_TIME_DEFAULT 使用设备树中 sampling-times 的最小值（第 0 个元素）。
 *   对于 STM32F4，sampling-times = <3 15 28 56 84 112 144 480>，
 *   默认 = 3 个 ADC 时钟周期。
 */
#define ADC_RESOLUTION 12
#define ADC_GAIN ADC_GAIN_1
#define ADC_REFERENCE ADC_REF_INTERNAL
#define ADC_ACQUISITION_TIME ADC_ACQ_TIME_DEFAULT

/*
 * ============================================================================
 * 方式一（中断逐通道模式）: 每次 adc_read 只读 1 个通道
 * 内部温度传感器 (通道 16) 专用
 * ============================================================================
 *
 * STM32F407 内部温度传感器是连接到 ADC1 通道 16 的模拟信号。
 * 数据手册要求:
 *   1. 必须先设置 ADC_CCR 寄存器的 TSVREFE 位 (Bit 23)，才能读取通道 16/17
 *      (TS = Temperature Sensor, VREF = VREFINT, E = Enable)
 *   2. 温度传感器上电后需要等待稳定时间 (tSTART ≈ 10μs)
 *   3. 采样时间建议 ≥ 10μs (数据手册最小值)
 *
 * Zephyr ADC 驱动不会自动设置 TSVREFE，需要手动开启。
 * VREF (通道 17) 也是一样的，所以如果后续要读 VREF 也需要 TSVREFE。
 *
 * 温度计算公式 (STM32F4 使用双点校准):
 *   TS_CAL1: 30°C 时的 ADC 原始值 (存储在 Flash 0x1FFF7A2C)
 *   TS_CAL2: 110°C 时的 ADC 原始值 (存储在 Flash 0x1FFF7A2E)
 *
 *   Temperature = (110 - 30) / (TS_CAL2 - TS_CAL1) × (RAW - TS_CAL1) + 30
 * * 方式一（中断逐通道模式）: 每次 adc_read 只读 1 个通道
 * 原理:
 *   - adc_sequence.channels 只包含 1 个通道位，如 BIT(1)
 *   - 驱动不启用 SCAN 模式，只做单次转换
 *   - ISR 只触发一次 → 读取一个值 → 信号量正常释放
 *   - 不依赖 DMA，纯中断模式即可
 *
 * 适用场景:
 *   - 没有配置 DMA 的情况 (CONFIG_ADC_STM32_DMA=n)
 *   - 不需要高速连续采集多个通道
 * 注意:
 *   - 这是 STM32F4 非 DMA 模式下的安全做法
 *   - STM32F4 的多通道 SCAN 模式是为 DMA 设计的，中断模式下会 OVR
 *   但需要注意校准值是在 VDDA=3.3V 条件下测量的，如果实际 VDDA 不同
 *   需要先按比例修正 RAW 值。
 */

#define STM32_ADC_COMMON_INSTANCE __LL_ADC_COMMON_INSTANCE

static void adc_tempsensor_enable(void)
{
	ADC_TypeDef *adc = (ADC_TypeDef *)DT_REG_ADDR(ADC_NODE);
	uint32_t path;

	path = LL_ADC_GetCommonPathInternalCh(STM32_ADC_COMMON_INSTANCE(adc));

	if (!(path & LL_ADC_PATH_INTERNAL_TEMPSENSOR)) {
		LL_ADC_SetCommonPathInternalCh(STM32_ADC_COMMON_INSTANCE(adc),
					       path | LL_ADC_PATH_INTERNAL_TEMPSENSOR);
		k_usleep(LL_ADC_DELAY_TEMPSENSOR_STAB_US);
	}
}

/*
 * ============================================================================
 * 方式一（中断逐通道模式）: 每次 adc_read 只读 1 个通道
 * ============================================================================
 */

static int16_t sample_buf_intr;

static const struct adc_sequence seq_ch1_intr = {
	.channels = BIT(1),
	.buffer = &sample_buf_intr,
	.buffer_size = sizeof(sample_buf_intr),
	.resolution = ADC_RESOLUTION,
};

static const struct adc_sequence seq_ch2_intr = {
	.channels = BIT(2),
	.buffer = &sample_buf_intr,
	.buffer_size = sizeof(sample_buf_intr),
	.resolution = ADC_RESOLUTION,
};

static const struct adc_sequence seq_ch16_intr = {
	.channels = BIT(16),
	.buffer = &sample_buf_intr,
	.buffer_size = sizeof(sample_buf_intr),
	.resolution = ADC_RESOLUTION,
};

int adc_read_channel(uint8_t channel_id, int32_t *val_mv)
{
	const struct adc_sequence *seq;
	int ret;
	int32_t raw;

	switch (channel_id) {
	case 1:
		seq = &seq_ch1_intr;
		break;
	case 2:
		seq = &seq_ch2_intr;
		break;
	case 16:
		seq = &seq_ch16_intr;
		break;
	default:
		printk("ADC: unsupported channel %d\n", channel_id);
		return -1;
	}

	ret = adc_read(adc_dev, seq);
	if (ret) {
		printk("ADC ch%d read failed: %d\n", channel_id, ret);
		return ret;
	}

	raw = sample_buf_intr;

	/*
	 * adc_raw_to_millivolts: 将原始 ADC 值 (0~4095) 转换为实际电压 (mV)
	 *
	 * 转换公式:
	 *   voltage_mV = raw_value / 2^resolution * Vref * 1000
	 *
	 * 具体例子 (12-bit, Vref = 3.3V = 3300mV):
	 *   raw = 2047  →  mV = 2047 / 4096 × 3300 ≈ 1649 mV
	 *   raw = 4095  →  mV = 4095 / 4096 × 3300 ≈ 3299 mV
	 *   raw = 0     →  mV = 0 mV
	 *
	 * 参数说明:
	 *   adc_ref_internal(adc_dev)     — 获取内部参考电压值 (mV)
	 *   ADC_GAIN                       — 增益因子 (×1)
	 *   ADC_RESOLUTION                 — 分辨率 (12 位)
	 *   &raw                           — 输入/输出: 传入原始值, 返回 mV 值
	 *
	 * 注意: 这个函数就地修改 raw 变量的值，所以 raw 既是输入也是输出。
	 */
	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, &raw);
	*val_mv = raw;
	return 0;
}

/*
 * ============================================================================
 * 方式二（DMA 扫描模式）: 一次 adc_read 读取 3 个通道 (CH1 + CH2 + CH16)
 * ============================================================================
 *
 * 原理:
 *   - adc_sequence.channels = BIT(1) | BIT(2), 即多通道 SCAN 模式
 *   - 驱动启用 SCAN 模式: 硬件背靠背连续转换两个通道
 *   - DMA 自动搬运数据到 sample_buffer, ISR 只需处理完成信号
 *   - DMA 硬件速度足够快, 不会发生 OVR 溢出
 *
 * 前提条件:
 *   - 必须启用 CONFIG_ADC_STM32_DMA=y
 *   - 设备树必须配置 dmas 属性 (DMA2 Stream0 Channel0)
 *   - 设备树必须启用 &dma2
 * 
 * 数据在 sample_buffer_dma 中的存储顺序:
 *   sample_buffer_dma[0] = 通道 1 的原始值 (PA1, 引脚电压)
 *   sample_buffer_dma[1] = 通道 2 的原始值 (PA2, 引脚电压)
 *   sample_buffer_dma[2] = 通道 16 的原始值 (内部温度传感器)
 *
 * 硬件资源:
 *   - STM32F407 ADC1 → DMA2 Stream 0, Channel 0
 *   - 数据宽度: 16-bit half-word (对应 12-bit ADC 分辨率)
 *   - 传输方向: 外设→内存 (PERIPH_TO_MEMORY)
 * 注意: 转换顺序由通道号从小到大排列 (1 → 2 → 16)
 */

static int16_t sample_buffer_dma[3];

//更新DMA扫描模式配置，添加对通道16的支持
static const struct adc_sequence seq_scan_dma = {
	.channels = BIT(1) | BIT(2) | BIT(16),
	.buffer = sample_buffer_dma,
	.buffer_size = sizeof(sample_buffer_dma),
	.resolution = ADC_RESOLUTION,
};

/*
 * 将原始 ADC 值转换为摄氏度 (STM32F4 双点校准)
 *
 * 校准数据存储在系统 Flash 的 OTP 区域:
 *   0x1FFF7A2C: TS_CAL1 (30°C 时的 ADC 原始值, 12-bit 右对齐)
 *   0x1FFF7A2E: TS_CAL2 (110°C 时的 ADC 原始值, 12-bit 右对齐)
 *
 * 公式:
 *   slope = (110 - 30) / (TS_CAL2 - TS_CAL1)
 *   temp  = slope × (raw_voltage_mV - TS_CAL1_voltage_mV) + 30
 *
 * 由于校准值是在 VDDA_cal = 3.3V 条件下测量的，而实际 VDDA 可能不同，
 * 所以需要将原始值先按比例修正:
 *   raw_corrected = raw × VDDA_cal / VDDA_real
 *
 * 这里 VDDA_real 从 adc_ref_internal() 获取。
 */
static int32_t adc_raw_to_temp_celsius(int32_t raw_mv)
{
	uint32_t ts_cal1_raw = *((uint16_t *)0x1FFF7A2C);
	uint32_t ts_cal2_raw = *((uint16_t *)0x1FFF7A2E);

	int32_t ts_cal1_mv, ts_cal2_mv;

	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, (int32_t *)&ts_cal1_raw);
	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, (int32_t *)&ts_cal2_raw);

	ts_cal1_mv = ts_cal1_raw;
	ts_cal2_mv = ts_cal2_raw;

	if (ts_cal2_mv <= ts_cal1_mv) {
		printk("ADC: invalid calibration data\n");
		return 0;
	}

	int32_t temp = (int32_t)((int64_t)(raw_mv - ts_cal1_mv) * (110 - 30)
				 / (ts_cal2_mv - ts_cal1_mv) + 30);

	return temp;
}

int adc_read_all_scan(int32_t *val0_mv, int32_t *val1_mv, int32_t *temp_celsius)
{
	int ret;
	int32_t raw0, raw1, raw16;

	ret = adc_read(adc_dev, &seq_scan_dma);
	if (ret) {
		printk("ADC scan read failed: %d\n", ret);
		return ret;
	}

	raw0 = sample_buffer_dma[0];
	raw1 = sample_buffer_dma[1];
	raw16 = sample_buffer_dma[2];

	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, &raw0);
	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, &raw1);
	adc_raw_to_millivolts(adc_ref_internal(adc_dev),
			      ADC_GAIN, ADC_RESOLUTION, &raw16);

	*val0_mv = raw0;
	*val1_mv = raw1;
	*temp_celsius = adc_raw_to_temp_celsius(raw16);
	return 0;
}

/*
 * ============================================================================
 * ADC 初始化
 * ============================================================================
 *
 * adc_channel_setup 是为通道设置采样时间等参数，不是配置读取方式。
 * 读取方式是每次调用 adc_read 时由 adc_sequence.channels 决定的。
 * 所以通道 setup 只需做一次，两种方式都可以复用。
 *
 * Q: GPIO 那边是 gpio_is_ready_dt，这个怎么是 device_is_ready？
 *
 * A: 理由同上——ADC API 是通用 device 接口，没有 DT 专用包装宏。
 *    gpio_is_ready_dt 只是 device_is_ready + gpio_dt_spec 的便捷封装，
 *    底层调用的仍然是 device_is_ready。
 *
 * Q: adc_channel_cfg 的四个参数什么意思？其他参数呢？不用设置吗？
 *
 * A: struct adc_channel_cfg 的完整字段如下:
 *
 *    gain              — 增益 (ADC_GAIN_1 = ×1, 不放大)
 *    reference         — 参考电压源 (ADC_REF_INTERNAL = 片内 VDDA ≈ 3.3V)
 *    acquisition_time  — 采样保持时间 (ADC_ACQ_TIME_DEFAULT = 最短)
 *    channel_id        — 物理通道号 (1 = ADC_IN1 / PA1, 2 = ADC_IN2 / PA2)
 *
 *    还有两个可选字段没填:
 *    differential      — 差分模式 (默认 0 = 单端模式, PA1 对地测量)
 *    input_positive    — 差分模式下的正输入端 (单端模式不需要)
 *
 *    对于 STM32F4 单端 ADC 测量, 这 4 个参数已经足够。差分模式需要
 *    更多配置，详见 include/zephyr/drivers/adc.h。
 *
 * Q: 两个通道只能分开初始化吗？
 *
 * A: 是的，adc_channel_setup 每次只配置一个通道。这是 Zephyr ADC API
 *    的设计——每个通道通过独立的函数调用完成配置，允许不同通道有
 *    不同的增益、参考源、采样时间等个性化参数。
 * 通道 1/2:  外部引脚, 短采样时间 (ADC_ACQ_TIME_DEFAULT ≈ 3 个 ADC 时钟)
 * 通道 16:   内部温度传感器, 长采样时间 (ADC_ACQ_TIME_MAX ≈ 480 个 ADC 时钟)
 *            数据手册要求 ≥ 10μs, 480 周期 @ 10.5MHz ≈ 45.7μs, 满足要求
 */

int adc_init(void)
{
	int ret;

	if (!device_is_ready(adc_dev)) {
		printk("ADC device not ready\n");
		return -1;
	}

	/* 使能内部温度传感器 & VREFINT (必须在 channel_setup 之前) */
	adc_tempsensor_enable();

	/* 通道 1: PA1, 外部引脚, 默认采样时间 */
	struct adc_channel_cfg channel_cfg1 = {
		.gain = ADC_GAIN,
		.reference = ADC_REFERENCE,
		.acquisition_time = ADC_ACQUISITION_TIME,
		.channel_id = 1,
	};
	ret = adc_channel_setup(adc_dev, &channel_cfg1);
	if (ret) {
		printk("ADC channel 1 setup failed: %d\n", ret);
		return ret;
	}

	/* 通道 2: PA2, 外部引脚, 默认采样时间 */
	struct adc_channel_cfg channel_cfg2 = {
		.gain = ADC_GAIN,
		.reference = ADC_REFERENCE,
		.acquisition_time = ADC_ACQUISITION_TIME,
		.channel_id = 2,
	};
	ret = adc_channel_setup(adc_dev, &channel_cfg2);
	if (ret) {
		printk("ADC channel 2 setup failed: %d\n", ret);
		return ret;
	}

	/* 通道 16: 内部温度传感器, 需要最长的采样时间 */
	struct adc_channel_cfg channel_cfg16 = {
		.gain = ADC_GAIN,
		.reference = ADC_REFERENCE,
		.acquisition_time = ADC_ACQ_TIME_MAX,
		.channel_id = 16,
	};
	ret = adc_channel_setup(adc_dev, &channel_cfg16);
	if (ret) {
		printk("ADC channel 16 (temp sensor) setup failed: %d\n", ret);
		return ret;
	}

	return 0;
}