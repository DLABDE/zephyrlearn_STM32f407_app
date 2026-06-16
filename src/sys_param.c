/*
 * 系统参数管理模块实现
 *
 * 本文件基于 Zephyr Settings 子系统实现参数的持久化存储，
 * 采用表格驱动设计，持久化参数与运行时参数分离管理。
 *
 * Settings 子系统集成方式：
 *   - 使用 settings_save_one / settings_load_one 的简单模式
 *   - 无需实现 Handler（h_set/h_export 等回调），代码更简洁
 *   - 每个持久化参数对应一个 Settings 键值对
 *   - 键名格式："param/<key>"，如 "param/device_addr"
 *   - Settings File 后端将所有键值对存储在 LittleFS 文件中
 *
 * 数据流（以 sys_param_set 为例）：
 *   sys_param_set(PARAM_DEVICE_ADDR, 100)
 *     → 范围校验 [1, 247]
 *     → 更新 param_values[0] = 100
 *     → settings_save_one("param/device_addr", &val, 4)
 *       → Settings File 后端写入 "/lfs1/settings/run" 文件
 *         → VFS fs_write → LittleFS → Flash Map → SPI NOR → W25Q16
 */

#include "sys_param.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h> /* settings_save_one / settings_load_one / settings_subsys_init */
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(sys_param, LOG_LEVEL_INF);

/* ========== Settings 键名前缀 ========== */

/*
 * 所有持久化参数的 Settings 键名以此为前缀
 * 例如：PARAM_DEVICE_ADDR 的完整键名为 "param/device_addr"
 *
 * Settings 子系统使用树形键名空间，"/" 为分隔符
 * 前缀 "param" 作为子树名称，便于未来扩展（如 "log/xxx"）
 */
#define SETTINGS_KEY_PREFIX "param/"

/* ========== 持久化参数描述表 ========== */

/*
 * 表格驱动设计：每个参数的元信息在此统一定义
 * 新增参数只需在此表和 param_index_t 枚举中添加条目
 *
 * 使用指定初始化器（[PARAM_XXX] = {...}）确保枚举与表项一一对应
 * 即使调整枚举顺序，表项也能正确关联
 *
 * 字段说明：
 *   name       - 参数名称（中文描述，用于日志显示）
 *   key        - Settings 存储键名（不含前缀，代码自动拼接 "param/"）
 *   min_val    - 最小值（范围校验用）
 *   max_val    - 最大值（范围校验用）
 *   default_val- 默认值（首次启动或重置时使用）
 */
static const param_item_t param_items[PARAM_COUNT] = {
	[PARAM_DEVICE_ADDR]       = {"设备地址",       "device_addr",      1,     247,   1},
	[PARAM_BAUD_RATE_IDX]     = {"波特率索引",     "baud_rate_idx",    0,     5,     3},
	[PARAM_SAMPLE_PERIOD]     = {"采样周期ms",     "sample_period",    100,   10000, 1000},
	[PARAM_TEMP_ALARM_HIGH]   = {"高温报警阈值",   "temp_alarm_high",  0,     100,   80},
	[PARAM_TEMP_ALARM_LOW]    = {"低温报警阈值",   "temp_alarm_low",   -40,   50,    -10},
	[PARAM_CHANNEL_MAX_VOLTS] = {"最大电压x10", "ch_max_volts",    0,     12000, 10000},
	[PARAM_CHANNEL_MAX_CUR]   = {"最大电流x10", "ch_max_cur",      0,     12000, 6000},
};

/* ========== 运行时参数描述表 ========== */

/*
 * 运行时参数不通过 Settings 存储，key 字段仅用于标识和日志
 * 每次启动都从默认值开始，适用于实时状态、传感器读数等
 */
static const param_item_t rt_param_items[RT_PARAM_COUNT] = {
	[RT_PARAM_WORK_STATUS] = {"工作状态",   "work_status",  0,     5,        0},
	[RT_PARAM_TEMP_C]      = {"当前温度",   "temp_c",       -40,   125,      25},
	[RT_PARAM_VOLTAGE_MV]  = {"当前电压mV", "voltage_mv",   0,     65000,    0},
	[RT_PARAM_CURRENT_MA]  = {"当前电流mA", "current_ma",   0,     65000,    0},
	[RT_PARAM_POWER_W]     = {"当前功率W",  "power_w",      0,     65000,    0},
	[RT_PARAM_UPTIME_S]    = {"运行时间s",  "uptime_s",     0,     INT32_MAX, 0},
};

/* ========== 参数值存储 ========== */

/*
 * 参数值数组：连续存储，索引与描述表一一对应
 *
 * 设计考量：
 *   - 连续内存布局，支持 memcpy 批量读写（Modbus 友好）
 *   - 持久化参数和运行时参数各自独立数组，互不干扰
 *   - 索引即为数组下标，O(1) 访问
 *
 * 生命周期：
 *   - param_values[]：init 时从 Settings 加载，set 时更新内存 + Flash
 *   - rt_param_values[]：init 时设为默认值，set 时仅更新内存
 */
static int32_t param_values[PARAM_COUNT];
static int32_t rt_param_values[RT_PARAM_COUNT];

/* 模块初始化标志，用于判断是否可以调用 settings_save_one */
static bool param_initialized;

/* ========== 内部辅助函数 ========== */

/*
 * 构建完整的 Settings 键名
 *
 * 将前缀 "param/" 与参数 key 拼接为完整键名
 * 例如：key="device_addr" → full_key="param/device_addr"
 *
 * 返回：0 成功，负数 键名过长
 */
static int build_settings_key(char *buf, size_t buf_size, const char *key)
{
	int ret = snprintf(buf, buf_size, SETTINGS_KEY_PREFIX "%s", key);

	if (ret < 0 || (size_t)ret >= buf_size) {
		return -EINVAL;
	}
	return 0;
}

/*
 * 范围校验
 *
 * 检查 value 是否在 [min_val, max_val] 闭区间内
 */
static bool is_value_in_range(int32_t value, int32_t min_val, int32_t max_val)
{
	return (value >= min_val && value <= max_val);
}

/* ========== 持久化参数实现 ========== */

int32_t sys_param_get(param_index_t index)
{
	if (index >= PARAM_COUNT) {
		LOG_ERR("Invalid param index: %d", index);
		return 0;
	}
	return param_values[index];
}

int sys_param_set(param_index_t index, int32_t value)
{
	if (index >= PARAM_COUNT) {
		return PARAM_ERR_INVALID_INDEX;
	}

	/* 范围校验：值必须在 [min_val, max_val] 范围内 */
	if (!is_value_in_range(value, param_items[index].min_val,
			       param_items[index].max_val)) {
		LOG_WRN("Param [%d] %s value %d out of range [%d, %d]",
			index, param_items[index].name, value,
			param_items[index].min_val, param_items[index].max_val);
		return PARAM_ERR_OUT_OF_RANGE;
	}

	/* 更新内存值 */
	param_values[index] = value;

	/*
	 * 立即保存到 Settings（Flash）
	 *
	 * 使用 settings_save_one 直接保存单个键值对：
	 *   - 无需实现 Handler 的 h_export 回调
	 *   - LittleFS 有磨损均衡，频繁写入可接受
	 *   - 掉电安全：LittleFS 保证文件系统一致性
	 *
	 * 仅在模块初始化完成后才保存，避免初始化过程中重复写入
	 */
	if (param_initialized) {
		char key[64];
		int rc = build_settings_key(key, sizeof(key),
					    param_items[index].key);
		if (rc != 0) {
			LOG_ERR("Build key failed for %s",
				param_items[index].key);
			return PARAM_ERR_SAVE_FAILED;
		}

		rc = settings_save_one(key, &value, sizeof(value));
		if (rc != 0) {
			LOG_ERR("Save param %s failed: %d", key, rc);
			return PARAM_ERR_SAVE_FAILED;
		}

		LOG_INF("Saved [%d] %s = %d", index,
			param_items[index].name, value);
	}

	return PARAM_OK;
}

int sys_param_reset(param_index_t index)
{
	if (index >= PARAM_COUNT) {
		return PARAM_ERR_INVALID_INDEX;
	}

	/* 重置为默认值并保存 */
	return sys_param_set(index, param_items[index].default_val);
}

int sys_param_reset_all(void)
{
	for (int i = 0; i < PARAM_COUNT; i++) {
		int rc = sys_param_reset((param_index_t)i);
		if (rc != PARAM_OK) {
			LOG_ERR("Reset param [%d] failed: %d", i, rc);
			return rc;
		}
	}

	LOG_INF("All params reset to defaults");
	return PARAM_OK;
}

int sys_param_get_count(void)
{
	return PARAM_COUNT;
}

int sys_param_get_info(param_index_t index, param_item_t *item)
{
	if (index >= PARAM_COUNT || item == NULL) {
		return PARAM_ERR_INVALID_INDEX;
	}
	*item = param_items[index];
	return PARAM_OK;
}

/* ========== 运行时参数实现 ========== */

int32_t sys_rtparam_get(rt_param_index_t index)
{
	if (index >= RT_PARAM_COUNT) {
		LOG_ERR("Invalid rt_param index: %d", index);
		return 0;
	}
	return rt_param_values[index];
}

int sys_rtparam_set(rt_param_index_t index, int32_t value)
{
	if (index >= RT_PARAM_COUNT) {
		return PARAM_ERR_INVALID_INDEX;
	}

	/* 范围校验 */
	if (!is_value_in_range(value, rt_param_items[index].min_val,
			       rt_param_items[index].max_val)) {
		LOG_WRN("RT param [%d] %s value %d out of range [%d, %d]",
			index, rt_param_items[index].name, value,
			rt_param_items[index].min_val,
			rt_param_items[index].max_val);
		return PARAM_ERR_OUT_OF_RANGE;
	}

	/* 仅更新内存值，不写入 Flash */
	rt_param_values[index] = value;
	return PARAM_OK;
}

int sys_rtparam_get_count(void)
{
	return RT_PARAM_COUNT;
}

int sys_rtparam_get_info(rt_param_index_t index, param_item_t *item)
{
	if (index >= RT_PARAM_COUNT || item == NULL) {
		return PARAM_ERR_INVALID_INDEX;
	}
	*item = rt_param_items[index];
	return PARAM_OK;
}

/* ========== 批量访问接口 ========== */

int sys_param_get_batch(param_index_t start, int32_t *buf, int count)
{
	if (start >= PARAM_COUNT || buf == NULL || count <= 0) {
		return -EINVAL;
	}

	/* 限制读取范围不超过参数总数 */
	int actual = (start + count > PARAM_COUNT)
		     ? (PARAM_COUNT - start) : count;

	/*
	 * 连续内存布局，直接 memcpy 批量拷贝
	 * 这是将参数值存储在连续数组中的核心优势
	 * Modbus 读取一段连续寄存器时，只需一次 memcpy
	 */
	memcpy(buf, &param_values[start], actual * sizeof(int32_t));
	return actual;
}

int sys_param_set_batch(param_index_t start, const int32_t *buf, int count)
{
	if (start >= PARAM_COUNT || buf == NULL || count <= 0) {
		return PARAM_ERR_INVALID_INDEX;
	}

	/*
	 * 逐个校验并设置
	 *
	 * 不能直接 memcpy，因为每个值都需要范围校验和 Settings 保存
	 * 遇到校验失败的参数时停止，返回已成功设置的数量
	 */
	int set_count = 0;

	for (int i = 0; i < count && (start + i) < PARAM_COUNT; i++) {
		int rc = sys_param_set((param_index_t)(start + i), buf[i]);
		if (rc != PARAM_OK) {
			LOG_WRN("Batch set failed at index %d: %d",
				start + i, rc);
			break;
		}
		set_count++;
	}

	return set_count;
}

int sys_rtparam_get_batch(rt_param_index_t start, int32_t *buf, int count)
{
	if (start >= RT_PARAM_COUNT || buf == NULL || count <= 0) {
		return -EINVAL;
	}

	int actual = (start + count > RT_PARAM_COUNT)
		     ? (RT_PARAM_COUNT - start) : count;

	memcpy(buf, &rt_param_values[start], actual * sizeof(int32_t));
	return actual;
}

/* ========== 调试接口 ========== */

void sys_param_print_all(void)
{
	LOG_INF("===== Persistent Parameters =====");
	LOG_INF("%-4s %-16s %-10s %-10s %-10s",
		"Idx", "Name", "Min", "Max", "Value");

	for (int i = 0; i < PARAM_COUNT; i++) {
		LOG_INF("%-4d %-16s %-10d %-10d %-10d",
			i, param_items[i].name,
			param_items[i].min_val, param_items[i].max_val,
			param_values[i]);
	}

	LOG_INF("===== Runtime Parameters =====");
	LOG_INF("%-4s %-16s %-10s %-10s %-10s",
		"Idx", "Name", "Min", "Max", "Value");

	for (int i = 0; i < RT_PARAM_COUNT; i++) {
		LOG_INF("%-4d %-16s %-10d %-10d %-10d",
			i, rt_param_items[i].name,
			rt_param_items[i].min_val,
			rt_param_items[i].max_val,
			rt_param_values[i]);
	}
}

int sys_param_test(void)
{
	LOG_INF("===== Param System Test Start =====");

	/* 测试 1：读取所有参数默认值 */
	LOG_INF("Test 1: Read all default values");
	for (int i = 0; i < PARAM_COUNT; i++) {
		int32_t val = sys_param_get((param_index_t)i);

		LOG_INF("  [%d] %s = %d (default: %d)",
			i, param_items[i].name, val,
			param_items[i].default_val);
	}

	/* 测试 2：设置持久化参数 */
	LOG_INF("Test 2: Set persistent params");
	int rc = sys_param_set(PARAM_DEVICE_ADDR, 100);

	if (rc != PARAM_OK) {
		LOG_ERR("Set PARAM_DEVICE_ADDR failed: %d", rc);
		return -1;
	}
	rc = sys_param_set(PARAM_TEMP_ALARM_HIGH, 95);
	if (rc != PARAM_OK) {
		LOG_ERR("Set PARAM_TEMP_ALARM_HIGH failed: %d", rc);
		return -1;
	}
	rc = sys_param_set(PARAM_CHANNEL_MAX_VOLTS, 888);
	if (rc != PARAM_OK) {
		LOG_ERR("Set PARAM_CHANNEL_MAX_VOLTS failed: %d", rc);
		return -1;
	}

	/* 测试 3：验证持久化参数值 */
	LOG_INF("Test 3: Verify persistent params");
	int32_t addr = sys_param_get(PARAM_DEVICE_ADDR);
	int32_t alarm = sys_param_get(PARAM_TEMP_ALARM_HIGH);

	if (addr != 100 || alarm != 95) {
		LOG_ERR("Verify failed: addr=%d (expect 100), alarm=%d (expect 95)",
			addr, alarm);
		return -1;
	}
	LOG_INF("  device_addr = %d, temp_alarm_high = %d (PASSED)",
		addr, alarm);

	/* 测试 4：范围校验 */
	LOG_INF("Test 4: Range validation");
	rc = sys_param_set(PARAM_DEVICE_ADDR, 0); /* 低于最小值 1 */
	if (rc != PARAM_ERR_OUT_OF_RANGE) {
		LOG_ERR("Range check failed: expected OUT_OF_RANGE, got %d", rc);
		return -1;
	}
	rc = sys_param_set(PARAM_DEVICE_ADDR, 300); /* 高于最大值 247 */
	if (rc != PARAM_ERR_OUT_OF_RANGE) {
		LOG_ERR("Range check failed: expected OUT_OF_RANGE, got %d", rc);
		return -1;
	}
	LOG_INF("  Out-of-range values correctly rejected (PASSED)");

	/* 测试 5：运行时参数 */
	LOG_INF("Test 5: Runtime params");
	rc = sys_rtparam_set(RT_PARAM_TEMP_C, 35);
	if (rc != PARAM_OK) {
		LOG_ERR("Set RT_PARAM_TEMP_C failed: %d", rc);
		return -1;
	}
	int32_t temp = sys_rtparam_get(RT_PARAM_TEMP_C);

	if (temp != 35) {
		LOG_ERR("RT param verify failed: temp=%d (expect 35)", temp);
		return -1;
	}
	LOG_INF("  temp_c = %d (PASSED)", temp);

	/* 测试 6：批量读取 */
	LOG_INF("Test 6: Batch read");
	int32_t batch_buf[PARAM_COUNT];
	int read_count = sys_param_get_batch(PARAM_DEVICE_ADDR,
					     batch_buf, PARAM_COUNT);

	if (read_count > 0) {
		LOG_INF("  Batch read %d params:", read_count);
		for (int i = 0; i < read_count; i++) {
			LOG_INF("    [%d] = %d", i, batch_buf[i]);
		}
	}

	/* 测试 7：重置参数 */
	LOG_INF("Test 7: Reset params");
	rc = sys_param_reset(PARAM_DEVICE_ADDR);
	if (rc != PARAM_OK) {
		LOG_ERR("Reset PARAM_DEVICE_ADDR failed: %d", rc);
		return -1;
	}
	addr = sys_param_get(PARAM_DEVICE_ADDR);
	if (addr != param_items[PARAM_DEVICE_ADDR].default_val) {
		LOG_ERR("Reset verify failed: addr=%d (expect %d)",
			addr, param_items[PARAM_DEVICE_ADDR].default_val);
		return -1;
	}
	LOG_INF("  device_addr reset to %d (PASSED)", addr);

	/* 打印最终状态 */
	sys_param_print_all();

	LOG_INF("===== Param System Test Complete =====");
	return 0;
}

/* ========== 初始化 ========== */

int sys_param_init(void)
{
	int rc;

	/*
	 * 初始化 Settings 子系统
	 *
	 * 前置条件：LittleFS 文件系统已挂载（fs_storage_init() 已调用）
	 * Settings File 后端将在 /lfs1/settings/run 文件中读写键值对
	 *
	 * settings_subsys_init() 内部流程：
	 *   1. 调用后端的 settings_backend_init()
	 *   2. File 后端创建 /lfs1/settings/ 目录（如不存在）
	 *   3. 打开或创建 /lfs1/settings/run 文件
	 *
	 * 注意：重复调用返回 0（init_fn 置 NULL 后不再执行）
	 */
	rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("Settings subsystem init failed: %d", rc);
		return PARAM_ERR_LOAD_FAILED;
	}
	LOG_INF("Settings subsystem initialized");

	/*
	 * 从 Settings 加载持久化参数
	 *
	 * 使用 settings_load_one 逐个加载每个参数：
	 *   - 键存在且值有效 → 读取到 param_values[]
	 *   - 键不存在（首次启动）→ 使用默认值
	 *   - 值超出范围（数据损坏）→ 使用默认值并修正存储
	 *
	 * 相比 Handler 模式（h_set/h_export），load_one 更简单直观：
	 *   - 无需实现回调函数
	 *   - 无需注册 handler
	 *   - 适合参数数量不多的场景
	 */
	for (int i = 0; i < PARAM_COUNT; i++) {
		char key[64];

		rc = build_settings_key(key, sizeof(key), param_items[i].key);
		if (rc != 0) {
			LOG_ERR("Build key failed for param %d", i);
			param_values[i] = param_items[i].default_val;
			continue;
		}

		int32_t val;
		/*
		 * settings_load_one 返回值：
		 *   > 0 : 成功读取的字节数
		 *   0   : 键不存在
		 *   < 0 : 错误码
		 */
		ssize_t len = settings_load_one(key, &val, sizeof(val));

		if (len == sizeof(val)) {
			/* 成功读取，校验范围 */
			if (is_value_in_range(val, param_items[i].min_val,
					      param_items[i].max_val)) {
				param_values[i] = val;
				LOG_INF("Loaded [%d] %s = %d",
					i, param_items[i].name, val);
			} else {
				/*
				 * 值超出范围，使用默认值并修正存储
				 *
				 * 这是 Settings 子系统没有的内置校验，
				 * 需要应用层自行实现
				 */
				LOG_WRN("Loaded [%d] %s = %d out of range, using default %d",
					i, param_items[i].name, val,
					param_items[i].default_val);
				param_values[i] = param_items[i].default_val;
				settings_save_one(key,
						  &param_items[i].default_val,
						  sizeof(int32_t));
			}
		} else {
			/*
			 * 键不存在或读取失败，使用默认值并持久化
			 *
			 * 当参数文件不存在时（首次启动、OTA升级后、
			 * 文件系统损坏），仅设置内存默认值是不够的 ——
			 * 下次启动 Settings File 后端仍会因文件缺失而
			 * 报告 "file open error"。
			 *
			 * 主动调用 settings_save_one 可以：
			 *   1. 创建 Settings 文件（内部 fs_open 带
			 *      FS_O_CREATE 标志，自动新建文件）
			 *   2. 持久化默认值，下次启动直接读取成功
			 *   3. 即使保存失败也不影响系统运行
			 *      （内存默认值已生效，下次启动重试）
			 *
			 * 注意：settings_save_one 内部会检查重复值，
			 * 首次写入时文件不存在，会直接创建并写入。
			 */
			param_values[i] = param_items[i].default_val;
			LOG_INF("Param [%d] %s not found, using default %d",
				i, param_items[i].name,
				param_items[i].default_val);
			rc = settings_save_one(key,
					      &param_items[i].default_val,
					      sizeof(int32_t));
			if (rc != 0) {
				LOG_WRN("Save default for [%d] %s failed: %d",
					i, param_items[i].name, rc);
			}
		}
	}

	/*
	 * 设置运行时参数为默认值
	 *
	 * 运行时参数不持久化，每次启动都从默认值开始
	 * 应用代码在运行过程中通过 sys_rtparam_set 更新
	 */
	for (int i = 0; i < RT_PARAM_COUNT; i++) {
		rt_param_values[i] = rt_param_items[i].default_val;
	}

	param_initialized = true;

	LOG_INF("Param system initialized: %d persistent, %d runtime",
		PARAM_COUNT, RT_PARAM_COUNT);

	return PARAM_OK;
}
