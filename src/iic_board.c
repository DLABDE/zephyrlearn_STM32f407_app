/*
 * I2C 驱动模块 — STM32F407 I2C1 总线操作封装
 *
 * 本文件封装了 Zephyr I2C API 的常用操作，提供:
 *   1. 总线初始化与扫描
 *   2. 寄存器读写（单字节/多字节）
 *   3. 纯数据读写（无寄存器地址）
 *   4. 综合测试函数
 *
 * ============================================================================
 * Zephyr I2C API 核心概念
 * ============================================================================
 *
 * 【设备树 → C 代码的映射】
 *
 *   DTS 定义:                     C 代码获取:
 *   &i2c1 { ... }                DT_NODELABEL(i2c1)           → 节点 ID
 *   i2cdev@3c { reg = <0x3C>; }  DT_CHILD(i2c1, i2cdev_3c)   → 子节点 ID
 *                                 DT_REG_ADDR(node)            → 读取 reg 地址
 *
 *   获取设备句柄:
 *   const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
 *
 *   设备句柄是 Zephyr 驱动模型的核心，所有 API 都通过它操作硬件。
 *   DEVICE_DT_GET() 在编译时解析设备树，如果节点不存在则编译失败。
 *
 * 【I2C 地址的坑: 7 位 vs 8 位】
 *
 *   I2C 协议中，地址是 7 位的（0x00~0x7F），但总线上传输时会在最低位
 *   拼接 R/W 标志，形成 8 位的"地址字节":
 *
 *     8 位写地址 = (7位地址 << 1) | 0  → 例如 0x3C << 1 = 0x78
 *     8 位读地址 = (7位地址 << 1) | 1  → 例如 0x3C << 1 | 1 = 0x79
 *
 *   Zephyr I2C API 统一使用 7 位地址，驱动内部自动处理 R/W 位拼接。
 *   所以如果你的设备手册写 "地址 0x78"，传给 API 的应该是 0x3C。
 *
 * 【Zephyr I2C API 速查】
 *
 *   i2c_write_read(dev, addr, wbuf, wlen, rbuf, rlen)
 *     → 先写后读（最灵活，一次总线事务完成写+读）
 *     → 等价于: START → 写 wbuf → RESTART → 读 rbuf → STOP
 *
 *   i2c_write(dev, buf, len, addr)
 *     → 纯写（无寄存器地址前缀）
 *
 *   i2c_read(dev, buf, len, addr)
 *     → 纯读（无寄存器地址前缀）
 *
 *   i2c_reg_write_byte(dev, addr, reg, val)
 *     → 写单个寄存器（1 字节地址 + 1 字节数据）
 *
 *   i2c_reg_read_byte(dev, addr, reg, val)
 *     → 读单个寄存器（1 字节地址 + 1 字节数据）
 *
 *   i2c_burst_write(dev, addr, reg, buf, len)
 *     → 写多个寄存器（1 字节地址 + N 字节数据，寄存器地址自动递增）
 *
 *   i2c_burst_read(dev, addr, reg, buf, len)
 *     → 读多个寄存器（1 字节地址 + N 字节数据，寄存器地址自动递增）
 *
 *   注意: 以上 API 都是同步阻塞的，调用线程会阻塞直到传输完成。
 *         Zephyr 也提供异步 API (i2c_transfer_cb)，但使用场景较少。
 *
 * 【STM32 I2C 驱动版本】
 *
 *   STM32F407 使用 "st,stm32-i2c-v1" 驱动（即 STM32 经典 I2C 外设）。
 *   特点:
 *   - 硬件上只有 I2C1 和 I2C2（F4 没有 I2C3，那是 F1/F3 的）
 *   - 不支持 DMA 传输（v1 驱动不支持，v2 才支持）
 *   - 中断驱动，每次传输最大 255 字节
 *   - 已知问题: 某些情况下 BUSY 标志卡住，需要软复位（驱动已处理）
 *
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "iic_board.h"

/* 注册日志模块，日志前缀为 "i2c_board" */
LOG_MODULE_REGISTER(i2c_board, LOG_LEVEL_INF);

/*
 * 从设备树获取 I2C1 总线的设备句柄
 *
 * DT_NODELABEL(i2c1) 对应 DTS 中的 &i2c1 { ... } 节点
 * DEVICE_DT_GET() 将设备树节点转换为运行时设备句柄
 *
 * 这是一个编译时常量，如果 i2c1 节点不存在，编译直接失败。
 * 这比运行时检查更安全——配置错误在编译期就能发现。
 */
static const struct device *i2c1_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

/*
 * 从设备树获取总线设备的 7 位地址
 *
 * DT_NODELABEL(i2c_device_0x78) 对应 DTS 中 i2c_device_0x78: i2cdev@3c 子节点
 * DT_REG_ADDR() 读取子节点的 reg 属性值
 *
 * 如果你的设备地址不同，修改 DTS 中 reg 值即可，此处自动跟随。
 */
#define I2C_DEV_ADDR  DT_REG_ADDR(DT_NODELABEL(i2c_device_0x78))


int i2c_board_init(void)
{
	/*
	 * device_is_ready() 检查设备是否已初始化就绪
	 *
	 * Zephyr 的驱动初始化是分阶段的 (SYS_INIT level):
	 *   PRE_KERNEL_1 → PRE_KERNEL_2 → POST_KERNEL → APPLICATION
	 * I2C 驱动通常在 POST_KERNEL 阶段初始化。
	 * 如果在 main() 之前调用此函数，可能设备还没就绪。
	 *
	 * 返回 true 表示: 驱动已初始化 + 硬件已配置 + 可以使用
	 * 返回 false 表示: 驱动未初始化或初始化失败
	 */
	if (!device_is_ready(i2c1_dev)) {
		LOG_ERR("I2C1 device not ready!");
		return -ENODEV;
	}

	LOG_INF("I2C1 initialized (addr=0x%02X)", (uint32_t)I2C_DEV_ADDR);
	return 0;
}


int i2c_board_scan(void)
{
	int count = 0;

	LOG_INF("Scanning I2C bus...");

	/*
	 * I2C 总线扫描原理:
	 *
	 * 对每个可能的 7 位地址发送一个 START + 地址 + STOP，
	 * 如果设备存在且应答（ACK），i2c_write() 返回 0；
	 * 如果无设备或设备不应答（NACK），返回负错误码。
	 *
	 * 扫描范围 0x03 ~ 0x77:
	 *   0x00~0x02: I2C 协议保留（广播、起始字节、CBUS）
	 *   0x08~0x77: 用户可用地址
	 *   0x78~0x7F: 10 位地址保留
	 *   这里从 0x03 开始扫描，覆盖所有有效地址
	 */
	for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
		/*
		 * i2c_write(dev, buf, len, addr)
		 *
		 * 发送一个空写操作（0 字节数据），仅发送地址字节。
		 * 如果设备存在，会回复 ACK，函数返回 0。
		 * 如果设备不存在，总线 NACK，函数返回负错误码。
		 *
		 * 这比 i2c_read() 更安全，因为不会读取到意外数据。
		 */
		int ret = i2c_write(i2c1_dev, NULL, 0, addr);
		if (ret == 0) {
			LOG_INF("  Device found at 0x%02X (8-bit: 0x%02X/0x%02X)",
				addr, (addr << 1), (addr << 1) | 1);
			count++;
		}
	}

	if (count == 0) {
		LOG_WRN("No I2C devices found! Check wiring and pull-ups.");
	} else {
		LOG_INF("Scan complete: %d device(s) found", count);
	}

	return count;
}


int i2c_board_write_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t value)
{
	/*
	 * i2c_reg_write_byte(dev, addr, reg, val)
	 *
	 * 这是 Zephyr 提供的寄存器写入便捷函数，等价于:
	 *   uint8_t buf[2] = { reg_addr, value };
	 *   i2c_write(dev, buf, 2, dev_addr);
	 *
	 * 时序: [START] [dev_addr+W] [reg_addr] [value] [STOP]
	 *
	 * 适用场景: 配置传感器寄存器、设置设备参数等
	 */
	int ret = i2c_reg_write_byte(i2c1_dev, dev_addr, reg_addr, value);
	if (ret < 0) {
		LOG_ERR("Write reg failed: addr=0x%02X reg=0x%02X err=%d",
			dev_addr, reg_addr, ret);
	}
	return ret;
}


int i2c_board_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *value)
{
	/*
	 * i2c_reg_read_byte(dev, addr, reg, val)
	 *
	 * 等价于:
	 *   i2c_write_read(dev, addr, &reg_addr, 1, val, 1);
	 *
	 * 时序: [START] [dev_addr+W] [reg_addr]
	 *       [RESTART] [dev_addr+R] [value] [STOP]
	 *
	 * RESTART (也叫Repeated Start) 的作用:
	 *   不释放总线，直接从写模式切换到读模式。
	 *   这确保了读操作的原子性——其他主设备无法在中间插入传输。
	 */
	int ret = i2c_reg_read_byte(i2c1_dev, dev_addr, reg_addr, value);
	if (ret < 0) {
		LOG_ERR("Read reg failed: addr=0x%02X reg=0x%02X err=%d",
			dev_addr, reg_addr, ret);
	}
	return ret;
}


int i2c_board_write_burst(uint8_t dev_addr, uint8_t reg_addr,
			  const uint8_t *data, uint16_t len)
{
	/*
	 * i2c_burst_write(dev, addr, reg, buf, len)
	 *
	 * 时序: [START] [dev_addr+W] [reg_addr] [data0] [data1] ... [dataN-1] [STOP]
	 *
	 * 大多数 I2C 设备在连续写入时，寄存器地址会自动递增。
	 * 例如: 写入 reg=0x01, data=[A, B, C]
	 *   → reg 0x01 = A, reg 0x02 = B, reg 0x03 = C
	 *
	 * 这对于批量读取传感器数据（如加速度计 XYZ 三轴）非常有用。
	 *
	 * 注意: 不是所有设备都支持地址自动递增，查阅设备数据手册确认。
	 */
	int ret = i2c_burst_write(i2c1_dev, dev_addr, reg_addr, data, len);
	if (ret < 0) {
		LOG_ERR("Burst write failed: addr=0x%02X reg=0x%02X len=%u err=%d",
			dev_addr, reg_addr, len, ret);
	}
	return ret;
}


int i2c_board_read_burst(uint8_t dev_addr, uint8_t reg_addr,
			 uint8_t *data, uint16_t len)
{
	/*
	 * i2c_burst_read(dev, addr, reg, buf, len)
	 *
	 * 时序: [START] [dev_addr+W] [reg_addr]
	 *       [RESTART] [dev_addr+R] [data0] [data1] ... [dataN-1] [STOP]
	 *
	 * 同样依赖设备的地址自动递增功能。
	 */
	int ret = i2c_burst_read(i2c1_dev, dev_addr, reg_addr, data, len);
	if (ret < 0) {
		LOG_ERR("Burst read failed: addr=0x%02X reg=0x%02X len=%u err=%d",
			dev_addr, reg_addr, len, ret);
	}
	return ret;
}


int i2c_board_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
	/*
	 * i2c_write(dev, buf, len, addr)
	 *
	 * 纯写操作，不发送寄存器地址前缀。
	 * 时序: [START] [dev_addr+W] [data0] [data1] ... [STOP]
	 *
	 * 适用场景:
	 *   - OLED SSD1306: 发送控制字节 + 命令/数据
	 *     例如: buf = {0x00, 0xAE}  →  控制字节=0x00(命令), 命令=0xAE(关显示)
	 *   - 某些简单设备: 整个写操作就是一串命令，无寄存器概念
	 */
	int ret = i2c_write(i2c1_dev, data, len, dev_addr);
	if (ret < 0) {
		LOG_ERR("Raw write failed: addr=0x%02X len=%u err=%d",
			dev_addr, len, ret);
	}
	return ret;
}


int i2c_board_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len)
{
	/*
	 * i2c_read(dev, buf, len, addr)
	 *
	 * 纯读操作，不发送寄存器地址。
	 * 时序: [START] [dev_addr+R] [data0] [data1] ... [STOP]
	 *
	 * 适用场景:
	 *   - 读取设备 ID 寄存器（某些设备上电后默认输出 ID）
	 *   - 读取 FIFO 数据（地址指针自动指向下一个数据）
	 */
	int ret = i2c_read(i2c1_dev, data, len, dev_addr);
	if (ret < 0) {
		LOG_ERR("Raw read failed: addr=0x%02X len=%u err=%d",
			dev_addr, len, ret);
	}
	return ret;
}


int i2c_board_test(void)
{
	int ret;
	uint8_t val;

	printk("\n=== I2C Board Test ===\n");

	/* 步骤 1: 初始化 I2C 总线 */
	ret = i2c_board_init();
	if (ret < 0) {
		printk("FAIL: I2C init failed (%d)\n", ret);
		return ret;
	}
	printk("PASS: I2C1 initialized\n");

	/* 步骤 2: 扫描总线上的设备 */
	ret = i2c_board_scan();
	if (ret < 0) {
		printk("FAIL: I2C scan failed (%d)\n", ret);
		return ret;
	}
	printk("PASS: Scan found %d device(s)\n", ret);

	/* 步骤 3: 尝试对 DTS 中定义的设备进行读写测试 */
	printk("\n--- Testing device at 0x%02X ---\n", I2C_DEV_ADDR);

	/*
	 * 尝试读取寄存器 0x00
	 *
	 * 注意: 这里假设设备存在且支持寄存器读取。
	 * 如果设备不支持寄存器协议（如某些纯输出设备），
	 * 读操作会失败，这是正常的，不代表 I2C 配置有问题。
	 *
	 * 对于 OLED SSD1306 (0x3C):
	 *   它不使用标准寄存器协议，i2c_reg_read_byte 可能返回错误。
	 *   但 i2c_write (发送控制字节) 应该能成功。
	 */
	ret = i2c_board_read_reg(I2C_DEV_ADDR, 0x00, &val);
	if (ret == 0) {
		printk("PASS: Read reg 0x00 = 0x%02X\n", val);
	} else {
		printk("INFO: Read reg 0x00 failed (%d) — "
		       "device may not support register protocol\n", ret);
	}

	/*
	 * 尝试纯写测试
	 * 发送一个空操作或设备特定的命令，验证总线写入功能。
	 * 对于 OLED SSD1306: 0x00 是命令控制字节，0xAE 是关显示命令
	 * 如果你的设备不同，请修改此处的测试数据。
	 
	uint8_t test_buf[] = { 0x00, 0xAE };  // SSD1306: 命令模式 + 关显示
	ret = i2c_board_write_raw(I2C_DEV_ADDR, test_buf, sizeof(test_buf));
	if (ret == 0) {
		printk("PASS: Raw write test succeeded\n");
	} else {
		printk("INFO: Raw write test failed (%d) — "
		       "check device address and wiring\n", ret);
	}

	printk("=== I2C Test Complete ===\n\n");
	*/
	return 0;
}
