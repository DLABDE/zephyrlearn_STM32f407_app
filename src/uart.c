#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include "uart.h"

const struct device *uart3 = DEVICE_DT_GET(DT_NODELABEL(usart3));
const struct device *uart4 = DEVICE_DT_GET(DT_NODELABEL(uart4));

/*
 * RX 双缓冲 (ping-pong)
 *
 * 工作原理:
 *   1. uart_rx_enable(buf0)  → DMA 开始往 buf0 写数据
 *   2. 驱动立即请求下一块缓冲区 → UART_RX_BUF_REQUEST 事件
 *   3. 回调中调用 uart_rx_buf_rsp(buf1) → 提供 buf1 作为下一块
 *   4. 空闲中断触发 → UART_RX_RDY 事件 → buf0 存满一帧
 *   5. 驱动自动切换到 buf1 继续接收, buf0 被释放
 *   6. 驱动再次请求下一块 → 提供 buf0
 *   7. 循环往复, 永不丢数据
 *
 * 【为什么需要双缓冲而不是一个 FIFO？】
 *
 *   你之前用的 FIFO 缓冲区 + 中断逐字节写入，那是在 CPU（中断）搬运数据的模式下。
 *   但现在 DMA 模式下，数据搬运者是 DMA 硬件，不是 CPU。
 *
 *   DMA 的工作方式是：你给它一个"起始地址 + 长度"，它就往这个地址范围
 *   持续写入数据，直到写满为止。DMA 不知道什么是"队列"、什么是"环形缓冲区"，
 *   它只会线性地往你给的地址写。
 *
 *   问题来了：当 DMA 写满 buf_a 后，它需要立刻有一个新地方继续写，
 *   否则新来的 UART 数据就丢了（DMA 停下来等 = 丢字节）。
 *
 *   双缓冲的解决方案：
 *     - DMA 正在写 buf_a 时，我们提前准备好 buf_b
 *     - buf_a 写满 → DMA 无缝切换到 buf_b 继续写 → 零间隔，不丢数据
 *     - 同时 buf_a 被释放给 CPU 处理（复制到 reasm_buf）
 *     - 下次 buf_b 写满 → 切回 buf_a → 循环
 *
 *   对比 FIFO 方案：
 *     FIFO（环形缓冲区）需要"写指针追读指针"，DMA 硬件不支持这种模式。
 *     DMA 只能做"线性写入一块连续内存"，写满就停。
 *     所以 Zephyr 的 Async API 用 ping-pong 双缓冲来模拟"持续接收"。
 *
 *   一句话总结：FIFO 是 CPU 模式下的方案，双缓冲是 DMA 模式下的方案。
 *   两者解决同一个问题（接收时不丢数据），但适配不同的数据搬运者。
 */
static uint8_t rx_buf_a[UART_RX_BUF_SIZE];
static uint8_t rx_buf_b[UART_RX_BUF_SIZE];
static uint8_t *rx_active_buf = rx_buf_a;

/*
 * K_MSGQ_DEFINE: 定义一个内核消息队列
 *
 * 参数:
 *   uart_rx_msgq  — 队列名称
 *   sizeof(struct uart_rx_frame) — 每条消息的大小（一帧数据）
 *   8             — 队列容量，最多存 8 条消息（8 帧）
 *   4             — 消息内容的对齐字节数（struct uart_rx_frame 按 4 字节对齐）
 *
 * 工作方式: 生产者-消费者模式
 *   生产者: ISR/回调中 k_msgq_put() 放入一帧
 *   消费者: 线程中 k_msgq_get() 取出一帧
 *   队列满时 put 丢弃（K_NO_WAIT），队列空时 get 阻塞或返回错误
 *
 * 为什么用消息队列而不是全局变量？
 *   消息队列是内核提供的线程安全 IPC，内部有自旋锁保护。
 *   ISR 写入 + 线程读取，不需要手动加锁，不会竞争。
 */
K_MSGQ_DEFINE(uart_rx_msgq, sizeof(struct uart_rx_frame), 8, 4);

/*
 * 帧重组缓冲区 (Reassembly Buffer)
 *
 * 为什么需要它？
 *
 *   DMA 双缓冲有一个不可避免的副作用：当 buf_a 写满时，DMA 立即切换到 buf_b。
 *   如果此时正好有一帧数据正在接收中（比如帧的前 12 字节在 buf_a 末尾，
 *   后 6 字节在 buf_b 开头），驱动会产生两个 RX_RDY 事件：
 *     RX_RDY #1: buf_a 的 [offset..end) → "Hello, UAR"
 *     RX_RDY #2: buf_b 的 [0..6)        → "T! 5\r\n"
 *
 *   这就是一个逻辑帧被 DMA 缓冲区边界截断成了两个"碎片"。
 *   reasm_buf 的作用就是把这些碎片重新拼回完整的一帧。
 *
 * 工作方式:
 *   1. 每次 RX_RDY → 把数据追加到 reasm_buf 末尾 → 重启定时器
 *   2. 定时器到期（说明线路空闲，帧结束）→ 把 reasm_buf 的内容作为完整帧提交
 *   3. 如果帧没被截断，也是一样的流程：RX_RDY → 追加 → 定时器到期 → 提交
 *
 *   定时器的作用: 区分"帧内碎片"和"帧间间隔"
 *     - 帧内碎片：两个 RX_RDY 之间间隔很短（DMA 切换瞬间），定时器被重启
 *     - 帧间间隔：两个 RX_RDY 之间间隔较长（真正的空闲），定时器到期 → 提交
 */
static uint8_t rx_reasm_buf[UART_RX_BUF_SIZE];
static size_t rx_reasm_len = 0;

/*
 * k_work_delayable: 内核延迟工作项
 *
 * 本质上是一个"延迟执行的函数"。
 * 你告诉内核："UART_FRAME_TIMEOUT_US 微秒后，帮我调用 rx_reasm_timeout_fn"
 * 每次调用 k_work_reschedule() 会重新计时（覆盖之前的定时）
 *
 * 这就是重组的核心机制：
 *   每次 RX_RDY 来 → reschedule 定时器 → 如果很快又来 RX_RDY → 重新计时
 *   → 直到线路真正空闲 → 定时器到期 → 提交完整帧
 */
static struct k_work_delayable rx_reasm_work;

/*
 * rx_reasm_submit: 将重组缓冲区的内容作为完整帧提交到消息队列
 *
 * 调用时机:
 *   1. 定时器到期（rx_reasm_timeout_fn 中调用）→ 线路空闲，帧结束
 *   2. RX_DISABLED 事件中调用 → 接收被禁用前，把残余数据提交
 */
static void rx_reasm_submit(void)
{
    if (rx_reasm_len == 0) {
        return;
    }

    struct uart_rx_frame frame;
    frame.len = rx_reasm_len;
    if (frame.len > UART_RX_BUF_SIZE) {
        frame.len = UART_RX_BUF_SIZE;
    }
    memcpy(frame.data, rx_reasm_buf, frame.len);
    rx_reasm_len = 0;
    k_msgq_put(&uart_rx_msgq, &frame, K_NO_WAIT);
}

/*
 * rx_reasm_timeout_fn: 重组定时器到期回调
 *
 * 当线路空闲超过 UART_FRAME_TIMEOUT_US 后，内核在 system workqueue 线程中
 * 调用此函数。此时说明一帧数据已经完整接收（没有新的 RX_RDY 来了），
 * 把重组缓冲区的内容提交到消息队列。
 *
 * 注意: 此函数运行在线程上下文（workqueue），不是 ISR，但为了防止与
 * ISR 中的 RX_RDY 回调竞争 rx_reasm_buf，仍需要 irq_lock() 保护。
 */
static void rx_reasm_timeout_fn(struct k_work *work)
{
    ARG_UNUSED(work); /* 告诉编译器 work 参数未使用，避免 unused-parameter 警告 */

    unsigned int key = irq_lock(); /* 关闭中断，防止 ISR 修改 rx_reasm_buf */
    rx_reasm_submit();
    irq_unlock(key);               /* 恢复中断 */
}

static void uart_async_callback(const struct device *dev,
                                struct uart_event *evt,
                                void *user_data)
{
    switch (evt->type) {

    case UART_TX_DONE:
        /* DMA 完成了 TX 数据搬运，发送结束 */
        break;

    case UART_TX_ABORTED:
        printk("UART CB: TX_ABORTED\n");
        break;

    /*
     * UART_RX_RDY: 接收到一段数据，可以读取了
     *
     * 触发时机:
     *   1. IDLE 空闲中断 → 驱动 flush DMA → 产生 RX_RDY（最常见）
     *   2. DMA 缓冲区写满 → TC 中断 → 产生 RX_RDY（缓冲区满时）
     *
     * 注意: 这不是"收到完整一帧"的意思！
     *   RX_RDY 只表示"DMA 中有一段新数据可以读了"。
     *   一个逻辑帧可能产生 1 个或 2 个 RX_RDY（被 DMA 缓冲区边界截断时）。
     *   所以需要 reasm_buf 把碎片拼回来。
     *
     * evt->data.rx.buf    — 数据所在的 DMA 缓冲区指针
     * evt->data.rx.offset — 数据在缓冲区中的起始偏移
     * evt->data.rx.len    — 数据长度
     */
    case UART_RX_RDY: {
        size_t chunk_len = evt->data.rx.len;

        /* 溢出保护: 如果追加后超过 reasm_buf 容量，丢弃已有数据重新开始 */
        if (rx_reasm_len + chunk_len > UART_RX_BUF_SIZE) {
            rx_reasm_len = 0;
        }

        /* 把这段数据追加到重组缓冲区末尾 */
        memcpy(&rx_reasm_buf[rx_reasm_len],
               &evt->data.rx.buf[evt->data.rx.offset],
               chunk_len);
        rx_reasm_len += chunk_len;

        /*
         * k_work_reschedule: 重新调度延迟工作项
         *
         * 效果: 从此刻起，等 UART_FRAME_TIMEOUT_US 微秒后执行 rx_reasm_timeout_fn
         * 如果之前已经调度过（定时器还在跑），会取消之前的并重新计时
         *
         * 这就是重组的关键:
         *   - 帧内碎片（DMA 截断）: 两个 RX_RDY 间隔极短，第二个 reschedule
         *     会重置定时器，第一个不会触发提交
         *   - 帧间空闲: 最后一个 RX_RDY 之后，定时器到期 → 提交完整帧
         */
        k_work_reschedule(&rx_reasm_work, K_USEC(UART_FRAME_TIMEOUT_US));
        break;
    }

    /*
     * UART_RX_BUF_REQUEST: 驱动请求提供下一个接收缓冲区
     *
     * 什么时候触发？
     *   uart_rx_enable() 调用后，驱动立即触发此事件，要求你提供"备用缓冲区"。
     *   这样当当前缓冲区写满时，DMA 可以无缝切换到备用缓冲区继续接收。
     *
     * 为什么需要切换缓冲区？
     *   因为 DMA 是线性写入的，写满一个缓冲区后必须有一个新缓冲区才能继续。
     *   如果不提供（不调用 uart_rx_buf_rsp），DMA 写满后就停了，新数据丢失。
     *   切换 = ping-pong：a→b→a→b→...，始终保持一个在写、一个备用。
     *
     * uart_rx_buf_rsp: 告诉驱动"下一个缓冲区在这里"
     *   参数: 设备、缓冲区指针、缓冲区大小
     *   调用后，驱动把缓冲区记录为 rx_next_buffer，等当前缓冲区写满时自动切换
     */
    case UART_RX_BUF_REQUEST:
        if (rx_active_buf == rx_buf_a) {
            rx_active_buf = rx_buf_b;
        } else {
            rx_active_buf = rx_buf_a;
        }
        uart_rx_buf_rsp(dev, rx_active_buf, UART_RX_BUF_SIZE);
        break;

    /*
     * UART_RX_BUF_RELEASED: 驱动释放了一个缓冲区
     *
     * 含义: 之前用于接收的缓冲区已经不再被 DMA 使用了，你可以安全地
     * 读取其中的数据或重新使用它。
     *
     * 触发时机: 当前缓冲区写满 → DMA 切换到下一个缓冲区 → 释放旧缓冲区
     * 在我们的代码中，RX_RDY 已经把数据复制到了 reasm_buf，
     * 所以这个事件只需要知道就行，不需要做额外处理。
     */
    case UART_RX_BUF_RELEASED:
        break;

    /*
     * UART_RX_DISABLED: 接收已停止
     *
     * 触发时机:
     *   1. 没有提供备用缓冲区（没调用 uart_rx_buf_rsp），当前缓冲区写满后
     *      驱动自动停止接收并触发此事件
     *   2. 显式调用 uart_rx_disable()
     *
     * 处理:
     *   1. 取消重组定时器（避免定时器回调访问已释放的资源）
     *   2. 提交重组缓冲区中的残余数据（可能还有最后一帧没提交）
     *   3. 重新启动接收（uart_rx_enable），确保持续接收
     */
    case UART_RX_DISABLED: {
        /*
         * k_work_cancel_delayable: 取消一个延迟工作项
         * 如果定时器还在倒计时，取消它，回调不会被执行
         * 返回值被 (void) 忽略，因为即使取消失败（定时器已在执行）也无妨
         */
        (void)k_work_cancel_delayable(&rx_reasm_work);

        unsigned int key = irq_lock();
        rx_reasm_submit();
        irq_unlock(key);

        /* 重新启动接收，用 buf_a 作为第一个缓冲区 */
        uart_rx_enable(dev, rx_buf_a, UART_RX_BUF_SIZE, 0);
        break;
    }

    case UART_RX_STOPPED:
        printk("UART CB: RX_STOPPED\n");
        break;

    default:
        printk("UART CB: unknown event %d\n", evt->type);
        break;
    }
}

int uart_init(void)
{
    if (!device_is_ready(uart3)) {
        printk("UART3 device not ready\n");
        return -1;
    }
    if (!device_is_ready(uart4)) {
        printk("UART4 device not ready\n");
        return -1;
    }

    /*
     * k_work_init_delayable: 初始化延迟工作项
     *
     * 把 rx_reasm_work 和 rx_reasm_timeout_fn 绑定起来。
     * 之后调用 k_work_reschedule(&rx_reasm_work, timeout) 就会在
     * timeout 后执行 rx_reasm_timeout_fn。
     */
    k_work_init_delayable(&rx_reasm_work, rx_reasm_timeout_fn);

    uart_callback_set(uart3, uart_async_callback, NULL);
    uart_callback_set(uart4, uart_async_callback, NULL);

    int ret = uart_rx_enable(uart4, rx_buf_a, UART_RX_BUF_SIZE, 0);
    printk("UART init: uart_rx_enable ret=%d\n", ret);

    return 0;
}

int uart3_tx_async(const uint8_t *data, size_t len)
{
    return uart_tx(uart3, data, len, SYS_FOREVER_US);
}

int uart4_rx_frame_get(struct uart_rx_frame *frame, k_timeout_t timeout)
{
    return k_msgq_get(&uart_rx_msgq, frame, timeout);
}
