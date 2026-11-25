// SPDX-License-Identifier: GPL-2.0-only
/*
 * Main camera lens actuator (/dev/MAINAF) for the stock MT6853 camera HAL.
 *
 * Port of the 4.14 drivers/misc/mediatek/lens/main driver, reduced to the
 * one actuator this board has: the DW9800W VCM of the s5kgm1st main camera
 * (I2C 0x0c on i2c2, DT node camera_main_af@18). The HAL (libcam.afhal)
 * selects the driver with AFIOC_S_SETDRVNAME "DW9800WAF" and then moves the
 * lens with AFIOC_T_MOVETO. The VCM supply is cam0_vcamaf (FAN53870 L7) or
 * cam0_vcamafp (PM8008 L7) of the kd_camera_hw node, taken on open and
 * dropped on release.
 *
 * Probing touches no hardware.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/ioctl.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <clocksource/arm_arch_timer.h>

#include "mtk_cam.h"

extern int camera_power;

#define AF_DRVNAME		"MAINAF"
#define AF_CLASS_NAME		"actuatordrv_main_af"
#define AF_MAGIC		'A'

#define AFDRV_DW9800WAF		"DW9800WAF"
#define DW9800W_I2C_ADDR	0x0c
#define DW9800W_MAX_POS		1023

#define CONVERT_CCU_TIMESTAMP	0x1000

struct stAF_MotorInfo {
	u32 u4CurrentPosition;
	u32 u4MacroPosition;
	u32 u4InfPosition;
	bool bIsMotorMoving;
	bool bIsMotorOpen;
	bool bIsSupportSR;
};

struct stAF_MotorName {
	u8 uMotorName[32];
};

struct stAF_CtrlCmd {
	long long i8CmdID;
	long long i8Param[2];
};

#define AFIOC_G_MOTORINFO	_IOR(AF_MAGIC, 0, struct stAF_MotorInfo)
#define AFIOC_T_MOVETO		_IOW(AF_MAGIC, 1, u32)
#define AFIOC_T_SETINFPOS	_IOW(AF_MAGIC, 2, u32)
#define AFIOC_T_SETMACROPOS	_IOW(AF_MAGIC, 3, u32)
#define AFIOC_S_SETDRVNAME	_IOW(AF_MAGIC, 10, struct stAF_MotorName)
#define AFIOC_S_SETPOWERDOWN	_IOW(AF_MAGIC, 11, u32)
#define AFIOC_S_SETPOWERCTRL	_IOW(AF_MAGIC, 13, u32)
#define AFIOC_S_SETDRVINIT	_IOW(AF_MAGIC, 16, u32)
#define AFIOC_G_GETDRVNAME	_IOWR(AF_MAGIC, 17, struct stAF_MotorName)
#define AFIOC_X_CTRLPARA	_IOWR(AF_MAGIC, 18, struct stAF_CtrlCmd)

static_assert(sizeof(struct stAF_MotorInfo) == 16);
static_assert(sizeof(struct stAF_CtrlCmd) == 24);

struct main_af {
	struct i2c_client *client;
	struct device *dev;
	struct cdev cdev;
	dev_t devno;
	struct class *class;
	struct regulator *vcamaf;
	bool vcamaf_on;
	/* Serialises open/release/ioctl and the state below. */
	struct mutex lock;
	bool opened;
	bool selected;
	bool inited;
	u32 inf, macro, cur;
};

static struct main_af *g_af;

static int dw9800w_write(struct main_af *af, u8 reg, const u8 *val, int n)
{
	u8 buf[3];
	struct i2c_msg msg = {
		.addr = DW9800W_I2C_ADDR,
		.flags = 0,
		.len = n + 1,
		.buf = buf,
	};
	int ret;

	buf[0] = reg;
	memcpy(&buf[1], val, n);
	ret = i2c_transfer(af->client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int dw9800w_init(struct main_af *af)
{
	static const u8 seq[][2] = {
		{ 0x02, 0x01 },	/* power down */
		{ 0x02, 0x00 },	/* active */
		{ 0xfe, 0xfe },	/* delay */
		{ 0x02, 0x02 },	/* ringing control on */
		{ 0x06, 0x40 },	/* SAC3 mode */
		{ 0x07, 0x60 },	/* resonance period */
		{ 0xfe, 0xfe },
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		if (seq[i][0] == 0xfe) {
			usleep_range(100, 200);
			continue;
		}
		ret = dw9800w_write(af, seq[i][0], &seq[i][1], 1);
		if (ret)
			return ret;
	}
	return 0;
}

static int dw9800w_move(struct main_af *af, u32 pos)
{
	u8 val[2];
	int ret;

	if (pos > af->macro || pos < af->inf || pos > DW9800W_MAX_POS)
		return -EINVAL;

	if (!af->inited) {
		ret = dw9800w_init(af);
		if (ret) {
			dev_err(af->dev, "DW9800W init failed: %d\n", ret);
			return ret;
		}
		af->inited = true;
		af->cur = 0;
	}

	if (af->cur == pos)
		return 0;

	val[0] = pos >> 8;
	val[1] = pos & 0xff;
	ret = dw9800w_write(af, 0x03, val, 2);
	if (ret) {
		dev_err(af->dev, "move to %u failed: %d\n", pos, ret);
		return ret;
	}
	af->cur = pos;
	return 0;
}

static void dw9800w_power_down(struct main_af *af)
{
	static const u8 pd = 0x01;

	if (af->inited)
		dw9800w_write(af, 0x02, &pd, 1);
	af->inited = false;
}

static struct regulator *af_get_supply(struct main_af *af)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct regulator *reg;

	np = of_find_compatible_node(NULL, NULL, "mediatek,imgsensor");
	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	reg = regulator_get_optional(&pdev->dev, camera_power == 2 ?
				     "cam0_vcamafp" : "cam0_vcamaf");
	put_device(&pdev->dev);
	return reg;
}

static void af_power(struct main_af *af, bool on)
{
	int ret;

	if (on && !af->vcamaf) {
		struct regulator *reg = af_get_supply(af);

		if (IS_ERR(reg)) {
			dev_warn(af->dev, "no VCM supply: %ld\n", PTR_ERR(reg));
			return;
		}
		af->vcamaf = reg;
	}
	if (!af->vcamaf || on == af->vcamaf_on)
		return;

	if (on) {
		ret = regulator_set_voltage(af->vcamaf, 2800000, 2800000);
		if (ret)
			dev_warn(af->dev, "set VCM supply 2.8V: %d\n", ret);
		ret = regulator_enable(af->vcamaf);
		if (ret) {
			dev_err(af->dev, "enable VCM supply: %d\n", ret);
			return;
		}
		af->vcamaf_on = true;
		usleep_range(5000, 5500);
	} else {
		regulator_disable(af->vcamaf);
		af->vcamaf_on = false;
	}
}

/* Arch counter ticks (CCU timestamps) to CLOCK_MONOTONIC microseconds. */
static long long af_tick_to_mono_us(long long tick)
{
	u64 rate = arch_timer_get_rate();
	u64 now_tick = arch_timer_read_counter();
	s64 now_ns = ktime_get_ns();
	s64 delta;

	if (!rate)
		return 0;
	delta = (s64)now_tick - tick;
	if (delta >= 0)
		now_ns -= mul_u64_u64_div_u64(delta, NSEC_PER_SEC, rate);
	else
		now_ns += mul_u64_u64_div_u64(-delta, NSEC_PER_SEC, rate);
	return div_s64(now_ns, NSEC_PER_USEC);
}

static long af_ctrl_param(void __user *arg)
{
	struct stAF_CtrlCmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.i8CmdID != CONVERT_CCU_TIMESTAMP)
		return -EPERM;
	cmd.i8Param[0] = af_tick_to_mono_us(cmd.i8Param[0]);
	cmd.i8Param[1] = af_tick_to_mono_us(cmd.i8Param[1]);
	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		return -EFAULT;
	return 1;
}

static long af_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct main_af *af = file->private_data;
	void __user *uarg = (void __user *)arg;
	struct stAF_MotorName name;
	struct stAF_MotorInfo info;
	long ret = 0;

	mutex_lock(&af->lock);
	switch (cmd) {
	case AFIOC_S_SETDRVNAME:
		if (copy_from_user(&name, uarg, sizeof(name))) {
			ret = -EFAULT;
			break;
		}
		name.uMotorName[sizeof(name.uMotorName) - 1] = '\0';
		af->selected = !strcmp(name.uMotorName, AFDRV_DW9800WAF);
		/* As in 4.14: 1 when the named driver exists, -1 otherwise. */
		ret = af->selected ? 1 : -1;
		break;
	case AFIOC_G_GETDRVNAME:
		if (copy_from_user(&name, uarg, sizeof(name))) {
			ret = -EFAULT;
			break;
		}
		name.uMotorName[sizeof(name.uMotorName) - 1] = '\0';
		if (strcmp(name.uMotorName, AFDRV_DW9800WAF))
			break;
		memset(&name, 0, sizeof(name));
		strscpy(name.uMotorName, "DW9800WAF.c", sizeof(name.uMotorName));
		ret = copy_to_user(uarg, &name, sizeof(name)) ? -EFAULT : 1;
		break;
	case AFIOC_S_SETDRVINIT:
		break;
	case AFIOC_S_SETPOWERDOWN:
		dw9800w_power_down(af);
		ret = 1;
		break;
	case AFIOC_S_SETPOWERCTRL:
		af_power(af, arg > 0);
		break;
	case AFIOC_X_CTRLPARA:
		ret = af_ctrl_param(uarg);
		break;
	case AFIOC_G_MOTORINFO:
		if (!af->selected) {
			ret = -EPERM;
			break;
		}
		memset(&info, 0, sizeof(info));
		info.u4MacroPosition = af->macro;
		info.u4InfPosition = af->inf;
		info.u4CurrentPosition = af->cur;
		info.bIsSupportSR = true;
		info.bIsMotorMoving = true;
		info.bIsMotorOpen = af->opened;
		if (copy_to_user(uarg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	case AFIOC_T_MOVETO:
		ret = af->selected ? dw9800w_move(af, arg) : -EPERM;
		break;
	case AFIOC_T_SETINFPOS:
		if (af->selected)
			af->inf = arg;
		else
			ret = -EPERM;
		break;
	case AFIOC_T_SETMACROPOS:
		if (af->selected)
			af->macro = arg;
		else
			ret = -EPERM;
		break;
	default:
		ret = -EPERM;
		break;
	}
	mutex_unlock(&af->lock);
	return ret;
}

static int af_open(struct inode *inode, struct file *file)
{
	struct main_af *af = g_af;

	if (!af)
		return -ENODEV;

	mutex_lock(&af->lock);
	if (af->opened) {
		mutex_unlock(&af->lock);
		return -EBUSY;
	}
	af->opened = true;
	af->selected = false;
	af->inited = false;
	af->inf = 0;
	af->macro = DW9800W_MAX_POS;
	af->cur = 0;
	af_power(af, true);
	mutex_unlock(&af->lock);

	file->private_data = af;
	return 0;
}

static int af_release(struct inode *inode, struct file *file)
{
	struct main_af *af = file->private_data;

	mutex_lock(&af->lock);
	dw9800w_power_down(af);
	af_power(af, false);
	af->selected = false;
	af->opened = false;
	mutex_unlock(&af->lock);
	return 0;
}

static const struct file_operations af_fops = {
	.owner = THIS_MODULE,
	.open = af_open,
	.release = af_release,
	.unlocked_ioctl = af_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int af_probe(struct i2c_client *client)
{
	struct main_af *af;
	struct device *dev;
	int ret;

	if (g_af)
		return -EBUSY;

	af = devm_kzalloc(&client->dev, sizeof(*af), GFP_KERNEL);
	if (!af)
		return -ENOMEM;
	af->client = client;
	af->dev = &client->dev;
	mutex_init(&af->lock);

	ret = alloc_chrdev_region(&af->devno, 0, 1, AF_DRVNAME);
	if (ret)
		return ret;
	cdev_init(&af->cdev, &af_fops);
	ret = cdev_add(&af->cdev, af->devno, 1);
	if (ret)
		goto err_region;
	af->class = class_create(AF_CLASS_NAME);
	if (IS_ERR(af->class)) {
		ret = PTR_ERR(af->class);
		goto err_cdev;
	}
	dev = device_create(af->class, &client->dev, af->devno, NULL,
			    AF_DRVNAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	i2c_set_clientdata(client, af);
	g_af = af;
	dev_info(&client->dev, "/dev/%s (DW9800W @0x%02x)\n", AF_DRVNAME,
		 DW9800W_I2C_ADDR);
	return 0;

err_class:
	class_destroy(af->class);
err_cdev:
	cdev_del(&af->cdev);
err_region:
	unregister_chrdev_region(af->devno, 1);
	return ret;
}

static void af_remove(struct i2c_client *client)
{
	struct main_af *af = i2c_get_clientdata(client);

	g_af = NULL;
	device_destroy(af->class, af->devno);
	class_destroy(af->class);
	cdev_del(&af->cdev);
	unregister_chrdev_region(af->devno, 1);
	if (af->vcamaf) {
		if (af->vcamaf_on)
			regulator_disable(af->vcamaf);
		regulator_put(af->vcamaf);
	}
}

static const struct of_device_id af_of_match[] = {
	{ .compatible = "mediatek,camera_main_af" },
	{ }
};
MODULE_DEVICE_TABLE(of, af_of_match);

static struct i2c_driver af_driver = {
	.driver = {
		.name = AF_DRVNAME,
		.of_match_table = af_of_match,
	},
	.probe = af_probe,
	.remove = af_remove,
};

static int __init main_af_init(void)
{
	if (!mtk_cam_hw_enabled())
		return 0;
	return i2c_add_driver(&af_driver);
}
late_initcall(main_af_init);
