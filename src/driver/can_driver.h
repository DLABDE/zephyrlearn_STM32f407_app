/**
 * @file    can_driver.h
 * @brief   多路 CAN 驱动模块 API
 *
 * 特性:
 *   - 非阻塞 TX: can_drv_send() 入队即返回, TX 线程异步发送
 *   - 阻塞 RX: can_drv_recv() 从队列取帧, 支持超时
 *   - 自动总线监控: 状态变化日志 + bus-off 自恢复
 */

#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

/** CAN 设备索引 */
enum can_dev_idx {
	CAN_DEV_CAN1,
	
	CAN_DEV_COUNT,
};

/**
 * @brief 初始化指定路 CAN 驱动, 启动 TX 线程, 注册状态回调.
 * @param idx CAN 设备索引
 * @return 0 成功, 负值 失败
 */
int can_drv_init(enum can_dev_idx idx);

/**
 * @brief 初始化所有 CAN 驱动.
 * @return 0 全部成功, 负值 第一个失败的设备错误码
 */
int can_drv_init_all(void);

/**
 * @brief 非阻塞发送 CAN 帧 (入 TX 队列).
 * @param idx     CAN 设备索引
 * @param frame   待发送的帧 (内部拷贝, 调用后可释放)
 * @param timeout 队列满时的等待时间 (建议 K_NO_WAIT 或 K_MSEC(100))
 * @return 0 成功, -EAGAIN 队列满超时, 其他 错误
 */
int can_drv_send(enum can_dev_idx idx, const struct can_frame *frame, k_timeout_t timeout);

/**
 * @brief 从 RX 队列取一帧 (阻塞).
 * @param idx     CAN 设备索引
 * @param frame   输出参数, 接收到的帧
 * @param timeout 等待超时 (K_FOREVER 永久等待)
 * @return 0 成功, -EAGAIN 超时
 */
int can_drv_recv(enum can_dev_idx idx, struct can_frame *frame, k_timeout_t timeout);

/**
 * @brief 获取指定路 CAN 控制器状态.
 * @param idx CAN 设备索引
 * @return 当前状态 (CAN_STATE_ERROR_ACTIVE 等)
 */
enum can_state can_drv_get_state(enum can_dev_idx idx);

/**
 * @brief 状态枚举转可读字符串.
 */
const char *can_drv_state_str(enum can_state s);

/**
 * @brief 获取 CAN 设备索引的名称字符串.
 */
const char *can_drv_name(enum can_dev_idx idx);

#endif /* CAN_DRIVER_H */
