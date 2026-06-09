#ifndef __OLED1306_H__
#define __OLED1306_H__

/*
 * OLED SSD1306 显示驱动模块
 *
 * 基于 Zephyr Display 子系统 API，封装了常用的 OLED 操作:
 *   - 初始化与清屏
 *   - 逐像素绘制（点、线、矩形）
 *   - 帧缓冲区管理
 *   - 显示开关
 *
 * 注意: 本模块使用 Zephyr Display API（高层 API），
 *       与 iic_board.c 中的 I2C 底层 API 是不同的操作层级。
 *       两者不要同时操作同一个设备，否则会产生总线冲突。
 */

#include <stdint.h>
#include <stdbool.h>

/* 屏幕分辨率常量（与设备树中 width/height 对应） */
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

/*
 * OLED 初始化
 *
 * 获取设备树中定义的 SSD1306 设备句柄，验证设备就绪，
 * 并清空屏幕内容。
 *
 * 返回值: 0 成功, 负的错误码失败
 */
int oled_init(void);

/*
 * 清空屏幕（所有像素关闭）
 *
 * 将帧缓冲区全部置零并刷新到屏幕。
 * SSD1306 像素格式 MONO01: 0 = 关闭, 1 = 点亮
 */
void oled_clear(void);

/*
 * 刷新屏幕
 *
 * 将当前帧缓冲区内容发送到 OLED 显示。
 * 所有绘制操作（set_pixel / draw_rect 等）只修改内存中的
 * 帧缓冲区，必须调用此函数才能在屏幕上看到变化。
 *
 * 典型用法:
 *   oled_clear();
 *   oled_draw_rect(0, 0, 128, 64, true);
 *   oled_refresh();   // 此时屏幕才更新
 *
 * 注意: display_write() 是同步阻塞调用，整屏刷新约需 ~25ms (400KHz I2C)
 */
void oled_refresh(void);

/*
 * 设置单个像素
 *
 * 参数:
 *   x     — 列坐标 (0 ~ OLED_WIDTH-1)
 *   y     — 行坐标 (0 ~ OLED_HEIGHT-1)
 *   on    — true=点亮, false=关闭
 */
void oled_set_pixel(uint16_t x, uint16_t y, bool on);

/*
 * 画水平线
 *
 * 参数:
 *   x     — 起始列
 *   y     — 行
 *   len   — 线长（像素数）
 *   on    — true=点亮, false=关闭
 */
void oled_draw_hline(uint16_t x, uint16_t y, uint16_t len, bool on);

/*
 * 画垂直线
 *
 * 参数:
 *   x     — 列
 *   y     — 起始行
 *   len   — 线长（像素数）
 *   on    — true=点亮, false=关闭
 */
void oled_draw_vline(uint16_t x, uint16_t y, uint16_t len, bool on);

/*
 * 画矩形边框
 *
 * 参数:
 *   x, y  — 左上角坐标
 *   w, h  — 宽度和高度
 *   on    — true=点亮, false=关闭
 */
void oled_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on);

/*
 * 开关显示
 *
 * 参数:
 *   on — true=开启显示, false=关闭显示（省电模式）
 */
void oled_set_display(bool on);

/*
 * OLED 综合测试
 *
 * 执行: 初始化 → 清屏 → 显示测试图案和文字 → 等待 3 秒
 * 用于验证 OLED 硬件连接和驱动配置是否正确。
 *
 * 返回值: 0 成功, 负的错误码失败
 */
int oled_test(void);

#endif /* __OLED1306_H__ */
