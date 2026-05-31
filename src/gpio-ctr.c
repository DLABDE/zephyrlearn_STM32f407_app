#include <stdio.h>
#include <inttypes.h>          // 提供 PRIu32 等格式化宏，用于跨平台打印 uint32_t
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pinctrl.h>	//pinctrl_apply_state等函数

#include "gpio-ctr.h"

/* LED 设备树别名节点 */
#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)


static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct pwm_dt_spec pwm_led0 = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));

/*
 * PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3))
 *
 * Q: 这是什么意思？也没任何变量？还有其他写法吗？
 *
 * A: 这是一个宏展开后会"声明变量"的宏。它在编译期自动生成一个
 *    static const struct pinctrl_dev_config * 变量，名为:
 *
 *       PINCTRL_DT_DEV_CONFIG_DEFINE_DT_NODELABEL_pwm3
 *
 *    这个变量存储了 pwm3 节点的 pinctrl 配置数据，供
 *    pinctrl_apply_state() 使用，不需要你手动操作这个变量。
 *
 *    使用流程:
 *       PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3))           // [编译时] 生成配置变量
 *       PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3))   // [运行时] 获取配置指针
 *       pinctrl_apply_state(cfg, PINCTRL_STATE_DEFAULT)  // [运行时] 应用配置
 *
 *    其他写法:
 *      // 直接在 pinctrl_apply_state 时定义与获取, 但生成的变量名不易读
 *      PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3));
 *      ret = pinctrl_apply_state(
 *          PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3)),
 *          PINCTRL_STATE_DEFAULT);
 *
 *    为什么看起来反直觉:
 *      这是 Zephyr 设备树宏的"声明即定义"模式。宏展开后等于:
 *        static const struct pinctrl_dev_config *xxx = { ... };
 *      只是宏把变量名和内容都隐藏在展开后的代码中, 所以看起来像什么都没做。
 *      类似的"反直觉"宏还有: DEVICE_DT_DEFINE, PWM_DT_SPEC_GET 等。
 */
PINCTRL_DT_DEFINE(DT_NODELABEL(pwm3));

uint8_t led0_mode = 0;

int gpio_init_init(void)
{
    int ret;
    if (!gpio_is_ready_dt(&led)) {
		printk("ERR: led0 not ready\n");
		return 1;
	}
	if (!gpio_is_ready_dt(&led1)) {
		printk("ERR: led1 not ready\n");
		return 1;
	}
	if (!pwm_is_ready_dt(&pwm_led0)) {
		printk("ERR: pwm_led0 not ready\n");
		return 1;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("ERR: gpio configure led0 failed\n");
		return 1;
	}
	ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("ERR: gpio configure led1 failed\n");
		return 1;
	}
    return 0;
}

void  ledpin_toggle(uint8_t lednum)
{
    int ret = gpio_pin_toggle_dt(lednum == 0 ? &led : &led1);
    if (ret < 0)
    {
        printk("ERR: gpio toggle led %d failed\n", lednum);
    }
}

void led_ctr_ser(void)
{
    static uint8_t led0_statebak = 0;
    static int led_count = 0;
    static int pwm_step = 0;
    int ret;

    led_count++;

    if(led_count % 500 == 0)
    {
        /* 切换 LED 状态 */
        if(led0_mode==0)
        {
            ledpin_toggle(0);
        }
    }
    if(led_count % 100 == 0)
    {
        ledpin_toggle(1);
        if(led0_mode==1)
        {
            /* 呼吸灯效果:
                * pwm_step 从 0 递增到 100 再回到 0, 循环往复。
                * 占空比 = pwm_step% = pulse / period * 100
                * pulse = period * pwm_step / 100
                */
            pwm_step+=5;
            if(pwm_step > 100)
            {
                pwm_step = 0;
            }
            uint32_t pulse = pwm_led0.period * pwm_step / 100;
            ret = pwm_set_pulse_dt(&pwm_led0, pulse);
            if (ret < 0) {
                printk("ERR: pwm set pulse failed\n");
            }
        }
    }

    if(led0_mode != led0_statebak)
    {
        printk("led0_mode: %d\n", led0_mode);
        if(led0_mode==1)
        {
            /* 切换到 PWM 模式:
                * 将 PA6 从 GPIO 模式切换为 AF2 (TIM3_CH1) 模式,
                * 这样 PWM 信号才能输出到引脚。
                *
                * 【关键】pinctrl_apply_state 会应用 pwm3 节点的
                * 全部 pinctrl 配置, 即 tim3_ch1_pa6 和 tim3_ch2_pa7,
                * 这会把 PA6 和 PA7 都切换到 AF2 模式!
                * 所以必须在 apply 之后, 把 PA7 重新配回 GPIO 输出模式,
                * 否则 led1 (PA7) 将脱离 GPIO 控制。
                */
            gpio_pin_set_dt(&led, 0);
            ret = pinctrl_apply_state(
                PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(pwm3)),
                PINCTRL_STATE_DEFAULT
            );
            if (ret < 0) {
                printk("ERR: pinctrl apply failed: %d\n", ret);
            }
            /* 恢复 PA7 的 GPIO 控制 — 被 pinctrl_apply_state 误切到 AF 模式 */
            ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
            if (ret < 0) {
                printk("ERR: reconfigure led1 gpio failed\n");
            }
            pwm_step = 0;
        }
        else
        {
            /* 切换到 GPIO 模式:
                * 先停止 PWM 输出,
                * 然后调用 gpio_pin_configure 将引脚设回 GPIO 输出模式。
                */
            pwm_set_pulse_dt(&pwm_led0, 0);
            ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
            if (ret < 0) {
                printk("ERR: reconfigure led0 gpio failed\n");
            }
        }
        led0_statebak = led0_mode;
    }
}

/* =========================================================================
 * Input 子系统回调 — 多设备事件监听
 * =========================================================================
 *
 * 【核心概念】Input 伪设备(如 longpress)会创建自己的 struct device，
 *            它们消费原始事件后，以自己的设备身份重新上报新事件。
 *
 *   事件流:
 *     按键按下
 *       → gpio_keys 设备发出 INPUT_KEY_0 事件
 *       → longpress 伪设备监听到 INPUT_KEY_0
 *       → 持续按住超过 long-delay-ms
 *       → longpress 设备发出 INPUT_KEY_X 事件  ← 注意: 发出设备是 longpress!
 *       → 松开按键
 *       → longpress 设备发出 INPUT_KEY_X (value=0) 事件
 *
 *   因此: 如果回调只监听 gpio_keys 设备，就收不到 longpress 发出的事件！
 *         必须分别注册回调监听不同的设备。
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * 回调 1: 监听 gpio_keys 原始按键事件
 * -------------------------------------------------------------------------
 * 只接收 gpio_keys 设备发出的 INPUT_KEY_0 / INPUT_KEY_1 事件。
 * 这些是硬件按键的直接事件，不经过任何伪设备处理。
 * ------------------------------------------------------------------------- */
static void gpio_keys_input_cb(struct input_event *evt, void *user_data)
{
	if (evt->sync == 0) {//这是一个中间事件，还没传完，直接跳过
		//1 这组事件已完整，可以处理——这是一种 事件同步机制
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_0:
		printk("[GPIO-KEYS] Button 0 (PE4) %s\n",
		       evt->value ? "pressed" : "released");
		break;
	case INPUT_KEY_1:
		printk("[GPIO-KEYS] Button 1 (PE3) %s\n",
		       evt->value ? "pressed" : "released");
		break;
	default:
		printk("[GPIO-KEYS] Unknown code=%d %s\n",
		       evt->code,
		       evt->value ? "pressed" : "released");
		break;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(bt0))), gpio_keys_input_cb, NULL);

/* -------------------------------------------------------------------------
 * 回调 2: 监听 longpress 伪设备事件
 * -------------------------------------------------------------------------
 * longpress 伪设备是独立的 struct device，它消费 gpio_keys 的原始事件后，
 * 以自己的设备身份重新上报转换后的事件:
 *   - 长按: 上报 long-codes (INPUT_KEY_X / INPUT_KEY_Y)
 *   - 短按: 上报 short-codes (INPUT_KEY_A / INPUT_KEY_B)
 *
 * DT_NODELABEL(longpress) 通过 DTS 中的标签引用 longpress 节点。
 * 也可以用 DT_ALIAS()，但需要先在 aliases 中定义别名。
 * ------------------------------------------------------------------------- */
static void longpress_input_cb(struct input_event *evt, void *user_data)
{
	if (evt->sync == 0) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_X:
		printk("[LONGPRESS] Button 0 long press %s\n",
		       evt->value ? "started" : "ended");
		led0_mode = 1;
		break;
	case INPUT_KEY_Y:
		printk("[LONGPRESS] Button 1 long press %s\n",
		       evt->value ? "started" : "ended");
		led0_mode = 0;
		break;
	case INPUT_KEY_A:
		printk("[LONGPRESS] Button 0 short press\n");
		break;
	case INPUT_KEY_B:
		printk("[LONGPRESS] Button 1 short press\n");
		break;
	default:
		printk("[LONGPRESS] Unknown code=%d %s\n",
		       evt->code,
		       evt->value ? "pressed" : "released");
		break;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(longpress)), longpress_input_cb, NULL);
