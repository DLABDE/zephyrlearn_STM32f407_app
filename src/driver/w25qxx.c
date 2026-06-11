/*
 * W25Q16 SPI NOR Flash 驱动实现
 *
 * 本文件基于 Zephyr 的 jedec,spi-nor 通用驱动，封装了简洁的读写接口。
 * 不需要手写 SPI 收发逻辑，Zephyr 的 spi_nor 驱动已处理了：
 *   - SPI 命令帧的组装（操作码 + 地址 + 数据）
 *   - CS 片选的拉低/拉高（每次事务自动控制）
 *   - 写使能（WREN）的自动发送
 *   - 等待写入/擦除完成（轮询状态寄存器的 BUSY 位）
 *   - 扇区/页对齐检查
 *
 * 调用链：w25qxx_write() → flash_write() → spi_nor 驱动 → SPI 控制器驱动 → GPIO
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>      /* flash_erase / flash_write / flash_read */
#include <zephyr/device.h>             /* DEVICE_DT_GET / device_is_ready */
#include <zephyr/devicetree.h>         /* DT_NODELABEL */
#include <zephyr/logging/log.h>        /* LOG_INF / LOG_ERR 等 */
#include <string.h>
#include "w25qxx.h"

/*
 * LOG_MODULE_REGISTER 的作用：
 *   注册一个名为 "w25qxx" 的日志模块，运行时可通过 shell 或 Kconfig 控制日志级别
 *   第二个参数 LOG_LEVEL_INF 表示默认只输出 INF 及以上级别的日志
 *   可选级别：LOG_LEVEL_NONE / LOG_LEVEL_ERR / LOG_LEVEL_WRN / LOG_LEVEL_INF / LOG_LEVEL_DBG
 */
LOG_MODULE_REGISTER(w25qxx, LOG_LEVEL_INF);

/*
 * 通过 DTS 节点标签获取 W25Q16 设备节点
 *
 * DT_NODELABEL(w25qxx) 对应 DTS 中 w25qxx: w25qxx@0 { ... } 的节点
 * 这比 DT_COMPAT_GET_ANY_STATUS_OKAY(jedec_spi_nor) 更精确：
 *   - 后者只返回第一个 compatible 匹配的节点，多设备时会出错
 *   - DT_NODELABEL 通过标签精确指定，不受节点顺序影响
 */
#define W25QXX_NODE DT_NODELABEL(w25qxx)

/* 测试用偏移地址：W25Q16 最后一个扇区（2MB - 4KB = 0x1FF000），避免覆盖其他数据 */
#define W25QXX_TEST_OFFSET 0x1FF000

/* Flash 设备实例指针，由 w25qxx_init() 初始化 */
static const struct device *flash_dev;

int w25qxx_init(void)
{
	/*
	 * DEVICE_DT_GET：编译时通过 DTS 节点获取设备结构体指针
	 * 它不检查设备是否已初始化，只做编译时节点有效性检查
	 * 如果 DTS 中没有定义该节点，编译直接报错
	 */
	flash_dev = DEVICE_DT_GET(W25QXX_NODE);

	/*
	 * device_is_ready：运行时检查设备是否就绪
	 * 就绪条件：驱动已初始化完成、JEDEC ID 校验通过、SPI 通信正常
	 * 如果 CS 引脚未正确连接或 SPI 配置有误，此处会返回 false
	 */
	if (!device_is_ready(flash_dev)) {
		LOG_ERR("W25Q16 device not ready! Check SPI wiring and DTS config");
		return -ENODEV;
	}

	LOG_INF("W25Q16 initialized: %s", flash_dev->name);
	return 0;
}

int w25qxx_erase(uint32_t offset, uint32_t size)
{
	if (flash_dev == NULL) {
		return -EINVAL;
	}

	/*
	 * flash_erase 参数说明：
	 *   dev    : Flash 设备指针
	 *   offset : 擦除起始偏移（必须扇区对齐，即 4KB 对齐）
	 *   size   : 擦除大小（必须为扇区大小的整数倍）
	 *
	 * W25Q16 支持三种擦除粒度：
	 *   - 扇区擦除 (4 KB)   : 命令 0x20，最常用
	 *   - 块擦除   (32 KB)  : 命令 0x52
	 *   - 块擦除   (64 KB)  : 命令 0xD8
	 *   - 整片擦除          : 命令 0xC7
	 * Zephyr 驱动会根据 size 自动选择合适的擦除命令
	 */
	int rc = flash_erase(flash_dev, offset, size);
	if (rc != 0) {
		LOG_ERR("Erase failed at 0x%06X, size=%u (rc=%d)", offset, size, rc);
	} else {
		LOG_INF("Erased at 0x%06X, size=%u", offset, size);
	}
	return rc;
}

int w25qxx_write(uint32_t offset, const uint8_t *data, size_t len)
{
	if (flash_dev == NULL) {
		return -EINVAL;
	}

	/*
	 * flash_write 参数说明：
	 *   dev    : Flash 设备指针
	 *   offset : 写入起始偏移
	 *   data   : 待写入数据
	 *   len    : 数据长度
	 *
	 * 注意事项：
	 *   1. 目标区域必须已擦除（0xFF），Flash 只能把 1 写成 0，不能把 0 写成 1
	 *   2. 单次写入不能跨页边界（每页 256 字节），跨页需分多次调用
	 *      例如：从 offset=250 写 10 字节会失败（250+10=260 超过页边界 256）
	 *      正确做法：先写 6 字节（250~255），再写 4 字节（256~259）
	 *   3. 驱动内部会自动发送 WREN（写使能）命令，无需手动操作
	 */
	int rc = flash_write(flash_dev, offset, data, len);
	if (rc != 0) {
		LOG_ERR("Write failed at 0x%06X, len=%zu (rc=%d)", offset, len, rc);
	}
	return rc;
}

int w25qxx_read(uint32_t offset, uint8_t *buf, size_t len)
{
	if (flash_dev == NULL) {
		return -EINVAL;
	}

	/*
	 * flash_read 参数说明：
	 *   dev    : Flash 设备指针
	 *   offset : 读取起始偏移
	 *   buf    : 接收缓冲区
	 *   len    : 读取长度
	 *
	 * 读取无对齐限制，任意偏移、任意长度均可
	 * 驱动内部发送命令 0x03（标准读）+ 24位地址，然后接收数据
	 */
	int rc = flash_read(flash_dev, offset, buf, len);
	if (rc != 0) {
		LOG_ERR("Read failed at 0x%06X, len=%zu (rc=%d)", offset, len, rc);
	}
	return rc;
}

int w25qxx_test(void)
{
	/* 测试数据：4 字节，故意选非 0xFF 的值以验证写入效果 */
	const uint8_t test_data[] = { 0x55, 0xAA, 0x66, 0x99 };
	uint8_t buf[sizeof(test_data)];
	int rc;

	LOG_INF("=== W25Q16 Test Start ===");

	/* 步骤 1：擦除测试扇区 */
	LOG_INF("Step 1: Erasing sector at 0x%06X...", W25QXX_TEST_OFFSET);
	rc = w25qxx_erase(W25QXX_TEST_OFFSET, W25QXX_SECTOR_SIZE);
	if (rc != 0) {
		return rc;
	}

	/* 步骤 2：验证擦除结果（擦除后应全为 0xFF） */
	LOG_INF("Step 2: Verifying erase...");
	rc = w25qxx_read(W25QXX_TEST_OFFSET, buf, sizeof(buf));
	if (rc != 0) {
		return rc;
	}
	for (size_t i = 0; i < sizeof(buf); i++) {
		if (buf[i] != 0xFF) {
			LOG_ERR("Erase verify failed: offset+0x%02X = 0x%02X (expected 0xFF)",
				i, buf[i]);
			return -EIO;
		}
	}
	LOG_INF("Erase verified: all 0xFF");

	/* 步骤 3：写入测试数据 */
	LOG_INF("Step 3: Writing test data {0x55, 0xAA, 0x66, 0x99}...");
	rc = w25qxx_write(W25QXX_TEST_OFFSET, test_data, sizeof(test_data));
	if (rc != 0) {
		return rc;
	}

	/* 步骤 4：读回并比较 */
	LOG_INF("Step 4: Reading back and comparing...");
	memset(buf, 0, sizeof(buf));
	rc = w25qxx_read(W25QXX_TEST_OFFSET, buf, sizeof(buf));
	if (rc != 0) {
		return rc;
	}

	if (memcmp(test_data, buf, sizeof(test_data)) == 0) {
		LOG_INF("Test PASSED! Read: 0x%02X 0x%02X 0x%02X 0x%02X",
			buf[0], buf[1], buf[2], buf[3]);
	} else {
		LOG_ERR("Test FAILED! Expected: 0x55 0xAA 0x66 0x99, Got: 0x%02X 0x%02X 0x%02X 0x%02X",
			buf[0], buf[1], buf[2], buf[3]);
		return -EIO;
	}

	LOG_INF("=== W25Q16 Test Complete ===");
	return 0;
}
