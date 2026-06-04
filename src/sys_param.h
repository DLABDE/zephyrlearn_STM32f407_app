/*
 * 系统参数管理模块
 *
 * 基于 Zephyr Settings 子系统实现参数持久化存储，提供表格驱动的参数管理接口。
 *
 * 设计要点：
 *   - 持久化参数：通过 Settings 子系统存储到 LittleFS，掉电不丢失
 *   - 运行时参数：仅存于内存，每次启动重置为默认值
 *   - 所有参数统一使用 int32_t 类型
 *   - 持久化参数和运行时参数分离，各自独立的索引空间
 *   - 支持批量读写接口，便于 Modbus 等协议访问
 *
 * 架构：
 *   应用代码 → sys_param API → Settings API → File 后端 → LittleFS → W25Q16 Flash
 *
 * Settings 子系统集成说明：
 *   - 使用 settings_save_one / settings_load_one 的简单模式
 *   - 每个持久化参数对应一个 Settings 键值对，键名格式 "param/<key>"
 *   - Settings File 后端将所有键值对存储在 /lfs1/settings/run 文件中
 *   - 无需实现 Handler（h_set/h_export 等），代码更简洁
 *
 * 使用流程：
 *   1. 确保 fs_storage_init() 已调用（LittleFS 已挂载）
 *   2. 调用 sys_param_init() 初始化参数系统
 *   3. 使用 sys_param_get/set 读写持久化参数
 *   4. 使用 sys_rtparam_get/set 读写运行时参数
 *   5. 使用 sys_param_get_batch/set_batch 批量访问（Modbus 用）
 */

#ifndef __SYS_PARAM_H__
#define __SYS_PARAM_H__

#include <stdint.h>
#include <stddef.h>

/* ========== 错误码定义 ========== */

typedef enum {
	PARAM_OK = 0,		/* 操作成功 */
	PARAM_ERR_INVALID_INDEX, /* 参数索引无效 */
	PARAM_ERR_OUT_OF_RANGE,	/* 参数值超出范围 */
	PARAM_ERR_SAVE_FAILED,	/* 保存失败 */
	PARAM_ERR_LOAD_FAILED,	/* 加载失败 */
	PARAM_ERR_NOT_INIT,	/* 模块未初始化 */
} param_err_t;

/* ========== 持久化参数索引枚举 ========== */

/*
 * 持久化参数：通过 Settings 子系统存储到 Flash，掉电不丢失
 *
 * 索引值即为参数在内部数组中的位置，可直接用于 Modbus 寄存器映射
 * 新增参数：在此枚举中添加（在 PARAM_COUNT 之前），并在 .c 文件描述表中添加对应条目
 */
typedef enum {
	PARAM_DEVICE_ADDR = 0,	/* 设备地址 (1-247, 默认1) */
	PARAM_BAUD_RATE_IDX,	/* 波特率索引 (0-5, 默认3 → 115200) */
	PARAM_SAMPLE_PERIOD,	/* 采样周期 ms (100-10000, 默认1000) */
	PARAM_TEMP_ALARM_HIGH,	/* 高温报警阈值 (0-100, 默认80) */
	PARAM_TEMP_ALARM_LOW,	/* 低温报警阈值 (-40-50, 默认-10) */
	PARAM_CHANNEL_MAX_VOLTS, /* 通道最大电压×10 (0-12000, 默认10000) */
	PARAM_CHANNEL_MAX_CUR,	/* 通道最大电流×10 (0-12000, 默认6000) */

	PARAM_COUNT,		/* 持久化参数总数（不可用作索引） */
} param_index_t;

/* ========== 运行时参数索引枚举 ========== */

/*
 * 运行时参数：仅存于内存，不持久化，每次启动重置为默认值
 * 适用于实时状态、传感器读数等不需要掉电保存的数据
 *
 * 与持久化参数分离，拥有独立的索引空间
 * Modbus 可将持久化参数映射到读写寄存器，运行时参数映射到只读寄存器
 */
typedef enum {
	RT_PARAM_WORK_STATUS = 0, /* 工作状态 (0-5, 默认0) */
	RT_PARAM_TEMP_C,	/* 当前温度 (-40-125, 默认25) */
	RT_PARAM_VOLTAGE_MV,	/* 当前电压 mV (0-65000, 默认0) */
	RT_PARAM_CURRENT_MA,	/* 当前电流 mA (0-65000, 默认0) */
	RT_PARAM_POWER_W,	/* 当前功率 W (0-65000, 默认0) */
	RT_PARAM_UPTIME_S,	/* 运行时间 s (0-2^31-1, 默认0) */

	RT_PARAM_COUNT,		/* 运行时参数总数（不可用作索引） */
} rt_param_index_t;

/* ========== 参数描述符结构体 ========== */

/*
 * 参数描述符：定义每个参数的元信息
 *
 * 用于表格驱动设计，新增参数只需在描述表中添加条目
 * - name:       中文描述，用于日志和显示
 * - key:        Settings 存储键名（不含前缀，代码自动拼接 "param/" 前缀）
 *               运行时参数的 key 仅用于标识，不参与 Settings 存储
 * - min_val:    参数最小值（范围校验用）
 * - max_val:    参数最大值（范围校验用）
 * - default_val:默认值（首次启动或重置时使用）
 */
typedef struct {
	const char *name;	/* 参数名称（中文描述） */
	const char *key;	/* Settings 存储键名 */
	int32_t min_val;	/* 最小值 */
	int32_t max_val;	/* 最大值 */
	int32_t default_val;	/* 默认值 */
} param_item_t;

/* ========== 初始化 ========== */

/*
 * 初始化参数系统
 *
 * 流程：
 *   1. 初始化 Settings 子系统（依赖 LittleFS 已挂载）
 *   2. 从 Settings 加载持久化参数值（不存在则用默认值）
 *   3. 设置运行时参数为默认值
 *
 * 前置条件：fs_storage_init() 已成功调用（LittleFS 已挂载）
 *
 * 返回：PARAM_OK 成功，其他错误码失败
 */
int sys_param_init(void);

/* ========== 持久化参数操作 ========== */

/* 获取持久化参数值，索引无效时返回 0 */
int32_t sys_param_get(param_index_t index);

/*
 * 设置持久化参数值
 *
 * 流程：范围校验 → 更新内存值 → 立即保存到 Settings（Flash）
 * 返回：PARAM_OK 成功，其他错误码失败
 */
int sys_param_set(param_index_t index, int32_t value);

/* 重置指定持久化参数为默认值 */
int sys_param_reset(param_index_t index);

/* 重置所有持久化参数为默认值 */
int sys_param_reset_all(void);

/* 获取持久化参数数量 */
int sys_param_get_count(void);

/* 获取持久化参数描述信息 */
int sys_param_get_info(param_index_t index, param_item_t *item);

/* ========== 运行时参数操作 ========== */

/* 获取运行时参数值，索引无效时返回 0 */
int32_t sys_rtparam_get(rt_param_index_t index);

/*
 * 设置运行时参数值
 *
 * 仅更新内存值，不写入 Flash
 * 返回：PARAM_OK 成功，其他错误码失败
 */
int sys_rtparam_set(rt_param_index_t index, int32_t value);

/* 获取运行时参数数量 */
int sys_rtparam_get_count(void);

/* 获取运行时参数描述信息 */
int sys_rtparam_get_info(rt_param_index_t index, param_item_t *item);

/* ========== 批量访问接口（Modbus 等协议用） ========== */

/*
 * 批量读取持久化参数
 *
 * 从 start 索引开始，读取 count 个参数值到 buf
 * 返回：实际读取的数量，负数表示错误
 *
 * 适用场景：Modbus 读保持寄存器（功能码 03）
 *   Modbus 寄存器地址 = 参数索引 × 2（int32_t 占 2 个 16 位寄存器）
 */
int sys_param_get_batch(param_index_t start, int32_t *buf, int count);

/*
 * 批量设置持久化参数
 *
 * 从 start 索引开始，逐个校验并设置 count 个参数值
 * 遇到校验失败的参数时停止，返回已成功设置的数量
 *
 * 适用场景：Modbus 写多个寄存器（功能码 16）
 */
int sys_param_set_batch(param_index_t start, const int32_t *buf, int count);

/* 批量读取运行时参数（同 sys_param_get_batch，但操作运行时参数） */
int sys_rtparam_get_batch(rt_param_index_t start, int32_t *buf, int count);

/* ========== 调试接口 ========== */

/* 打印所有参数（持久化 + 运行时） */
void sys_param_print_all(void);

/*
 * 参数系统测试
 *
 * 测试流程：读取默认值 → 设置参数 → 验证 → 范围校验 → 运行时参数 → 批量读取 → 重置
 * 返回：0 成功，负数失败
 */
int sys_param_test(void);

#endif /* __SYS_PARAM_H__ */
