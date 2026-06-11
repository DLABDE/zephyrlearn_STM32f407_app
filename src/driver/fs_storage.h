/*
 * 文件系统存储接口
 *
 * 基于 Zephyr VFS + LittleFS，提供简洁的文件读写接口。
 * 底层存储为 W25Q16 SPI NOR Flash，通过 DTS fstab 自动挂载。
 *
 * 架构：
 *   应用代码 → fs_storage API → VFS (fs_open/fs_write/fs_read) → LittleFS → Flash Map → SPI NOR
 *
 * 使用流程：
 *   1. DTS 中定义 lfs_partition 分区和 fstab 条目（已完成）
 *   2. 调用 fs_storage_init() 检查文件系统是否就绪
 *   3. 使用 fs_storage_write_file() / fs_storage_read_file() 读写文件
 *   4. 使用 fs_storage_test() 运行完整测试
 */

#ifndef __FS_STORAGE_H__
#define __FS_STORAGE_H__

#include <stddef.h>
#include <stdint.h>

/* LittleFS 挂载点路径，与 DTS fstab 中 mount-point 一致 */
#define FS_STORAGE_MOUNT_POINT "/lfs1"

/*
 * 初始化文件系统存储模块
 *
 * 检查 LittleFS 是否已自动挂载（automount），如果未挂载则手动挂载。
 * 首次使用时 LittleFS 会自动格式化分区。
 *
 * 返回：0 成功，负数错误码失败
 */
int fs_storage_init(void);

/*
 * 向文件写入数据
 *
 * @path : 文件路径（相对于挂载点，如 "/lfs1/config.txt" 或直接 "config.txt"）
 * @data : 待写入的数据
 * @len  : 数据长度
 *
 * 写入模式：FS_O_CREATE | FS_O_WRONLY | FS_O_TRUNC
 *   - 文件不存在则创建
 *   - 文件已存在则截断后写入（覆盖旧内容）
 *
 * 返回：0 成功，负数错误码失败
 */
int fs_storage_write_file(const char *path, const uint8_t *data, size_t len);

/*
 * 从文件读取数据
 *
 * @path   : 文件路径
 * @buf    : 接收缓冲区
 * @buf_len: 缓冲区大小
 * @out_len: 实际读取的字节数（可为 NULL）
 *
 * 返回：0 成功，负数错误码失败
 */
int fs_storage_read_file(const char *path, uint8_t *buf, size_t buf_len,
			 size_t *out_len);

/*
 * 文件系统存储测试
 *
 * 测试流程：
 *   1. 写入参数文件 /lfs1/param.txt
 *   2. 读回并验证内容
 *   3. 列出 /lfs1/ 目录内容
 *
 * 返回：0 测试通过，负数错误码测试失败
 */
int fs_storage_test(void);

#endif /* __FS_STORAGE_H__ */
