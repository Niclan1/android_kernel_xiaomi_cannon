// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8622 PWM haptic driver for the MT6853 (Redmi Note 9T).
 *
 * The AW8622 is an LRA H-bridge that follows a PWM input while its HWEN
 * pin is high. The stock 4.14 driver (drivers/misc/mediatek/aw8622_haptic)
 * drives a "long shock" as a 50 % square wave at the LRA resonance
 * frequency (DT "center_freq"); this driver reproduces that mode with the
 * generic PWM API and exposes the LED-class "vibrator" interface used by
 * the MediaTek vibrator HAL (state, duration, activate).
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define AW8622_DEFAULT_FREQ_HZ	208
#define AW8622_MAX_DURATION_MS	60000

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Bind the AW8622 haptic driver (0 = leave hardware alone)");

struct aw8622 {
	struct device *dev;
	struct pwm_device *pwm;
	struct gpio_desc *hwen;
	struct pinctrl *pinctrl;
	struct pinctrl_state *st_idle;
	struct pinctrl_state *st_pwm;
	struct led_classdev cdev;
	struct hrtimer timer;
	struct work_struct stop_work;
	struct mutex control_lock;	/* serialises timer/work cancellation */
	struct mutex lock;		/* serialises start/stop */
	u32 freq_hz;
	u32 duration_ms;
	u32 state;
	bool running;
};

static void aw8622_stop_locked(struct aw8622 *aw)
{
	struct pwm_state st;

	if (!aw->running)
		return;

	gpiod_set_value_cansleep(aw->hwen, 0);
	pwm_get_state(aw->pwm, &st);
	st.enabled = false;
	pwm_apply_might_sleep(aw->pwm, &st);
	if (aw->st_idle)
		pinctrl_select_state(aw->pinctrl, aw->st_idle);
	WRITE_ONCE(aw->running, false);
}

static int aw8622_start_locked(struct aw8622 *aw, u32 ms)
{
	struct pwm_state st;
	int ret;

	if (aw->st_pwm) {
		ret = pinctrl_select_state(aw->pinctrl, aw->st_pwm);
		if (ret)
			return ret;
	}

	pwm_init_state(aw->pwm, &st);
	st.period = DIV_ROUND_CLOSEST(NSEC_PER_SEC, aw->freq_hz);
	st.duty_cycle = st.period / 2;
	st.enabled = true;
	ret = pwm_apply_might_sleep(aw->pwm, &st);
	if (ret) {
		dev_err(aw->dev, "pwm apply failed: %d\n", ret);
		if (aw->st_idle)
			pinctrl_select_state(aw->pinctrl, aw->st_idle);
		return ret;
	}

	/* Only enable the bridge once the PWM input is toggling. */
	gpiod_set_value_cansleep(aw->hwen, 1);
	WRITE_ONCE(aw->running, true);
	hrtimer_start(&aw->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	return 0;
}

static void aw8622_stop_work(struct work_struct *work)
{
	struct aw8622 *aw = container_of(work, struct aw8622, stop_work);

	mutex_lock(&aw->lock);
	aw8622_stop_locked(aw);
	mutex_unlock(&aw->lock);
}

static enum hrtimer_restart aw8622_timer_fn(struct hrtimer *t)
{
	struct aw8622 *aw = container_of(t, struct aw8622, timer);

	schedule_work(&aw->stop_work);
	return HRTIMER_NORESTART;
}

static int aw8622_set_active(struct aw8622 *aw, bool active)
{
	int ret = 0;

	mutex_lock(&aw->control_lock);
	hrtimer_cancel(&aw->timer);
	cancel_work_sync(&aw->stop_work);
	mutex_lock(&aw->lock);
	aw8622_stop_locked(aw);
	if (active && aw->duration_ms)
		ret = aw8622_start_locked(aw, aw->duration_ms);
	mutex_unlock(&aw->lock);
	mutex_unlock(&aw->control_lock);

	return ret;
}

static struct aw8622 *cdev_to_aw(struct device *dev)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);

	return container_of(cdev, struct aw8622, cdev);
}

static ssize_t activate_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(cdev_to_aw(dev)->running));
}

static ssize_t activate_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct aw8622 *aw = cdev_to_aw(dev);
	u32 val;
	int ret;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	ret = aw8622_set_active(aw, val);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(activate);

static ssize_t duration_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(cdev_to_aw(dev)->duration_ms));
}

static ssize_t duration_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct aw8622 *aw = cdev_to_aw(dev);
	u32 val;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	mutex_lock(&aw->lock);
	WRITE_ONCE(aw->duration_ms, min_t(u32, val, AW8622_MAX_DURATION_MS));
	mutex_unlock(&aw->lock);
	return count;
}
static DEVICE_ATTR_RW(duration);

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(cdev_to_aw(dev)->state));
}

static ssize_t state_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	u32 val;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	WRITE_ONCE(cdev_to_aw(dev)->state, !!val);
	return count;
}
static DEVICE_ATTR_RW(state);

static struct attribute *aw8622_attrs[] = {
	&dev_attr_activate.attr,
	&dev_attr_duration.attr,
	&dev_attr_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(aw8622);

static int aw8622_brightness_set(struct led_classdev *cdev,
				 enum led_brightness value)
{
	struct aw8622 *aw = container_of(cdev, struct aw8622, cdev);

	if (value == LED_OFF)
		return aw8622_set_active(aw, false);
	return 0;
}

static int aw8622_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aw8622 *aw;
	int ret;

	if (!enable)
		return -ENODEV;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	aw->dev = dev;
	mutex_init(&aw->control_lock);
	mutex_init(&aw->lock);
	INIT_WORK(&aw->stop_work, aw8622_stop_work);
	hrtimer_setup(&aw->timer, aw8622_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	/* Keep the bridge disabled before anything else happens. */
	aw->hwen = devm_gpiod_get(dev, "hwen", GPIOD_OUT_LOW);
	if (IS_ERR(aw->hwen))
		return dev_err_probe(dev, PTR_ERR(aw->hwen), "no hwen gpio\n");

	aw->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(aw->pwm))
		return dev_err_probe(dev, PTR_ERR(aw->pwm), "no pwm\n");

	if (device_property_read_u32(dev, "center_freq", &aw->freq_hz) ||
	    aw->freq_hz < 50 || aw->freq_hz > 1000)
		aw->freq_hz = AW8622_DEFAULT_FREQ_HZ;

	aw->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(aw->pinctrl)) {
		aw->pinctrl = NULL;
	} else {
		aw->st_idle = pinctrl_lookup_state(aw->pinctrl,
						   "haptic_gpio_aw8622_default");
		aw->st_pwm = pinctrl_lookup_state(aw->pinctrl,
						  "haptic_gpio_aw8622_set");
		if (IS_ERR(aw->st_idle) || IS_ERR(aw->st_pwm)) {
			aw->st_idle = NULL;
			aw->st_pwm = NULL;
		} else {
			pinctrl_select_state(aw->pinctrl, aw->st_idle);
		}
	}

	aw->cdev.name = "vibrator";
	aw->cdev.max_brightness = 1;
	aw->cdev.brightness_set_blocking = aw8622_brightness_set;
	aw->cdev.groups = aw8622_groups;
	aw->cdev.flags = LED_CORE_SUSPENDRESUME;

	platform_set_drvdata(pdev, aw);
	ret = devm_led_classdev_register(dev, &aw->cdev);
	if (ret)
		return dev_err_probe(dev, ret, "led register failed\n");

	dev_info(dev, "AW8622 haptic ready, %u Hz\n", aw->freq_hz);
	return 0;
}

static void aw8622_remove(struct platform_device *pdev)
{
	struct aw8622 *aw = platform_get_drvdata(pdev);

	devm_led_classdev_unregister(aw->dev, &aw->cdev);
	aw8622_set_active(aw, false);
}

static void aw8622_shutdown(struct platform_device *pdev)
{
	aw8622_set_active(platform_get_drvdata(pdev), false);
}

static const struct of_device_id aw8622_of_match[] = {
	{ .compatible = "awinic,aw8622" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8622_of_match);

static struct platform_driver aw8622_driver = {
	.probe = aw8622_probe,
	.remove = aw8622_remove,
	.shutdown = aw8622_shutdown,
	.driver = {
		.name = "aw8622-vibrator",
		.of_match_table = aw8622_of_match,
	},
};
module_platform_driver(aw8622_driver);

MODULE_DESCRIPTION("Awinic AW8622 PWM haptic driver");
MODULE_LICENSE("GPL");
