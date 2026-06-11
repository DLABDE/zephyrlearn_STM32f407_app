#ifndef __UART_H__
#define __UART_H__

#include <zephyr/kernel.h>

#define UART_RX_BUF_SIZE 256

#define UART_FRAME_TIMEOUT_US 2000

/*
 * 接收到的一帧数据
 *
 * 帧边界由 UART 空闲中断 (IDLE line detection) 自动分割:
 *   当 RX 线上连续空闲超过 1 个字符时间, 驱动产生 UART_RX_RDY 事件,
 *   此时已收到的全部字节作为一帧, 存入此结构体.
 */
struct uart_rx_frame {
    uint8_t data[UART_RX_BUF_SIZE];
    size_t len;
};

int uart_init(void);

/*
 * 异步发送 (非阻塞)
 *
 * 调用 uart_tx() 将数据交给 DMA 后台发送, 立即返回.
 * 发送完成后通过回调通知 (UART_TX_DONE 事件).
 *
 * 这是线程 A 调用此函数不会被阻塞的核心原因.
 */
int uart3_tx_async(const uint8_t *data, size_t len);

/*
 * 获取一帧接收数据 (阻塞等待)
 *
 * 从消息队列中取出已接收的一帧.
 * 内部由 ISR → DMA 完成回调 → k_msgq_put 推入队列.
 * 线程 C 调用此函数, 有数据时返回, 无数据时阻塞.
 */
int uart4_rx_frame_get(struct uart_rx_frame *frame, k_timeout_t timeout);

#endif
