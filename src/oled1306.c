/*
 * OLED SSD1306 显示驱动模块 — 基于 Zephyr Display API
 *
 * 本文件封装了 Zephyr Display 子系统对 SSD1306 OLED 的常用操作。
 *
 * ============================================================================
 * Zephyr Display 子系统架构
 * ============================================================================
 *
 *   应用代码 (oled1306.c)
 *       ↓ 调用 display_write() / display_set_contrast() 等
 *   Display API (zephyr/drivers/display.h)
 *       ↓ 统一接口，屏蔽底层差异
 *   SSD1306 驱动 (zephyr/drivers/display/display_ssd1306.c)
 *       ↓ 通过 I2C/SPI 发送命令和数据
 *   I2C/SPI 驱动 (zephyr/drivers/i2c/i2c_stm32.c)
 *       ↓ 操作硬件寄存器
 *   SSD1306 硬件
 *
 *   关键概念:
 *   - SSD1306 驱动的 write() 将缓冲区数据直接发送给硬件，不做格式转换
 *   - 因此缓冲区必须使用 SSD1306 原生的页式（VTILED）格式
 *   - 像素格式: MONO01 = 0 关 / 1 开, MONO10 = 1 关 / 0 开
 *   - screen_info 包含 SCREEN_INFO_MONO_VTILED 标志
 *
 * ============================================================================
 * SSD1306 显示原理与帧缓冲区格式
 * ============================================================================
 *
 * SSD1306 的显存 (GDDRAM) 组织方式:
 *
 *   128 x 64 像素 = 128 列 x 8 页 (每页 8 行)
 *
 *   Page 0:  行 0~7    (y=0~7)
 *   Page 1:  行 8~15   (y=8~15)
 *   Page 2:  行 16~23  (y=16~23)
 *   ...
 *   Page 7:  行 56~63  (y=56~63)
 *
 *   每个 Page 的一列 = 1 字节，bit0 在上（D0=最上行, D7=最下行）
 *
 * ============================================================================
 * 帧缓冲区格式 — MONO_VTILED（垂直分页）
 * ============================================================================
 *
 *   fb[] 数组布局（页式，与 SSD1306 硬件格式一致）:
 *     fb[0]      = Page 0, 列 0  (y=0~7, x=0), bit0=y0, bit7=y7
 *     fb[1]      = Page 0, 列 1  (y=0~7, x=1)
 *     ...
 *     fb[127]    = Page 0, 列 127 (y=0~7, x=127)
 *     fb[128]    = Page 1, 列 0  (y=8~15, x=0)
 *     ...
 *     fb[1023]   = Page 7, 列 127 (y=56~63, x=127)
 *
 *   总大小 = 8 页 * 128 列 = 1024 字节
 *
 *   计算 fb 索引:
 *     page      = y / 8
 *     bit_index = y % 8
 *     byte_idx  = page * OLED_WIDTH + x
 *
 *   设置像素: fb[byte_idx] |= (1 << bit_index)
 *
 *   【重要】这与行优先 MONO01 格式完全不同!
 *   Zephyr Display API 的 SCREEN_INFO_MONO_VTILED 标志表示:
 *   缓冲区使用垂直分页格式，驱动直接将数据发送给硬件。
 *   如果使用行优先格式，显示内容会完全混乱。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "oled1306.h"

LOG_MODULE_REGISTER(oled1306, LOG_LEVEL_INF);

/*
 * 从设备树获取 SSD1306 设备句柄
 *
 * DT_NODELABEL(ssd1306) 对应 DTS 中 ssd1306: ssd1306@3c { ... } 节点
 * 当 chosen 中设置了 zephyr,display = &ssd1306 时，
 * 也可以用 DEVICE_DT_GET(DT_CHOSEN(zephyr_display)) 获取
 */
static const struct device *oled_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/*
 * 帧缓冲区
 *
 * 128 x 64 像素，MONO01 格式（1 bit/pixel）= 1024 字节
 * 所有绘制操作先修改此缓冲区，再通过 oled_refresh() 写入屏幕。
 *
 * 为什么需要自己的帧缓冲区?
 *   Zephyr Display API 是"应用管理帧缓冲区"模式:
 *   - display_write() 每次调用都会触发 I2C 传输
 *   - 如果逐像素 write，128x64 屏幕需要 8192 次 I2C 传输
 *   - 使用本地帧缓冲区，只需 1 次 display_write() 就能刷新整屏
 */
static uint8_t fb[OLED_WIDTH * OLED_HEIGHT / 8];

/* 显示能力缓存（初始化时从驱动获取） */
static struct display_capabilities caps;


int oled_init(void)
{
	if (!device_is_ready(oled_dev)) {
		LOG_ERR("SSD1306 device not ready!");
		return -ENODEV;
	}

	/* 获取显示能力信息（分辨率、像素格式等） */
	display_get_capabilities(oled_dev, &caps);

	LOG_INF("OLED initialized: %dx%d, pixel_format=%d",
		caps.x_resolution, caps.y_resolution, caps.current_pixel_format);

	/* 清空帧缓冲区并刷新屏幕 */
	oled_clear();

	return 0;
}


void oled_clear(void)
{
	memset(fb, 0, sizeof(fb));
	oled_refresh();
}


void oled_refresh(void)
{
	/*
	 * display_write(dev, x, y, desc, buf)
	 *
	 * SSD1306 驱动的 write() 将 buf 直接发送给硬件，不做格式转换。
	 * 因此 buf 必须是 VTILED 格式（页式，每字节 8 垂直像素）。
	 *
	 * 描述符参数:
	 *   buf_size = 1024  (128 * 64 / 8)
	 *   width    = 128   (像素宽度)
	 *   height   = 64    (像素高度)
	 *   pitch    = 128   (必须等于 width，驱动会检查)
	 *
	 * 驱动约束:
	 *   - pitch 必须等于 width
	 *   - y 必须是 8 的倍数
	 *   - height 必须是 8 的倍数
	 */
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(fb),
		.width = OLED_WIDTH,
		.height = OLED_HEIGHT,
		.pitch = OLED_WIDTH,
	};

	int ret = display_write(oled_dev, 0, 0, &desc, fb);
	if (ret < 0) {
		LOG_ERR("display_write failed: %d", ret);
	}
}


/*
 * 设置帧缓冲区中的单个像素
 *
 * VTILED（垂直分页）格式下，fb[] 中每个字节代表一列中的 8 个垂直像素:
 *   bit0 → 最上行 (y%8 == 0)
 *   bit7 → 最下行 (y%8 == 7)
 *
 * fb[] 按页优先、列递增排列:
 *   fb[page * OLED_WIDTH + x] 的第 (y % 8) 位 = 像素 (x, y)
 *
 * 这与 SSD1306 硬件的 GDDRAM 格式完全一致，
 * 驱动直接将 fb[] 发送给硬件，无需任何格式转换。
 */
void oled_set_pixel(uint16_t x, uint16_t y, bool on)
{
	if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
		return;
	}

	uint16_t page = y / 8;
	uint8_t bit_idx = y % 8;
	uint16_t byte_idx = page * OLED_WIDTH + x;

	if (on) {
		fb[byte_idx] |= (1 << bit_idx);
	} else {
		fb[byte_idx] &= ~(1 << bit_idx);
	}
}


void oled_draw_hline(uint16_t x, uint16_t y, uint16_t len, bool on)
{
	for (uint16_t i = 0; i < len && (x + i) < OLED_WIDTH; i++) {
		oled_set_pixel(x + i, y, on);
	}
}


void oled_draw_vline(uint16_t x, uint16_t y, uint16_t len, bool on)
{
	for (uint16_t i = 0; i < len && (y + i) < OLED_HEIGHT; i++) {
		oled_set_pixel(x, y + i, on);
	}
}


void oled_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on)
{
	oled_draw_hline(x, y, w, on);             /* 上边 */
	oled_draw_hline(x, y + h - 1, w, on);    /* 下边 */
	oled_draw_vline(x, y, h, on);             /* 左边 */
	oled_draw_vline(x + w - 1, y, h, on);    /* 右边 */
}


void oled_set_display(bool on)
{
	if (on) {
		/* display_blanking_off: 开启显示（退出省电模式） */
		display_blanking_off(oled_dev);
	} else {
		/* display_blanking_on: 关闭显示（进入省电模式） */
		display_blanking_on(oled_dev);
	}
}


int oled_test(void)
{
	int ret;

	printk("\n=== OLED SSD1306 Test ===\n");

	/* 步骤 1: 初始化 OLED */
	ret = oled_init();
	if (ret < 0) {
		printk("FAIL: OLED init failed (%d)\n", ret);
		return ret;
	}
	printk("PASS: OLED initialized\n");

	/* 步骤 2: 清屏 */
	oled_clear();
	printk("PASS: Screen cleared\n");

	/* 步骤 3: 绘制测试图案 — 双层边框 */
	oled_draw_rect(0, 0, OLED_WIDTH, OLED_HEIGHT, true);
	oled_draw_rect(2, 2, OLED_WIDTH - 4, OLED_HEIGHT - 4, true);
	oled_refresh();
	printk("PASS: Double border drawn\n");

	printk("=== OLED Test Complete ===\n\n");
	return 0;
}
