// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/flashlight compat for the stock MT6853 camera HAL (StrobeDrvFlashlight
 * in libcam.hal3a.v3.platform.so), backed by the mainline MT6360 LED-class
 * flash LED (drivers/leds/flash/leds-mt6360.c).
 *
 * Implements the 4.14 flashlight-core ABI (magic 'S', struct
 * flashlight_user_arg) with the semantics of flashlights-mt6360.c in its
 * SINGLE_FLASH_DUAL_LED configuration: one LED on FLED1, 46 duty levels,
 * levels 0..11 are torch, the rest strobe. Type 1 (main) is the only
 * device; colour temperature 2 is accepted but never drives the LED.
 * Currents are clamped by the LED node (led-max-microamp, flash-max-*).
 *
 * Off unless booted with mtk_cam.hw=1 mtk_cam.flash=1.
 */

#include <linux/fs.h>
#include <linux/led-class-flash.h>
#include <linux/leds.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "../mtk_cam.h"

/* ---- 4.14 ABI (flashlight.h / flashlight-core.h), keep exact ---- */

struct flashlight_user_arg {
	int type_id;
	int ct_id;
	int arg;
};

#define FLASHLIGHT_MAGIC 'S'
#define FLASHLIGHTIOC_X_SET_DRIVER	_IOWR(FLASHLIGHT_MAGIC, 30, int)
#define FLASH_IOC_SET_TIME_OUT_TIME_MS	_IOR(FLASHLIGHT_MAGIC, 100, int)
#define FLASH_IOC_SET_DUTY		_IOR(FLASHLIGHT_MAGIC, 110, int)
#define FLASH_IOC_SET_ONOFF		_IOR(FLASHLIGHT_MAGIC, 115, int)
#define FLASH_IOC_GET_MAIN_PART_ID	_IOR(FLASHLIGHT_MAGIC, 155, int)
#define FLASH_IOC_GET_SUB_PART_ID	_IOR(FLASHLIGHT_MAGIC, 160, int)
#define FLASH_IOC_GET_MAIN2_PART_ID	_IOR(FLASHLIGHT_MAGIC, 165, int)
#define FLASH_IOC_GET_PART_ID		_IOR(FLASHLIGHT_MAGIC, 166, int)
#define FLASH_IOC_LOW_POWER_DETECT_START _IOR(FLASHLIGHT_MAGIC, 175, int)
#define FLASH_IOC_LOW_POWER_DETECT_END	_IOR(FLASHLIGHT_MAGIC, 180, int)
#define FLASH_IOC_IS_LOW_POWER		_IOR(FLASHLIGHT_MAGIC, 182, int)
#define FLASH_IOC_GET_PROTOCOL_VERSION	_IOR(FLASHLIGHT_MAGIC, 190, int)
#define FLASH_IOC_IS_CHARGER_READY	_IOR(FLASHLIGHT_MAGIC, 210, int)
#define FLASH_IOC_SET_SCENARIO		_IOWR(FLASHLIGHT_MAGIC, 215, int)
#define FLASH_IOC_IS_HARDWARE_READY	_IOR(FLASHLIGHT_MAGIC, 220, int)
#define FLASH_IOC_GET_DUTY_NUMBER	_IOWR(FLASHLIGHT_MAGIC, 225, int)
#define FLASH_IOC_GET_MAX_TORCH_DUTY	_IOWR(FLASHLIGHT_MAGIC, 230, int)
#define FLASH_IOC_GET_DUTY_CURRENT	_IOWR(FLASHLIGHT_MAGIC, 235, int)
#define FLASH_IOC_GET_HW_TIMEOUT	_IOWR(FLASHLIGHT_MAGIC, 240, int)
#define FLASH_IOC_GET_HW_FAULT		_IOR(FLASHLIGHT_MAGIC, 250, int)
#define FLASH_IOC_GET_HW_FAULT2		_IOR(FLASHLIGHT_MAGIC, 251, int)

#define FLASHLIGHT_PROTOCOL_VERSION	2
#define FLASHLIGHT_CHARGER_READY	1

/* flashlights-mt6360.c, SINGLE_FLASH_DUAL_LED (the last entry is 0 there) */
#define FL_LEVEL_NUM		46
#define FL_LEVEL_TORCH		12

static const int fl_current_ma[FL_LEVEL_NUM] = {
	  25,   50,   75,  100,  125,  150,  175,  200,  225,  250,
	 275,  300,  325,  350,  375,  400,  425,  450,  475,  500,
	 525,  550,  575,  600,  625,  650,  675,  700,  725,  750,
	 800,  850,  900,  950, 1000, 1050, 1100, 1150, 1200, 1250,
	1300, 1350, 1400, 1450, 1500
};

/* torch brightness steps of leds-mt6360 (MT6360_ITORCH_*) */
#define FL_TORCH_MIN_UA		25000
#define FL_TORCH_STEP_UA	12500

struct fl_compat {
	struct device *dev;
	struct led_classdev *cdev;
	struct led_classdev_flash *fled;
	struct miscdevice misc;
	struct delayed_work off_work;
	struct mutex lock;		/* LED state and the fields below */
	int users;
	int level;
	unsigned int timeout_ms;
	bool on;
};

static int fl_level_clamp(int level)
{
	return clamp(level, 0, FL_LEVEL_NUM - 1);
}

static void fl_off_locked(struct fl_compat *fl)
{
	bool strobe = false;

	if (!led_get_flash_strobe(fl->fled, &strobe) && strobe)
		led_set_flash_strobe(fl->fled, false);
	led_set_brightness_sync(fl->cdev, LED_OFF);
	fl->on = false;
}

static int fl_on_locked(struct fl_compat *fl)
{
	struct led_flash_setting *t = &fl->fled->timeout;
	u32 ua = max(fl_current_ma[fl->level], 25) * 1000;
	u32 tmo;
	int ret;

	fl_off_locked(fl);

	if (fl->level < FL_LEVEL_TORCH) {
		u32 b = (ua - FL_TORCH_MIN_UA) / FL_TORCH_STEP_UA + 1;

		b = clamp_t(u32, b, 1, fl->cdev->max_brightness);
		ret = led_set_brightness_sync(fl->cdev, b);
	} else {
		/* led_set_flash_brightness/timeout clamp to the DT limits */
		ret = led_set_flash_brightness(fl->fled, ua);
		if (ret)
			return ret;
		tmo = fl->timeout_ms ? fl->timeout_ms * 1000 : t->max;
		ret = led_set_flash_timeout(fl->fled, tmo);
		if (ret)
			return ret;
		ret = led_set_flash_strobe(fl->fled, true);
	}
	if (ret)
		return ret;

	fl->on = true;
	if (fl->timeout_ms)
		mod_delayed_work(system_wq, &fl->off_work,
				 msecs_to_jiffies(fl->timeout_ms));
	return 0;
}

static void fl_off_work(struct work_struct *work)
{
	struct fl_compat *fl = container_of(to_delayed_work(work),
					    struct fl_compat, off_work);

	mutex_lock(&fl->lock);
	fl_off_locked(fl);
	mutex_unlock(&fl->lock);
}

static struct fl_compat *fl_from_file(struct file *file)
{
	return container_of(file->private_data, struct fl_compat, misc);
}

static int fl_open(struct inode *inode, struct file *file)
{
	struct fl_compat *fl = fl_from_file(file);

	mutex_lock(&fl->lock);
	fl->users++;
	mutex_unlock(&fl->lock);
	return 0;
}

static int fl_release(struct inode *inode, struct file *file)
{
	struct fl_compat *fl = fl_from_file(file);

	mutex_lock(&fl->lock);
	if (!--fl->users) {
		/* camera process closed or died: never leave the LED on */
		cancel_delayed_work(&fl->off_work);
		fl_off_locked(fl);
		fl->timeout_ms = 0;
	}
	mutex_unlock(&fl->lock);
	return 0;
}

static long fl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fl_compat *fl = fl_from_file(file);
	void __user *uarg = (void __user *)arg;
	struct flashlight_user_arg ua;
	bool drive, copy = false;
	u32 fault;
	long ret = 0;

	if (copy_from_user(&ua, uarg, sizeof(ua)))
		return -EFAULT;

	/*
	 * flashlight-core looks devices up by (type, ct). Stock DT only has
	 * type 0 (rear, type_id 1); the HAL probes type_id 2 (front) as well
	 * and stock also returned -EINVAL for it.
	 */
	if (ua.type_id != 1 || ua.ct_id < 1 || ua.ct_id > 2) {
		pr_debug("flashlight-compat: no device (%d,%d)\n",
				    ua.type_id, ua.ct_id);
		return -EINVAL;
	}
	drive = ua.ct_id == 1;

	mutex_lock(&fl->lock);
	switch (cmd) {
	case FLASH_IOC_GET_PROTOCOL_VERSION:
		ret = FLASHLIGHT_PROTOCOL_VERSION;
		break;
	case FLASH_IOC_IS_LOW_POWER:
		ua.arg = 0;
		copy = true;
		break;
	case FLASH_IOC_LOW_POWER_DETECT_START:
	case FLASH_IOC_LOW_POWER_DETECT_END:
	case FLASHLIGHTIOC_X_SET_DRIVER:
	case FLASH_IOC_SET_SCENARIO:
		break;
	case FLASH_IOC_IS_CHARGER_READY:
		ua.arg = FLASHLIGHT_CHARGER_READY;
		copy = true;
		break;
	case FLASH_IOC_IS_HARDWARE_READY:
	case FLASH_IOC_GET_PART_ID:
	case FLASH_IOC_GET_MAIN_PART_ID:
	case FLASH_IOC_GET_SUB_PART_ID:
	case FLASH_IOC_GET_MAIN2_PART_ID:
		ua.arg = 1;
		copy = true;
		break;
	case FLASH_IOC_SET_DUTY:
		if (drive)
			fl->level = fl_level_clamp(ua.arg);
		break;
	case FLASH_IOC_SET_TIME_OUT_TIME_MS:
		if (drive)
			fl->timeout_ms = ua.arg > 0 ? ua.arg : 0;
		break;
	case FLASH_IOC_SET_ONOFF:
		if (!drive)
			break;
		cancel_delayed_work(&fl->off_work);
		if (ua.arg)
			ret = fl_on_locked(fl);
		else
			fl_off_locked(fl);
		if (ret)
			dev_warn_ratelimited(fl->dev, "level %d on: %ld\n",
					     fl->level, ret);
		break;
	case FLASH_IOC_GET_DUTY_NUMBER:
		ua.arg = FL_LEVEL_NUM;
		copy = true;
		break;
	case FLASH_IOC_GET_DUTY_CURRENT:
		ua.arg = fl_current_ma[fl_level_clamp(ua.arg)];
		copy = true;
		break;
	case FLASH_IOC_GET_HW_FAULT:
	case FLASH_IOC_GET_HW_FAULT2:
		fault = 0;
		if (cmd == FLASH_IOC_GET_HW_FAULT)
			led_get_flash_fault(fl->fled, &fault);
		ua.arg = fault;
		copy = true;
		break;
	/* stock returned 0 for these without copying a value back */
	case FLASH_IOC_GET_MAX_TORCH_DUTY:
	case FLASH_IOC_GET_HW_TIMEOUT:
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&fl->lock);

	if (copy && copy_to_user(uarg, &ua, sizeof(ua)))
		return -EFAULT;
	return ret;
}

static const struct file_operations fl_fops = {
	.owner = THIS_MODULE,
	.open = fl_open,
	.release = fl_release,
	.unlocked_ioctl = fl_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static void fl_quiesce(struct fl_compat *fl)
{
	cancel_delayed_work_sync(&fl->off_work);
	mutex_lock(&fl->lock);
	if (fl->on)
		fl_off_locked(fl);
	mutex_unlock(&fl->lock);
}

static int fl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fl_compat *fl;
	int ret;

	if (!mtk_cam_flash_enabled())
		return -ENODEV;

	fl = devm_kzalloc(dev, sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;
	fl->dev = dev;
	mutex_init(&fl->lock);
	INIT_DELAYED_WORK(&fl->off_work, fl_off_work);

	fl->cdev = devm_of_led_get(dev, 0);
	if (IS_ERR(fl->cdev))
		return dev_err_probe(dev, PTR_ERR(fl->cdev),
				     "flash LED not available\n");
	if (!(fl->cdev->flags & LED_DEV_CAP_FLASH))
		return dev_err_probe(dev, -EINVAL, "%s is not a flash LED\n",
				     dev_name(fl->cdev->dev));
	fl->fled = lcdev_to_flcdev(fl->cdev);

	fl->misc.minor = MISC_DYNAMIC_MINOR;
	fl->misc.name = "flashlight";
	fl->misc.fops = &fl_fops;
	fl->misc.parent = dev;
	platform_set_drvdata(pdev, fl);
	ret = misc_register(&fl->misc);
	if (ret)
		return ret;

	dev_info(dev, "/dev/flashlight ready on %s (torch max %u, flash %u-%u uA)\n",
		 dev_name(fl->cdev->dev), fl->cdev->max_brightness,
		 fl->fled->brightness.min, fl->fled->brightness.max);
	return 0;
}

static void fl_remove(struct platform_device *pdev)
{
	struct fl_compat *fl = platform_get_drvdata(pdev);

	misc_deregister(&fl->misc);
	fl_quiesce(fl);
}

static void fl_shutdown(struct platform_device *pdev)
{
	struct fl_compat *fl = platform_get_drvdata(pdev);

	if (fl)
		fl_quiesce(fl);
}

static int fl_suspend(struct device *dev)
{
	fl_quiesce(dev_get_drvdata(dev));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fl_pm_ops, fl_suspend, NULL);

static const struct of_device_id fl_of_match[] = {
	{ .compatible = "mediatek,mt6853-flashlight-compat" },
	{}
};
MODULE_DEVICE_TABLE(of, fl_of_match);

static struct platform_driver fl_driver = {
	.probe = fl_probe,
	.remove = fl_remove,
	.shutdown = fl_shutdown,
	.driver = {
		.name = "mtk-flashlight-compat",
		.of_match_table = fl_of_match,
		.pm = pm_sleep_ptr(&fl_pm_ops),
	},
};

static int __init fl_compat_init(void)
{
	if (!mtk_cam_flash_enabled()) {
		pr_info("flashlight-compat: disabled (boot with mtk_cam.hw=1 mtk_cam.flash=1)\n");
		return 0;
	}
	return platform_driver_register(&fl_driver);
}
module_init(fl_compat_init);
