/*
 * 文件系统存储实现
 *
 * 本文件封装了 Zephyr VFS 文件操作 API，提供简洁的文件读写接口。
 * 底层使用 LittleFS 文件系统，通过 DTS fstab 机制自动挂载。
 *
 * 关键概念：
 *   - VFS（虚拟文件系统）：Zephyr 的统一文件操作层，fs_open/fs_read/fs_write 等
 *     API 不关心底层是 LittleFS 还是 FatFS，由 VFS 根据路径前缀自动路由
 *   - LittleFS：专为嵌入式设计的文件系统，掉电安全（不会损坏文件系统）、
 *     磨损均衡（自动分散写入位置延长 Flash 寿命）、极小 RAM 开销
 *   - fstab：DTS 中的文件系统挂载表，声明挂载参数后构建系统自动生成
 *     fs_mount_t 结构体，应用代码通过 FS_FSTAB_DECLARE_ENTRY 引用
 *   - automount：DTS 中设置后，LittleFS 在 SYS_INIT 阶段自动挂载，
 *     应用代码无需手动调用 fs_mount()
 *
 * 数据流（以 fs_write 为例）：
 *   fs_storage_write_file()
 *     → fs_open() / fs_write() / fs_close()        [VFS 层]
 *       → littlefs_write() → lfs_file_write()       [LittleFS 层]
 *         → lfs_api_prog() → flash_area_write()     [Flash Map 层]
 *           → spi_nor_write() → SPI 发送命令帧       [硬件驱动层]
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>             /* fs_open / fs_read / fs_write / fs_mount 等 VFS API */
#include <zephyr/fs/littlefs.h>       /* FS_FSTAB_DECLARE_ENTRY 宏 */
#include <zephyr/logging/log.h>
#include <string.h>
#include "fs_storage.h"

LOG_MODULE_REGISTER(fs_storage, LOG_LEVEL_INF);

/*
 * 通过 DTS fstab 声明 LittleFS 挂载点
 *
 * DT_NODELABEL(lfs1) 对应 DTS 中 lfs1: lfs1 { ... } 节点
 * FS_FSTAB_DECLARE_ENTRY 展开为：
 *   extern struct fs_mount_t z_fsmp_lfs1;
 * 即声明了由构建系统自动生成的 fs_mount_t 结构体
 *
 * 该结构体由 littlefs_fs.c 中的 DEFINE_FS 宏自动生成，包含：
 *   .type = FS_LITTLEFS
 *   .mnt_point = "/lfs1"
 *   .fs_data = &fs_data_lfs1（包含 read/prog/cache 缓冲区）
 *   .storage_dev = flash_area_id（指向 lfs_partition 分区）
 *   .flags = FS_MOUNT_FLAG_AUTOMOUNT（因为 DTS 中设置了 automount）
 */
#define LFS_PARTITION_NODE DT_NODELABEL(lfs1)
FS_FSTAB_DECLARE_ENTRY(LFS_PARTITION_NODE);

/*
 * 构建完整文件路径
 *
 * 如果 path 以 "/" 开头，视为绝对路径直接使用
 * 否则拼接挂载点前缀，如 "config.txt" → "/lfs1/config.txt"
 */
static int build_full_path(char *buf, size_t buf_size, const char *path)
{
	if (path[0] == '/') {
		/* 绝对路径，直接使用 */
		return snprintf(buf, buf_size, "%s", path);
	}
	/* 相对路径，拼接挂载点 */
	return snprintf(buf, buf_size, "%s/%s", FS_STORAGE_MOUNT_POINT, path);
}

int fs_storage_init(void)
{
	/*
	 * 获取 DTS fstab 自动生成的 fs_mount_t 结构体指针
	 *
	 * FS_FSTAB_ENTRY(LFS_PARTITION_NODE) 展开为 z_fsmp_lfs1
	 * 这是构建系统根据 DTS 中的 fstab 条目自动定义的全局变量
	 */
	struct fs_mount_t *mp = &FS_FSTAB_ENTRY(LFS_PARTITION_NODE);

	/*
	 * 检查文件系统是否已挂载
	 *
	 * 如果 DTS 中设置了 automount 且启用了 CONFIG_FS_LITTLEFS_FSTAB_AUTOMOUNT，
	 * LittleFS 会在 SYS_INIT 阶段（main() 之前）自动挂载
	 * 首次挂载时如果分区未格式化，LittleFS 会自动格式化后再挂载
	 */
	struct fs_statvfs stat;
	int rc = fs_statvfs(FS_STORAGE_MOUNT_POINT, &stat);
	if (rc == 0) {
		LOG_INF("LittleFS already mounted at %s", FS_STORAGE_MOUNT_POINT);
		LOG_INF("  Total: %lu bytes, Free: %lu bytes",
			(unsigned long)(stat.f_bsize * stat.f_blocks),
			(unsigned long)(stat.f_bsize * stat.f_bfree));
		return 0;
	}

	/*
	 * 如果自动挂载失败，尝试手动挂载
	 * 这通常发生在 automount 未启用或首次格式化失败时
	 */
	LOG_WRN("Auto-mount not detected, mounting manually...");
	rc = fs_mount(mp);
	if (rc != 0) {
		LOG_ERR("Manual mount failed: %d", rc);
		return rc;
	}

	LOG_INF("LittleFS mounted at %s", FS_STORAGE_MOUNT_POINT);
	return 0;
}

int fs_storage_write_file(const char *path, const uint8_t *data, size_t len)
{
	char full_path[64];
	struct fs_file_t file;
	int rc;

	/* 构建完整路径 */
	rc = build_full_path(full_path, sizeof(full_path), path);
	if (rc < 0 || (size_t)rc >= sizeof(full_path)) {
		LOG_ERR("Path too long: %s", path);
		return -EINVAL;
	}

	/*
	 * fs_file_t_init：初始化文件对象（必须在使用前调用）
	 * 类似于 C 标准库中 fopen 之前清零 FILE 结构体
	 */
	fs_file_t_init(&file);

	/*
	 * fs_open：打开文件
	 *   FS_O_CREATE : 文件不存在则创建
	 *   FS_O_WRITE  : 只写模式
	 *   FS_O_TRUNC  : 截断文件（清空已有内容后写入）
	 *
	 * 其他可用标志：
	 *   FS_O_READ   : 只读
	 *   FS_O_RDWR   : 读写
	 *   FS_O_APPEND : 追加模式（写入位置在文件末尾）
	 */
	rc = fs_open(&file, full_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc < 0) {
		LOG_ERR("Failed to open %s for write: %d", full_path, rc);
		return rc;
	}

	/*
	 * fs_write：向文件写入数据
	 *   返回值：成功写入的字节数，负数表示错误
	 *   LittleFS 内部会自动处理：
	 *     - 写使能（WREN 命令）
	 *     - 按页编程（每页 256 字节，跨页自动分次）
	 *     - 等待写入完成（轮询状态寄存器 BUSY 位）
	 *     - 磨损均衡（block-cycles 决定换块频率）
	 */
	ssize_t written = fs_write(&file, data, len);
	if (written < 0) {
		LOG_ERR("Failed to write %s: %zd", full_path, written);
		fs_close(&file);
		return (int)written;
	}
	if ((size_t)written != len) {
		LOG_WRN("Partial write: %zu/%zu bytes", (size_t)written, len);
	}

	/* fs_close：关闭文件，刷新缓存到 Flash */
	fs_close(&file);

	LOG_INF("Written %zd bytes to %s", written, full_path);
	return 0;
}

int fs_storage_read_file(const char *path, uint8_t *buf, size_t buf_len,
			 size_t *out_len)
{
	char full_path[64];
	struct fs_file_t file;
	int rc;

	rc = build_full_path(full_path, sizeof(full_path), path);
	if (rc < 0 || (size_t)rc >= sizeof(full_path)) {
		LOG_ERR("Path too long: %s", path);
		return -EINVAL;
	}

	fs_file_t_init(&file);

	/* 以只读模式打开 */
	rc = fs_open(&file, full_path, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("Failed to open %s for read: %d", full_path, rc);
		return rc;
	}

	/*
	 * fs_read：从文件读取数据
	 *   返回值：实际读取的字节数（可能小于 buf_len），0 表示文件末尾，负数表示错误
	 *   首次打开文件时，读取位置在文件开头（offset=0）
	 */
	ssize_t bytes_read = fs_read(&file, buf, buf_len);
	fs_close(&file);

	if (bytes_read < 0) {
		LOG_ERR("Failed to read %s: %zd", full_path, bytes_read);
		return (int)bytes_read;
	}

	if (out_len != NULL) {
		*out_len = (size_t)bytes_read;
	}

	LOG_INF("Read %zd bytes from %s", bytes_read, full_path);
	return 0;
}

int fs_storage_test(void)
{
	/*
	 * 测试场景：保存一个设备参数到文件，然后读回验证
	 *
	 * 这模拟了实际应用中最常见的场景：
	 *   - 设备首次启动，参数文件不存在 → 创建并写入默认值
	 *   - 设备后续启动，读取已保存的参数
	 *   - 参数变更时，覆盖写入新值
	 */
	const char *test_path = "param.txt";
	/* 测试参数：设备地址=1, 波特率=115200, 采样周期=1000ms */
	const char *test_param = "addr=1\nbaud=115200\nperiod=1000\n";
	size_t param_len = strlen(test_param);
	uint8_t read_buf[128];
	size_t read_len;
	int rc;

	LOG_INF("=== FS Storage Test Start ===");

	/* 步骤 1：写入参数文件 */
	LOG_INF("Step 1: Writing param file...");
	rc = fs_storage_write_file(test_path,
				   (const uint8_t *)test_param, param_len);
	if (rc != 0) {
		LOG_ERR("Write failed: %d", rc);
		return rc;
	}

	/* 步骤 2：读回参数文件 */
	LOG_INF("Step 2: Reading param file...");
	rc = fs_storage_read_file(test_path, read_buf, sizeof(read_buf) - 1,
				  &read_len);
	if (rc != 0) {
		LOG_ERR("Read failed: %d", rc);
		return rc;
	}

	/* 步骤 3：验证内容 */
	read_buf[read_len] = '\0'; /* 确保字符串以 null 结尾 */
	if (read_len == param_len &&
	    memcmp(test_param, read_buf, param_len) == 0) {
		LOG_INF("Step 3: Verify PASSED!");
		LOG_INF("Content:\n%s", read_buf);
	} else {
		LOG_ERR("Step 3: Verify FAILED!");
		LOG_ERR("Expected %zu bytes, got %zu bytes", param_len,
			read_len);
		return -EIO;
	}

	/* 步骤 4：列出目录内容 */
	LOG_INF("Step 4: Listing /lfs1/ directory...");
	struct fs_dir_t dir;
	struct fs_dirent entry;

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, FS_STORAGE_MOUNT_POINT);
	if (rc != 0) {
		LOG_ERR("opendir failed: %d", rc);
		return rc;
	}

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		const char *type = (entry.type == FS_DIR_ENTRY_DIR)
					   ? "DIR "
					   : "FILE";
		LOG_INF("  [%s] %s (%zu bytes)", type, entry.name,
			(size_t)entry.size);
	}
	fs_closedir(&dir);

	LOG_INF("=== FS Storage Test Complete ===");
	return 0;
}
