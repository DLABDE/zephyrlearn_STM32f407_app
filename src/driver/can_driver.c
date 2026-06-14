/**
 * @file    can_driver.c
 * @brief   多路 CAN 驱动模块 — TX/RX 队列 + 状态监控 + bus-off 自恢复
 *
 *
 * 架构 (每路独立):
 *   TX: can_drv_send() → TX 队列 → TX 线程 → can_send()
 *   RX: CAN 中断 → rx_cb 入队 → can_drv_recv() 取帧
 *   状态: CAN 中断 → state_cb → k_work → bus-off 恢复
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/printk.h>

#include "can_driver.h"

/* =========================================================================
 * 设备引用 (从 DTS 节点获取)
 * ========================================================================= */

static const struct device *can_devs[CAN_DEV_COUNT] = {
	[CAN_DEV_CAN1]  = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(can1)),
};

static const char *can_names[CAN_DEV_COUNT] = {
	[CAN_DEV_CAN1]  = "CAN1",
};

/* =========================================================================
 * 每路 CAN 实例的独立数据
 * ========================================================================= */

struct can_instance {
	bool initialized;
	volatile enum can_state current_state;
	volatile struct can_bus_err_cnt pending_err;
	struct k_msgq *tx_msgq;
	struct k_msgq *rx_msgq;
	struct k_work state_work;
	struct k_work_delayable recovery_work;
	struct k_thread tx_thread_data;
	k_thread_stack_t *tx_thread_stack;
};

/** TX 消息队列 (每路 8 帧缓冲) */
K_MSGQ_DEFINE(can1_tx_msgq,  sizeof(struct can_frame), 8, 4);

/** RX 消息队列 (每路 16 帧缓冲) */
K_MSGQ_DEFINE(can1_rx_msgq,  sizeof(struct can_frame), 16, 4);

/** TX 线程栈 */
K_THREAD_STACK_DEFINE(can1_tx_stack,  2048);

static struct can_instance instances[CAN_DEV_COUNT] = {
	[CAN_DEV_CAN1] = {
		.tx_msgq = &can1_tx_msgq,
		.rx_msgq = &can1_rx_msgq,
		.tx_thread_stack = can1_tx_stack,
	},
};

/* =========================================================================
 * 工具函数
 * ========================================================================= */

const char *can_drv_state_str(enum can_state s)
{
	switch (s) {
	case CAN_STATE_ERROR_ACTIVE:  return "error-active";
	case CAN_STATE_ERROR_WARNING: return "error-warning";
	case CAN_STATE_ERROR_PASSIVE: return "error-passive";
	case CAN_STATE_BUS_OFF:       return "bus-off";
	case CAN_STATE_STOPPED:       return "stopped";
	default:                      return "unknown";
	}
}

const char *can_drv_name(enum can_dev_idx idx)
{
	if (idx < 0 || idx >= CAN_DEV_COUNT) {
		return "INVALID";
	}
	return can_names[idx];
}

enum can_state can_drv_get_state(enum can_dev_idx idx)
{
	if (idx < 0 || idx >= CAN_DEV_COUNT) {
		return CAN_STATE_STOPPED;
	}
	return instances[idx].current_state;
}

/* =========================================================================
 * TX 路径
 * ========================================================================= */

static void tx_done_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(error);
	ARG_UNUSED(user_data);
}

static void tx_thread_fn(void *p1, void *p2, void *p3)
{
	enum can_dev_idx idx = (enum can_dev_idx)(intptr_t)p1;
	struct can_instance *inst = &instances[idx];
	const struct device *dev = can_devs[idx];

	struct can_frame frame;

	while (1) {
		if (k_msgq_get(inst->tx_msgq, &frame, K_MSEC(10)) != 0) {
			continue;
		}

		if (inst->current_state == CAN_STATE_BUS_OFF ||
		    inst->current_state == CAN_STATE_STOPPED) {
			continue;
		}

		//printk("[%s] 发送帧: ID=0x%08X, DLC=%d, 数据=%02X%02X\n",
		//       can_names[idx], frame.id, frame.dlc, frame.data[0], frame.data[1]);
		can_send(dev, &frame, K_MSEC(50), tx_done_cb, NULL);
	}
}

int can_drv_send(enum can_dev_idx idx, const struct can_frame *frame, k_timeout_t timeout)
{
	if (idx < 0 || idx >= CAN_DEV_COUNT || !instances[idx].initialized) {
		return -ENODEV;
	}
	return k_msgq_put(instances[idx].tx_msgq, frame, timeout);
}

/* =========================================================================
 * RX 路径
 * ========================================================================= */

static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);

	enum can_dev_idx idx = (enum can_dev_idx)(intptr_t)user_data;
	if (idx < 0 || idx >= CAN_DEV_COUNT) {
		return;
	}
	//printk("[%s] 接收帧: ID=0x%08X, DLC=%d, 数据=%02X%02X%02X%02X%02X%02X%02X%02X\n",
	//      can_names[idx], frame->id, frame->dlc, frame->data[0], frame->data[1], frame->data[2],
	//	  frame->data[3], frame->data[4], frame->data[5], frame->data[6], frame->data[7]);


	k_msgq_put(instances[idx].rx_msgq, frame, K_NO_WAIT);
}

int can_drv_recv(enum can_dev_idx idx, struct can_frame *frame, k_timeout_t timeout)
{
	if (idx < 0 || idx >= CAN_DEV_COUNT || !instances[idx].initialized) {
		return -ENODEV;
	}
	return k_msgq_get(instances[idx].rx_msgq, frame, timeout);
}

/* =========================================================================
 * 状态监控与 bus-off 恢复
 * ========================================================================= */

static void state_change_isr(const struct device *dev,
			     enum can_state state,
			     struct can_bus_err_cnt err_cnt,
			     void *user_data)
{
	ARG_UNUSED(dev);

	enum can_dev_idx idx = (enum can_dev_idx)(intptr_t)user_data;
	if (idx < 0 || idx >= CAN_DEV_COUNT) {
		return;
	}

	instances[idx].current_state = state;
	instances[idx].pending_err = err_cnt;
	k_work_submit(&instances[idx].state_work);
}

static void state_work_handler(struct k_work *work)
{
	/* 从 work 地址反推实例索引 */
	struct can_instance *inst =
		CONTAINER_OF(work, struct can_instance, state_work);
	enum can_dev_idx idx = inst - instances;

	enum can_state state = inst->current_state;
	struct can_bus_err_cnt err = inst->pending_err;

	printk("[%s] 状态: %s (TXerr=%u, RXerr=%u)\n",
	       can_names[idx], can_drv_state_str(state),
	       err.tx_err_cnt, err.rx_err_cnt);

	if (state == CAN_STATE_BUS_OFF) {
		printk("[%s] bus-off! 1 秒后恢复...\n", can_names[idx]);
		k_work_schedule(&inst->recovery_work, K_MSEC(1000));
	}
}

static void recovery_handler(struct k_work *work)
{
	struct can_instance *inst =
		CONTAINER_OF(k_work_delayable_from_work(work),
			     struct can_instance, recovery_work);
	enum can_dev_idx idx = inst - instances;
	const struct device *dev = can_devs[idx];
	int ret;

	can_stop(dev);
	k_msleep(100);

	ret = can_start(dev);
	if (ret == 0) {
		inst->current_state = CAN_STATE_ERROR_ACTIVE;
		printk("[%s] 恢复完成\n", can_names[idx]);
	} else {
		printk("[%s] 恢复失败: %d\n", can_names[idx], ret);
	}
}

/* =========================================================================
 * 初始化
 * ========================================================================= */

int can_drv_init(enum can_dev_idx idx)
{
	if (idx < 0 || idx >= CAN_DEV_COUNT) {
		return -EINVAL;
	}

	struct can_instance *inst = &instances[idx];

	if (inst->initialized) {
		return -EALREADY;
	}

	const struct device *dev = can_devs[idx];

	if (dev == NULL || !device_is_ready(dev)) {
		printk("[%s] 设备未就绪\n", can_names[idx]);
		return -ENODEV;
	}

	/* init k_work */
	k_work_init(&inst->state_work, state_work_handler);
	k_work_init_delayable(&inst->recovery_work, recovery_handler);

	/* 注册状态回调 */
	can_set_state_change_callback(dev, state_change_isr,
				      (void *)(intptr_t)idx);

	/* RX 过滤器: 接受所有扩展帧 */
	const struct can_filter filt = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };
	can_add_rx_filter(dev, rx_cb, (void *)(intptr_t)idx, &filt);

	/* 启动 CAN 控制器 */
	can_start(dev);

	/* 读取初始状态 */
	enum can_state state;
	struct can_bus_err_cnt err = {0};
	can_get_state(dev, &state, &err);
	inst->current_state = state;

	/* 启动 TX 线程 */
	char thread_name[16];
	snprintk(thread_name, sizeof(thread_name), "can_tx_%s",
		 can_names[idx]);

	k_thread_create(&inst->tx_thread_data, inst->tx_thread_stack,
			K_THREAD_STACK_SIZEOF(can1_tx_stack),
			tx_thread_fn,
			(void *)(intptr_t)idx, NULL, NULL,
			5, 0, K_MSEC(100));
	k_thread_name_set(&inst->tx_thread_data, thread_name);

	inst->initialized = true;

	printk("[%s] 驱动已启动 (%s, TXerr=%u, RXerr=%u)\n",
	       can_names[idx], can_drv_state_str(state),
	       err.tx_err_cnt, err.rx_err_cnt);

	return 0;
}

int can_drv_init_all(void)
{
	int ret;
	int first_err = 0;

	for (enum can_dev_idx idx = 0; idx < CAN_DEV_COUNT; idx++) {
		ret = can_drv_init(idx);
		if (ret != 0 && first_err == 0) {
			first_err = ret;
		}
	}

	return first_err;
}
