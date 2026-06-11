/*
 * W25Q16 SPI NOR Flash 驱动接口
 *
 * 基于 Zephyr 的 jedec,spi-nor 通用驱动，使用标准 Flash API 操作：
 *   - flash_erase() : 擦除（写入前必须先擦除，擦除后所有字节变为 0xFF）
 *   - flash_write() : 写入（目标区域必须已擦除，且按页对齐写入，每页 256 字节）
 *   - flash_read()  : 读取（任意偏移、任意长度）
 *
 * W25Q16 关键参数：
 *   容量     : 16 Mbit = 2 MB = 2,097,152 字节
 *   扇区大小 : 4 KB（擦除的最小单位）
 *   页大小   : 256 字节（写入的最大对齐单位，跨页写入需分多次）
 *   地址空间 : 0x000000 ~ 0x1FFFFF
 */

#ifndef __W25QXX_H__
#define __W25QXX_H__

#include <stdint.h>
#include <stddef.h>

/* W25Q16 扇区大小：4 KB（擦除的最小单位） */
#define W25QXX_SECTOR_SIZE    4096

/* W25Q16 页大小：256 字节（单次写入不能跨页边界） */
#define W25QXX_PAGE_SIZE      256

/* W25Q16 总容量：2 MB */
#define W25QXX_TOTAL_SIZE     (2 * 1024 * 1024)

/*
 * 初始化 W25Q16 Flash 设备
 *
 * 内部通过 DTS 节点 "w25qxx" 获取 jedec,spi-nor 设备实例，
 * 并检查设备是否就绪（SPI 驱动已加载、JEDEC ID 校验通过）
 *
 * 返回：0 成功，负数错误码失败
 */
int w25qxx_init(void);

/*
 * 擦除 Flash 指定区域
 *
 * @offset : 起始偏移地址（必须扇区对齐，即 4KB 对齐）
 * @size   : 擦除大小（必须为扇区大小的整数倍）
 *
 * 注意：Flash 写入前必须先擦除，擦除后所有字节变为 0xFF
 *       擦除是整扇区操作，不能只擦除部分字节
 *
 * 返回：0 成功，负数错误码失败
 */
int w25qxx_erase(uint32_t offset, uint32_t size);

/*
 * 向 Flash 写入数据
 *
 * @offset : 写入起始偏移地址
 * @data   : 待写入的数据
 * @len    : 数据长度
 *
 * 注意：
 *   1. 目标区域必须已擦除（擦除后为 0xFF，写入只能把 1 改为 0）
 *   2. 单次写入不能跨页边界（每页 256 字节），跨页需分多次调用
 *   3. 对已写入的非 0xFF 地址再次写入会导致数据错误
 *
 * 返回：0 成功，负数错误码失败
 */
int w25qxx_write(uint32_t offset, const uint8_t *data, size_t len);

/*
 * 从 Flash 读取数据
 *
 * @offset : 读取起始偏移地址
 * @buf    : 接收数据的缓冲区
 * @len    : 读取长度
 *
 * 读取无对齐限制，任意偏移、任意长度均可
 *
 * 返回：0 成功，负数错误码失败
 */
int w25qxx_read(uint32_t offset, uint8_t *buf, size_t len);

/*
 * W25Q16 简单测试：擦除 → 写入 → 读回验证
 *
 * 使用最后一个扇区（0x1FF000）进行测试，不影响其他区域数据
 * 测试流程：
 *   1. 擦除最后一个扇区
 *   2. 验证擦除后全为 0xFF
 *   3. 写入测试数据 {0x55, 0xAA, 0x66, 0x99}
 *   4. 读回并比对
 *
 * 返回：0 测试通过，负数错误码测试失败
 */
int w25qxx_test(void);

#endif /* __W25QXX_H__ */
