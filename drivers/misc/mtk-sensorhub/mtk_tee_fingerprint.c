// SPDX-License-Identifier: GPL-2.0-only
/*
 * Side-mounted fingerprint sensor glue for the MT6853 (Redmi Note 9T).
 *
 * The board ships with either a Goodix or an FPC1022 sensor on spi5. Image
 * capture and matching run inside the Microtrust TEE; the kernel only
 * powers and resets the sensor, keeps the SPI controller clocked for the
 * TEE, forwards the sensor interrupt and selects the trusted application
 * used by /dev/teei_fp. This is a reduced port of the stock 4.14 drivers
 * drivers/input/fingerprint/goodix_cap/gf_spi_tee.c and
 * drivers/input/fingerprint/fpc/fpc1022_tee.c with their userspace ABI:
 *
 *  Goodix: /dev/goodix_fp (class goodix_fp), ioctl magic 'g',
 *          netlink protocol 29, input device "uinput-goodix".
 *  FPC:    sysfs attributes irq, hw_reset, clk_enable, wakeup_enable,
 *          do_wakeup, fingerdown_wait and fpid_get on the "fingerprint"
 *          platform device, input device "fpc_irq@<dev>".
 *
 * The driver binds to the root "mediatek,fpc1022_irq" node (pins and
 * interrupt) and uses the "goodix,goodix-fp" spi5 child to read the sensor
 * ID like the stock drivers did. If neither ID answers from the normal
 * world, both interfaces are exposed and the vendor HAL/TA pair does the
 * detection: opening /dev/goodix_fp selects the Goodix TA, using the FPC
 * sysfs nodes selects the FPC TA (vendor=auto, the default). vendor=goodix,
 * vendor=fpc or vendor=both force the choice.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "tee_client_api.h"
#include "teei_fp.h"

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Bind the TEE fingerprint glue driver");

static char *vendor = "auto";
module_param(vendor, charp, 0444);
MODULE_PARM_DESC(vendor, "auto (default), goodix, fpc or both");

static bool vfp_cycle = true;
module_param(vfp_cycle, bool, 0444);
MODULE_PARM_DESC(vfp_cycle, "Power-cycle VFP at probe if the bootloader left it on");

/* ~0.5 s of boot time; gpio_test in sysfs runs the same probe on demand */
static bool gpio_probe;
module_param(gpio_probe, bool, 0444);
MODULE_PARM_DESC(gpio_probe, "If the spi5 ID read fails, repeat it bit-banged on GPIO28-31");

#define FP_WAKE_HOLD_MS		2000
#define FP_SPI_DETECT_HZ	1000000
#define FP_VFP_UV		3000000
#define FP_SPI_WAKE_US		500	/* max ~600 us: 16-bit ticks at 109.2 MHz */
/*
 * The stock spi-mt65xx defaulted CS setup/hold/idle to one SCK period
 * (1 us at the 1 MHz detection clock); 6.18 leaves them at one source
 * clock tick (~9 ns) unless the device asks for more.
 */
#define FP_SPI_CS_DELAY_NS	1000

/* Goodix ABI */
#define GF_NETLINK_ROUTE	29
#define GF_NL_MSG_LEN		16
#define GF_NETLINK_IRQ		1
#define GF_DEV_NAME		"goodix_fp"
#define GF_INPUT_NAME		"uinput-goodix"

#define GF_IOC_MAGIC		'g'
#define GF_IOC_INIT		_IOR(GF_IOC_MAGIC, 0, u8)
#define GF_IOC_EXIT		_IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET		_IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ	_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ	_IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_SPI_CLK	_IOW(GF_IOC_MAGIC, 5, u32)
#define GF_IOC_DISABLE_SPI_CLK	_IO(GF_IOC_MAGIC, 6)
#define GF_IOC_ENABLE_POWER	_IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER	_IO(GF_IOC_MAGIC, 8)
#define GF_IOC_INPUT_KEY_EVENT	_IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_ENTER_SLEEP_MODE	_IO(GF_IOC_MAGIC, 10)
#define GF_IOC_GET_FW_INFO	_IOR(GF_IOC_MAGIC, 11, u8)
#define GF_IOC_REMOVE		_IO(GF_IOC_MAGIC, 12)
#define GF_IOC_CHIP_INFO	_IOW(GF_IOC_MAGIC, 13, struct gf_ioc_chip_info)
#define GF_IOC_NAV_EVENT	_IOW(GF_IOC_MAGIC, 14, int)

enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_POWER,
	GF_KEY_MENU,
	GF_KEY_BACK,
	GF_KEY_CAMERA,
};

struct gf_key {
	enum gf_key_event key;
	u32 value;
};

enum gf_nav_event {
	GF_NAV_NONE = 0,
	GF_NAV_FINGER_UP,
	GF_NAV_FINGER_DOWN,
	GF_NAV_UP,
	GF_NAV_DOWN,
	GF_NAV_LEFT,
	GF_NAV_RIGHT,
	GF_NAV_CLICK,
	GF_NAV_HEAVY,
	GF_NAV_LONG_PRESS,
	GF_NAV_DOUBLE_CLICK,
};

static const unsigned int gf_nav_keys[] = {
	[GF_NAV_UP] = KEY_UP,
	[GF_NAV_DOWN] = KEY_DOWN,
	[GF_NAV_LEFT] = KEY_LEFT,
	[GF_NAV_RIGHT] = KEY_RIGHT,
	[GF_NAV_CLICK] = KEY_VOLUMEDOWN,
	[GF_NAV_HEAVY] = KEY_CHAT,
	[GF_NAV_LONG_PRESS] = KEY_SEARCH,
	[GF_NAV_DOUBLE_CLICK] = KEY_VOLUMEUP,
};

static const unsigned int gf_input_keys[] = {
	KEY_SELECT, KEY_MENU, KEY_BACK, KEY_POWER, KEY_CAMERA,
	KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_VOLUMEDOWN,
	KEY_VOLUMEUP, KEY_SEARCH, KEY_CHAT,
};

struct gf_ioc_chip_info {
	u8 vendor_id;
	u8 mode;
	u8 operation;
	u8 reserved[5];
};

/* FPC ABI */
#define FPC_HWID_REG		252
#define FPC1022_CHIP		0x1800
#define FPC1022_CHIP_MASK	0xff00

static const struct TEEC_UUID uuid_ta_goodix = {
	0x8888c03f, 0xc30c, 0x4dd0,
	{ 0xa3, 0x19, 0xea, 0x29, 0x64, 0x3d, 0x4d, 0x4b }
};

static const struct TEEC_UUID uuid_ta_fpc = {
	0x7778c03f, 0xc30c, 0x4dd0,
	{ 0xa3, 0x19, 0xea, 0x29, 0x64, 0x3d, 0x4d, 0x4b }
};

enum fp_vendor {
	FP_NONE,
	FP_GOODIX,
	FP_FPC,
	FP_BOTH,
};

static const char * const fp_vendor_names[] = {
	[FP_NONE] = "auto",
	[FP_GOODIX] = "goodix",
	[FP_FPC] = "fpc",
	[FP_BOTH] = "both",
};

struct mtk_fp {
	struct device *dev;		/* the "fingerprint" platform device */
	struct spi_device *spi;
	struct device *spi_ctlr;	/* runtime-PM handle of the SPI host */
	struct pinctrl *pinctrl;
	struct pinctrl_state *st_rst_h;
	struct pinctrl_state *st_rst_l;
	struct regulator *vfp;
	struct gpio_desc *irq_gpio;
	struct wakeup_source *ws;
	struct mutex lock;		/* clock, power, irq and TA state */
	enum fp_vendor vendor;		/* sensor seen by the REE ID read */
	enum fp_vendor ta;		/* trusted application in uuid_fp */
	int irq;
	bool irq_enabled;
	bool powered;
	unsigned int clk_ref;
	bool wakeup_enabled;
	bool wait_finger_down;

	/* Goodix */
	bool gf_on;
	bool gf_power_seq;
	u8 gf_vendor_id;
	struct input_dev *gf_input;
	dev_t devno;
	struct class *class;
	struct cdev cdev;
	struct device *chrdev;
	struct sock *nl_sk;
	u32 nl_pid;

	/* FPC */
	bool fpc_on;
	struct input_dev *fpc_input;
	char fpc_input_name[32];
};

static struct mtk_fp *g_fp;
static u32 fp_spi_hz = FP_SPI_DETECT_HZ;

static void fp_reset(struct mtk_fp *fp, unsigned int low_ms,
		     unsigned int settle_ms)
{
	pinctrl_select_state(fp->pinctrl, fp->st_rst_l);
	msleep(low_ms);
	pinctrl_select_state(fp->pinctrl, fp->st_rst_h);
	if (settle_ms)
		msleep(settle_ms);
}

static int fp_power_locked(struct mtk_fp *fp, bool on)
{
	int ret = 0;

	if (fp->powered == on || !fp->vfp)
		goto out;
	if (on)
		ret = regulator_enable(fp->vfp);
	else
		ret = regulator_disable(fp->vfp);
out:
	if (!ret)
		fp->powered = on;
	return ret;
}

/*
 * The TEE drives the SPI host itself, so the kernel only has to keep the
 * controller clocked: the 6.18 equivalent of mt_spi_{en,dis}able_master_clk.
 */
static int fp_clk_locked(struct mtk_fp *fp, bool on)
{
	int ret;

	if (on) {
		if (fp->clk_ref++)
			return 0;
		ret = pm_runtime_resume_and_get(fp->spi_ctlr);
		if (ret)
			fp->clk_ref = 0;
		return ret;
	}

	if (!fp->clk_ref)
		return 0;
	if (!--fp->clk_ref)
		pm_runtime_put(fp->spi_ctlr);
	return 0;
}

static void fp_irq_locked(struct mtk_fp *fp, bool on)
{
	if (fp->irq_enabled == on)
		return;
	if (on)
		enable_irq(fp->irq);
	else
		disable_irq(fp->irq);
	fp->irq_enabled = on;
}

/*
 * Point /dev/teei_fp at the Goodix or FPC trusted application. teei_fp
 * opens a fresh session per command, so a switch applies to the next one.
 */
static void fp_select_ta_locked(struct mtk_fp *fp, enum fp_vendor ta)
{
	if (fp->ta == ta)
		return;
	memcpy(&uuid_fp, ta == FP_GOODIX ? &uuid_ta_goodix : &uuid_ta_fpc,
	       sizeof(uuid_fp));
	fp->ta = ta;
	dev_info(fp->dev, "%s trusted application selected\n",
		 fp_vendor_names[ta]);

	/* the stock FPC driver kept its interrupt enabled at all times */
	if (ta == FP_FPC)
		fp_irq_locked(fp, true);
}

static void gf_netlink_send(struct mtk_fp *fp, u8 cmd)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	u32 pid = READ_ONCE(fp->nl_pid);

	if (!fp->nl_sk || !pid)
		return;

	skb = alloc_skb(NLMSG_SPACE(GF_NL_MSG_LEN), GFP_KERNEL);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, GF_NL_MSG_LEN, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	memset(nlmsg_data(nlh), 0, GF_NL_MSG_LEN);
	*(u8 *)nlmsg_data(nlh) = cmd;
	netlink_unicast(fp->nl_sk, skb, pid, MSG_DONTWAIT);
}

static void gf_netlink_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;

	if (!g_fp || skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	WRITE_ONCE(g_fp->nl_pid, nlh->nlmsg_pid);
}

static irqreturn_t fp_irq_thread(int irq, void *data)
{
	struct mtk_fp *fp = data;
	bool gf = fp->gf_on && READ_ONCE(fp->ta) == FP_GOODIX;

	if (gf || READ_ONCE(fp->wakeup_enabled))
		__pm_wakeup_event(fp->ws, FP_WAKE_HOLD_MS);
	if (gf) {
		gf_netlink_send(fp, GF_NETLINK_IRQ);
	} else if (fp->fpc_on) {
		sysfs_notify(&fp->dev->kobj, NULL, "irq");
		WRITE_ONCE(fp->wait_finger_down, false);
	}
	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------- Goodix */

static void gf_report_key(struct mtk_fp *fp, unsigned int code)
{
	input_report_key(fp->gf_input, code, 1);
	input_sync(fp->gf_input);
	input_report_key(fp->gf_input, code, 0);
	input_sync(fp->gf_input);
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct mtk_fp *fp = g_fp;

	if (!fp)
		return -ENXIO;
	mutex_lock(&fp->lock);
	fp_select_ta_locked(fp, FP_GOODIX);
	mutex_unlock(&fp->lock);
	filp->private_data = fp;
	return nonseekable_open(inode, filp);
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct mtk_fp *fp = filp->private_data;

	mutex_lock(&fp->lock);
	if (fp->ta == FP_GOODIX)
		fp_irq_locked(fp, false);
	mutex_unlock(&fp->lock);
	return 0;
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mtk_fp *fp = filp->private_data;
	void __user *uarg = (void __user *)arg;
	struct gf_ioc_chip_info info;
	struct gf_key key;
	int nav;
	u8 val8;
	long ret = 0;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -EINVAL;

	mutex_lock(&fp->lock);
	switch (cmd) {
	case GF_IOC_INIT:
		val8 = GF_NETLINK_ROUTE;
		if (copy_to_user(uarg, &val8, sizeof(val8))) {
			ret = -EFAULT;
			break;
		}
		fp_select_ta_locked(fp, FP_GOODIX);
		/* stock: interrupt requested here but left disabled */
		fp_irq_locked(fp, false);
		break;
	case GF_IOC_EXIT:
	case GF_IOC_DISABLE_IRQ:
		fp_irq_locked(fp, false);
		break;
	case GF_IOC_ENABLE_IRQ:
		fp_irq_locked(fp, true);
		break;
	case GF_IOC_RESET:
		fp_reset(fp, 5, 60);
		break;
	case GF_IOC_ENABLE_SPI_CLK:
		ret = fp_clk_locked(fp, true);
		break;
	case GF_IOC_DISABLE_SPI_CLK:
		ret = fp_clk_locked(fp, false);
		break;
	case GF_IOC_ENABLE_POWER:
		/* stock: VFP stays on, the first enable pulses reset */
		ret = fp_power_locked(fp, true);
		if (!ret && !fp->gf_power_seq) {
			fp_reset(fp, 15, 0);
			fp->gf_power_seq = true;
		}
		break;
	case GF_IOC_DISABLE_POWER:
		fp->gf_power_seq = false;
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&key, uarg, sizeof(key))) {
			ret = -EFAULT;
			break;
		}
		if (key.value == 1 && key.key == GF_KEY_POWER)
			gf_report_key(fp, KEY_POWER);
		else if (key.value == 1 && key.key == GF_KEY_CAMERA)
			gf_report_key(fp, KEY_CAMERA);
		break;
	case GF_IOC_NAV_EVENT:
		if (copy_from_user(&nav, uarg, sizeof(nav))) {
			ret = -EFAULT;
			break;
		}
		if (nav > 0 && nav < ARRAY_SIZE(gf_nav_keys) && gf_nav_keys[nav])
			gf_report_key(fp, gf_nav_keys[nav]);
		break;
	case GF_IOC_GET_FW_INFO:
		val8 = 0;
		if (copy_to_user(uarg, &val8, sizeof(val8)))
			ret = -EFAULT;
		break;
	case GF_IOC_CHIP_INFO:
		if (copy_from_user(&info, uarg, sizeof(info))) {
			ret = -EFAULT;
			break;
		}
		fp->gf_vendor_id = info.vendor_id;
		dev_info(fp->dev, "goodix chip info vendor 0x%x mode 0x%x op 0x%x\n",
			 info.vendor_id, info.mode, info.operation);
		break;
	case GF_IOC_ENTER_SLEEP_MODE:
	case GF_IOC_REMOVE:
		break;
	default:
		/* stock ignores the REE transfer commands it does not build */
		dev_dbg(fp->dev, "unsupported goodix ioctl 0x%x\n", cmd);
		break;
	}
	mutex_unlock(&fp->lock);
	return ret;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.open = gf_open,
	.release = gf_release,
	.unlocked_ioctl = gf_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static ssize_t debug_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "vendor id 0x%x\n", g_fp->gf_vendor_id);
}
static DEVICE_ATTR_RO(debug);

static struct attribute *gf_debug_attrs[] = {
	&dev_attr_debug.attr,
	NULL
};

static const struct attribute_group gf_debug_group = {
	.name = "debug",
	.attrs = gf_debug_attrs,
};

static int gf_setup(struct mtk_fp *fp)
{
	struct netlink_kernel_cfg cfg = { .input = gf_netlink_recv };
	int i, ret;

	fp->gf_input = devm_input_allocate_device(fp->dev);
	if (!fp->gf_input)
		return -ENOMEM;
	fp->gf_input->name = GF_INPUT_NAME;
	for (i = 0; i < ARRAY_SIZE(gf_input_keys); i++)
		input_set_capability(fp->gf_input, EV_KEY, gf_input_keys[i]);
	ret = input_register_device(fp->gf_input);
	if (ret)
		return ret;

	fp->nl_sk = netlink_kernel_create(&init_net, GF_NETLINK_ROUTE, &cfg);
	if (!fp->nl_sk)
		return -EBUSY;

	ret = alloc_chrdev_region(&fp->devno, 0, 1, GF_DEV_NAME);
	if (ret)
		goto err_nl;

	fp->class = class_create(GF_DEV_NAME);
	if (IS_ERR(fp->class)) {
		ret = PTR_ERR(fp->class);
		goto err_region;
	}

	cdev_init(&fp->cdev, &gf_fops);
	fp->cdev.owner = THIS_MODULE;
	ret = cdev_add(&fp->cdev, fp->devno, 1);
	if (ret)
		goto err_class;

	fp->chrdev = device_create(fp->class, &fp->spi->dev, fp->devno, fp,
				   GF_DEV_NAME);
	if (IS_ERR(fp->chrdev)) {
		ret = PTR_ERR(fp->chrdev);
		goto err_cdev;
	}

	if (sysfs_create_group(&fp->spi->dev.kobj, &gf_debug_group))
		dev_warn(fp->dev, "goodix debug attributes unavailable\n");
	fp->gf_on = true;
	return 0;

err_cdev:
	cdev_del(&fp->cdev);
err_class:
	class_destroy(fp->class);
err_region:
	unregister_chrdev_region(fp->devno, 1);
err_nl:
	netlink_kernel_release(fp->nl_sk);
	fp->nl_sk = NULL;
	return ret;
}

static void gf_teardown(struct mtk_fp *fp)
{
	if (!fp->gf_on)
		return;
	fp->gf_on = false;
	sysfs_remove_group(&fp->spi->dev.kobj, &gf_debug_group);
	device_destroy(fp->class, fp->devno);
	cdev_del(&fp->cdev);
	class_destroy(fp->class);
	unregister_chrdev_region(fp->devno, 1);
	netlink_kernel_release(fp->nl_sk);
	fp->nl_sk = NULL;
}

/* ------------------------------------------------------------------- FPC */

static ssize_t irq_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  gpiod_get_value_cansleep(g_fp->irq_gpio));
}

static ssize_t irq_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR(irq, 0600, irq_show, irq_store);

static ssize_t hw_reset_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (!sysfs_streq(buf, "reset"))
		return -EINVAL;

	mutex_lock(&g_fp->lock);
	fp_select_ta_locked(g_fp, FP_FPC);
	fp_power_locked(g_fp, true);
	pinctrl_select_state(g_fp->pinctrl, g_fp->st_rst_h);
	usleep_range(100, 200);
	fp_reset(g_fp, 5, 0);
	usleep_range(100, 200);
	mutex_unlock(&g_fp->lock);
	return count;
}
static DEVICE_ATTR_WO(hw_reset);

static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	if (sysfs_streq(buf, "enable"))
		WRITE_ONCE(g_fp->wakeup_enabled, true);
	else if (sysfs_streq(buf, "disable"))
		WRITE_ONCE(g_fp->wakeup_enabled, false);
	return count;
}
static DEVICE_ATTR_WO(wakeup_enable);

static ssize_t do_wakeup_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	input_report_key(g_fp->fpc_input, KEY_POWER, 1);
	input_report_key(g_fp->fpc_input, KEY_POWER, 0);
	input_sync(g_fp->fpc_input);
	return count;
}
static DEVICE_ATTR_WO(do_wakeup);

static ssize_t clk_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;

	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;

	mutex_lock(&g_fp->lock);
	if (buf[0] == '1')
		fp_select_ta_locked(g_fp, FP_FPC);
	ret = fp_clk_locked(g_fp, buf[0] == '1');
	mutex_unlock(&g_fp->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(clk_enable);

static ssize_t fingerdown_wait_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	if (sysfs_streq(buf, "enable"))
		WRITE_ONCE(g_fp->wait_finger_down, true);
	else if (sysfs_streq(buf, "disable"))
		WRITE_ONCE(g_fp->wait_finger_down, false);
	else
		return -EINVAL;
	return count;
}
static DEVICE_ATTR_WO(fingerdown_wait);

static ssize_t fpid_get_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_fp->vendor == FP_FPC ||
			  (g_fp->vendor == FP_NONE && g_fp->ta == FP_FPC));
}
static DEVICE_ATTR(fpid_get, 0600, fpid_get_show, NULL);

static struct attribute *fpc_attrs[] = {
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_do_wakeup.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_fpid_get.attr,
	&dev_attr_fingerdown_wait.attr,
	NULL
};

static const struct attribute_group fpc_group = {
	.attrs = fpc_attrs,
};

static int fpc_setup(struct mtk_fp *fp)
{
	int ret;

	fp->fpc_input = devm_input_allocate_device(fp->dev);
	if (!fp->fpc_input)
		return -ENOMEM;
	snprintf(fp->fpc_input_name, sizeof(fp->fpc_input_name),
		 "fpc_irq@%s", dev_name(fp->dev));
	fp->fpc_input->name = fp->fpc_input_name;
	input_set_capability(fp->fpc_input, EV_MSC, MSC_SCAN);
	input_set_capability(fp->fpc_input, EV_KEY, KEY_WAKEUP);
	input_set_capability(fp->fpc_input, EV_KEY, KEY_POWER);
	ret = input_register_device(fp->fpc_input);
	if (ret)
		return ret;

	ret = sysfs_create_group(&fp->dev->kobj, &fpc_group);
	if (ret)
		return ret;

	fp->wakeup_enabled = true;
	fp->fpc_on = true;
	return 0;
}

/* ------------------------------------------------------------- detection */

static int fp_spi_xfer(struct spi_device *spi, const u8 *cmd, size_t cmd_len,
		       u8 *rx, size_t rx_len)
{
	struct spi_transfer xfer[2] = { };
	u32 hz = READ_ONCE(fp_spi_hz);
	u8 *buf;
	int ret;

	buf = kzalloc(cmd_len + rx_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, cmd, cmd_len);

	xfer[0].tx_buf = buf;
	xfer[0].len = cmd_len;
	xfer[0].speed_hz = hz;
	xfer[0].delay.value = 5;
	xfer[0].delay.unit = SPI_DELAY_UNIT_USECS;
	xfer[1].tx_buf = buf + cmd_len;
	xfer[1].rx_buf = buf + cmd_len;
	xfer[1].len = rx_len;
	xfer[1].speed_hz = hz;
	xfer[1].delay.value = 5;
	xfer[1].delay.unit = SPI_DELAY_UNIT_USECS;

	ret = spi_sync_transfer(spi, xfer, rx_len ? 2 : 1);
	if (!ret && rx_len)
		memcpy(rx, buf + cmd_len, rx_len);
	kfree(buf);
	return ret;
}

/* one full-duplex transfer, data read back into @buf (stock gf_spi_read) */
static int fp_spi_fdx(struct spi_device *spi, u8 *buf, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = buf,
		.rx_buf = buf,
		.len = len,
		.speed_hz = READ_ONCE(fp_spi_hz),
		.delay = { .value = 5, .unit = SPI_DELAY_UNIT_USECS },
	};

	return spi_sync_transfer(spi, &xfer, 1);
}

static bool fp_is_goodix(struct mtk_fp *fp)
{
	static const u8 addr[] = { 0xf0, 0x00, 0x00 };
	u8 rx[4] = { };
	u8 *buf;
	int ret;

	/*
	 * 0xF0 + address, then 0xF1 + data in a second chip-select cycle;
	 * like stock, 0xF1 and the data share a single transfer.
	 */
	ret = fp_spi_xfer(fp->spi, addr, sizeof(addr), NULL, 0);
	if (!ret) {
		buf = kzalloc(1 + sizeof(rx), GFP_KERNEL);
		if (!buf)
			return false;
		buf[0] = 0xf1;
		ret = fp_spi_fdx(fp->spi, buf, 1 + sizeof(rx));
		memcpy(rx, buf + 1, sizeof(rx));
		kfree(buf);
	}
	dev_info(fp->dev, "goodix id %*ph (%d)\n", 4, rx, ret);
	if (ret)
		return false;
	return (rx[0] == 0x07 || rx[0] == 0x10) && rx[3] == 0x25;
}

static bool fp_is_fpc(struct mtk_fp *fp)
{
	static const u8 reg[] = { FPC_HWID_REG };
	u8 rx[2];
	u16 id = 0;
	int i, ret;

	for (i = 0; i < 2; i++) {
		ret = fp_spi_xfer(fp->spi, reg, sizeof(reg), rx, sizeof(rx));
		if (ret)
			continue;
		id = (rx[0] << 8) | rx[1];
		if ((id & FPC1022_CHIP_MASK) == FPC1022_CHIP)
			break;
	}
	dev_info(fp->dev, "fpc hwid 0x%04x (%d)\n", id, ret);
	return !ret && (id & FPC1022_CHIP_MASK) == FPC1022_CHIP;
}

/* SPI host configuration as programmed for the last transfer (clock held) */
static void fp_dump_spi(struct mtk_fp *fp)
{
	void __iomem *base = of_iomap(fp->spi_ctlr->of_node, 0);

	if (!base)
		return;
	dev_info(fp->dev,
		 "spi host cfg0 %08x cfg1 %08x cmd %08x pad %08x cfg2 %08x mode %u max %u Hz\n",
		 readl(base + 0x00), readl(base + 0x04), readl(base + 0x18),
		 readl(base + 0x24), readl(base + 0x28), fp->spi->mode,
		 fp->spi->max_speed_hz);
	iounmap(base);
}

static enum fp_vendor fp_detect_once(struct mtk_fp *fp, unsigned int settle_ms)
{
	int lvl_rst, lvl_run;

	/* sensor liveness hint: does its INT line follow the reset pulse? */
	pinctrl_select_state(fp->pinctrl, fp->st_rst_l);
	msleep(15);
	lvl_rst = gpiod_get_value_cansleep(fp->irq_gpio);
	pinctrl_select_state(fp->pinctrl, fp->st_rst_h);
	if (settle_ms)
		msleep(settle_ms);
	lvl_run = gpiod_get_value_cansleep(fp->irq_gpio);
	dev_info(fp->dev, "irq gpio %d in reset, %d after %u ms\n",
		 lvl_rst, lvl_run, settle_ms);

	if (fp_is_goodix(fp))
		return FP_GOODIX;
	if (fp_is_fpc(fp))
		return FP_FPC;
	return FP_NONE;
}

static void fp_detect_failed(struct mtk_fp *fp)
{
	dev_info(fp->dev, "no REE sensor id: vfp %s %d uV, irq gpio %d\n",
		 fp->vfp && regulator_is_enabled(fp->vfp) ? "on" : "off",
		 fp->vfp ? regulator_get_voltage(fp->vfp) : 0,
		 gpiod_get_value_cansleep(fp->irq_gpio));
	fp_dump_spi(fp);
}

static enum fp_vendor fp_detect(struct mtk_fp *fp)
{
	/* stock order: reset 15 ms low, clock on, ~1 ms, then read the ID */
	static const unsigned int settle_ms[] = { 2, 60 };
	enum fp_vendor v;
	int i;

	struct spi_delay saved;

	for (i = 0; i < ARRAY_SIZE(settle_ms); i++) {
		v = fp_detect_once(fp, settle_ms[i]);
		if (v != FP_NONE)
			return v;
	}
	fp_detect_failed(fp);

	/*
	 * c3q: with CS asserted the sensor held MISO low for ~170 us, longer
	 * than a whole 1 MHz ID read. Retry with a long CS-to-clock delay.
	 */
	saved = fp->spi->cs_setup;
	fp->spi->cs_setup.value = FP_SPI_WAKE_US;
	fp->spi->cs_setup.unit = SPI_DELAY_UNIT_USECS;
	if (!spi_setup(fp->spi)) {
		dev_info(fp->dev, "retrying with %u us cs setup\n",
			 FP_SPI_WAKE_US);
		v = fp_detect_once(fp, 10);
		if (v != FP_NONE) {
			fp_dump_spi(fp);
			return v;
		}
	}
	fp->spi->cs_setup = saved;
	spi_setup(fp->spi);
	return FP_NONE;
}

/* --------------------------------------------- GPIO (bit-banged) probe */

/*
 * The ID reads with GPIO28-31 in GPIO mode, bypassing the spi5 host, over
 * several SPI modes, clock rates and CS-to-clock delays. A MISO trace with
 * CS held low and no clock separates a sensor that needs time after CS
 * (MISO low for a while, then released) from one that ignores the command.
 */
enum { FP_BB_CLK, FP_BB_CS, FP_BB_MO, FP_BB_MI, FP_BB_NR };

struct fp_bb {
	struct gpio_desc *g[FP_BB_NR];
	unsigned int mode;	/* SPI mode 0-3 */
	unsigned int hp_us;	/* half clock period */
	unsigned int cs_us;	/* CS low to the first clock edge */
};

static u8 fp_bb_byte(struct fp_bb *bb, u8 out)
{
	bool cpol = bb->mode & SPI_CPOL, cpha = bb->mode & SPI_CPHA;
	u8 in = 0;
	int b;

	for (b = 7; b >= 0; b--) {
		if (cpha)
			gpiod_set_value_cansleep(bb->g[FP_BB_CLK], !cpol);
		gpiod_set_value_cansleep(bb->g[FP_BB_MO], (out >> b) & 1);
		udelay(bb->hp_us);
		/* sampling edge: leading for CPHA 0, trailing for CPHA 1 */
		gpiod_set_value_cansleep(bb->g[FP_BB_CLK], cpha ? cpol : !cpol);
		in = (in << 1) | !!gpiod_get_value_cansleep(bb->g[FP_BB_MI]);
		udelay(bb->hp_us);
		if (!cpha)
			gpiod_set_value_cansleep(bb->g[FP_BB_CLK], cpol);
	}
	return in;
}

static void fp_bb_xfer(struct fp_bb *bb, const u8 *tx, u8 *rx, size_t len)
{
	size_t i;

	gpiod_set_value_cansleep(bb->g[FP_BB_CLK], bb->mode & SPI_CPOL);
	gpiod_set_value_cansleep(bb->g[FP_BB_CS], 0);
	fsleep(bb->cs_us);
	for (i = 0; i < len; i++)
		rx[i] = fp_bb_byte(bb, tx[i]);
	udelay(10);
	gpiod_set_value_cansleep(bb->g[FP_BB_CS], 1);
	udelay(50);
}

/* CS low, clock idle: when (~us) MISO first reads 1 and how often it flips */
static void fp_bb_trace(struct fp_bb *bb, int *first_hi, int *edges, int *last)
{
	int i, v, prev = -1;

	*first_hi = -1;
	*edges = 0;
	gpiod_set_value_cansleep(bb->g[FP_BB_CS], 0);
	for (i = 0; i < 200; i++) {
		v = !!gpiod_get_value_cansleep(bb->g[FP_BB_MI]);
		if (v && *first_hi < 0)
			*first_hi = i * 10;
		if (prev >= 0 && v != prev)
			(*edges)++;
		prev = v;
		udelay(10);
	}
	*last = prev;
	gpiod_set_value_cansleep(bb->g[FP_BB_CS], 1);
	udelay(100);
}

/*
 * Drive each line low then high and read the pad back (paris reads DI even
 * for outputs): "01" is good; "00"/"11" means the pad is held externally.
 */
static void fp_bb_loopback(struct mtk_fp *fp, struct fp_bb *bb)
{
	static const int idle[FP_BB_NR] = { 0, 1, 0, -1 };
	int r[FP_BB_NR][2], i, lvl;

	for (i = 0; i < FP_BB_NR; i++) {
		if (i == FP_BB_MI && gpiod_direction_output(bb->g[i], 0)) {
			r[i][0] = r[i][1] = 9;
			continue;
		}
		for (lvl = 0; lvl < 2; lvl++) {
			gpiod_set_value_cansleep(bb->g[i], lvl);
			udelay(20);
			r[i][lvl] = !!gpiod_get_value_cansleep(bb->g[i]);
		}
		if (idle[i] >= 0)
			gpiod_set_value_cansleep(bb->g[i], idle[i]);
		else
			gpiod_direction_input(bb->g[i]);
	}
	udelay(50);
	dev_info(fp->dev,
		 "gpio loopback (drive 0,1 -> read): clk %d%d cs %d%d mo %d%d mi %d%d\n",
		 r[0][0], r[0][1], r[1][0], r[1][1], r[2][0], r[2][1],
		 r[3][0], r[3][1]);
}

static enum fp_vendor fp_gpio_probe(struct mtk_fp *fp)
{
	static const char * const st_names[] = { "fp-gpio-pu", "fp-gpio-pd" };
	static const struct {
		u8 mode, hp_us;
		u16 cs_us;
	} runs[] = {
		{ 0, 5, 10 }, { 0, 50, 10 }, { 0, 5, 500 }, { 0, 5, 2000 },
		{ 3, 5, 10 }, { 3, 5, 500 }, { 1, 5, 10 }, { 2, 5, 10 },
	};
	int idle[2], first[2], edges[2], last[2];
	struct pinctrl_state *st[2], *def;
	enum fp_vendor v = FP_NONE;
	u8 tx[5], gf[5], fpc[3];
	struct fp_bb bb = { };
	struct pinctrl *p;
	u16 hwid;
	int i, ret;

	p = pinctrl_get(fp->spi_ctlr);
	if (IS_ERR(p))
		return FP_NONE;
	def = pinctrl_lookup_state(p, PINCTRL_STATE_DEFAULT);
	st[0] = pinctrl_lookup_state(p, st_names[0]);
	st[1] = pinctrl_lookup_state(p, st_names[1]);
	if (IS_ERR(def) || IS_ERR(st[0]) || IS_ERR(st[1])) {
		dev_info(fp->dev, "gpio probe: spi5 lacks fp-gpio pin states\n");
		goto out_put;
	}

	ret = pinctrl_select_state(p, st[0]);
	if (ret)
		goto out_restore;
	for (i = 0; i < FP_BB_NR; i++) {
		bb.g[i] = gpiod_get_index(fp->dev, "fp-spi", i,
					  i == FP_BB_MI ? GPIOD_IN :
					  i == FP_BB_CS ? GPIOD_OUT_HIGH :
					  GPIOD_OUT_LOW);
		if (IS_ERR(bb.g[i])) {
			dev_info(fp->dev, "gpio probe: fp-spi-gpios %d: %ld\n",
				 i, PTR_ERR(bb.g[i]));
			bb.g[i] = NULL;
			goto out_gpio;
		}
	}

	fp_bb_loopback(fp, &bb);

	for (i = 0; i < 2; i++) {
		if (pinctrl_select_state(p, st[i]))
			goto out_gpio;
		udelay(50);
		idle[i] = gpiod_get_value_cansleep(bb.g[FP_BB_MI]);
		fp_bb_trace(&bb, &first[i], &edges[i], &last[i]);
	}
	dev_info(fp->dev,
		 "gpio trace, cs low 2 ms: pull-up idle %d first-high ~%d us edges %d end %d; pull-down idle %d first-high ~%d us edges %d end %d\n",
		 idle[0], first[0], edges[0], last[0],
		 idle[1], first[1], edges[1], last[1]);
	pinctrl_select_state(p, st[0]);

	/* the same trace with the sensor held in reset, then unpowered */
	pinctrl_select_state(fp->pinctrl, fp->st_rst_l);
	msleep(5);
	udelay(50);
	idle[0] = gpiod_get_value_cansleep(bb.g[FP_BB_MI]);
	ret = gpiod_get_value_cansleep(fp->irq_gpio);
	fp_bb_trace(&bb, &first[0], &edges[0], &last[0]);
	dev_info(fp->dev,
		 "gpio trace, reset low: irq %d, pull-up idle %d first-high ~%d us edges %d end %d\n",
		 ret, idle[0], first[0], edges[0], last[0]);
	if (fp->vfp && fp->powered && !regulator_disable(fp->vfp)) {
		msleep(100);
		idle[0] = gpiod_get_value_cansleep(bb.g[FP_BB_MI]);
		ret = gpiod_get_value_cansleep(fp->irq_gpio);
		fp_bb_trace(&bb, &first[0], &edges[0], &last[0]);
		idle[1] = regulator_is_enabled(fp->vfp);
		dev_info(fp->dev,
			 "gpio trace, vfp off (reg enabled %d): irq %d, pull-up idle %d first-high ~%d us edges %d end %d\n",
			 idle[1], ret, idle[0], first[0], edges[0], last[0]);
		if (regulator_enable(fp->vfp)) {
			fp->powered = false;
			dev_warn(fp->dev, "gpio probe: vfp re-enable failed\n");
			goto out_gpio;
		}
		msleep(10);
	}
	pinctrl_select_state(fp->pinctrl, fp->st_rst_h);
	msleep(10);

	for (i = 0; i < ARRAY_SIZE(runs) && v == FP_NONE; i++) {
		bb.mode = runs[i].mode;
		bb.hp_us = runs[i].hp_us;
		bb.cs_us = runs[i].cs_us;
		gpiod_set_value_cansleep(bb.g[FP_BB_CLK], bb.mode & SPI_CPOL);
		fp_reset(fp, 15, 10);

		memset(tx, 0, sizeof(tx));
		tx[0] = 0xf0;
		fp_bb_xfer(&bb, tx, gf, 3);
		tx[0] = 0xf1;
		fp_bb_xfer(&bb, tx, gf, 5);
		tx[0] = FPC_HWID_REG;
		fp_bb_xfer(&bb, tx, fpc, 3);

		hwid = (fpc[1] << 8) | fpc[2];
		if ((gf[1] == 0x07 || gf[1] == 0x10) && gf[4] == 0x25)
			v = FP_GOODIX;
		else if ((hwid & FPC1022_CHIP_MASK) == FPC1022_CHIP)
			v = FP_FPC;
		dev_info(fp->dev,
			 "gpio mode %u, %u us half clock, %u us cs setup: goodix %*ph, fpc %*ph -> %s\n",
			 bb.mode, bb.hp_us, bb.cs_us, 5, gf, 3, fpc,
			 v == FP_NONE ? "none" : fp_vendor_names[v]);
	}

out_gpio:
	for (i = 0; i < FP_BB_NR; i++)
		if (bb.g[i])
			gpiod_put(bb.g[i]);
out_restore:
	ret = pinctrl_select_state(p, def);
	if (ret)
		dev_warn(fp->dev, "gpio probe: restoring spi5 pins: %d\n", ret);
out_put:
	pinctrl_put(p);
	return v;
}
/* bootloader-on VFP: give the sensor a clean power-on like a cold boot */
static void fp_vfp_cycle_locked(struct mtk_fp *fp, unsigned int off_ms)
{
	int ret;

	if (!fp->vfp || !fp->powered)
		return;
	ret = regulator_disable(fp->vfp);
	if (ret)
		return;
	msleep(off_ms);
	ret = regulator_enable(fp->vfp);
	if (ret) {
		fp->powered = false;
		dev_warn(fp->dev, "vfp re-enable: %d\n", ret);
		return;
	}
	msleep(10);
}

/*
 * Debug (root only): "echo 1 > gpio_test" runs the GPIO probe, "echo cycle"
 * power-cycles VFP first. Stop the fingerprint HAL before using it.
 */
static ssize_t gpio_test_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mtk_fp *fp = g_fp;
	enum fp_vendor v;
	int ret;

	if (!fp)
		return -ENODEV;
	mutex_lock(&fp->lock);
	ret = fp_power_locked(fp, true);
	if (!ret) {
		if (sysfs_streq(buf, "cycle"))
			fp_vfp_cycle_locked(fp, 100);
		dev_info(dev, "gpio test: vfp %s %d uV\n",
			 fp->vfp && regulator_is_enabled(fp->vfp) ? "on" : "off",
			 fp->vfp ? regulator_get_voltage(fp->vfp) : 0);
		v = fp_gpio_probe(fp);
		dev_info(dev, "gpio test: %s\n",
			 v == FP_NONE ? "no sensor id" : fp_vendor_names[v]);
	}
	mutex_unlock(&fp->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(gpio_test);

static void fp_spi_cs_timing(struct spi_device *spi)
{
	struct spi_delay *d[] = {
		&spi->cs_setup, &spi->cs_hold, &spi->cs_inactive,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(d); i++) {
		if (d[i]->value)
			continue;
		d[i]->value = FP_SPI_CS_DELAY_NS;
		d[i]->unit = SPI_DELAY_UNIT_NSECS;
	}
}

/*
 * Debug (root only):
 * "echo '<hz> [<spi mode 0-3> [<settle ms> [<cs setup us>]]]' > spi_test"
 * repeats the normal-world ID read with other bus parameters. It resets
 * the sensor, so only use it while the fingerprint HAL is stopped.
 */
static ssize_t spi_test_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mtk_fp *fp = g_fp;
	unsigned int hz, mode = 0, settle = 60, cs_us = 0;
	struct spi_delay old_setup;
	enum fp_vendor v;
	u32 old_mode;
	int ret;

	if (!fp)
		return -ENODEV;
	if (sscanf(buf, "%u %u %u %u", &hz, &mode, &settle, &cs_us) < 1 ||
	    !hz || hz > 8000000 || mode > 3 || settle > 1000 || cs_us > 600)
		return -EINVAL;

	mutex_lock(&fp->lock);
	ret = fp_power_locked(fp, true);
	if (!ret)
		ret = fp_clk_locked(fp, true);
	if (ret)
		goto out;

	old_mode = fp->spi->mode;
	old_setup = fp->spi->cs_setup;
	fp->spi->mode = (old_mode & ~(SPI_CPOL | SPI_CPHA)) | mode;
	if (cs_us) {
		fp->spi->cs_setup.value = cs_us;
		fp->spi->cs_setup.unit = SPI_DELAY_UNIT_USECS;
	}
	WRITE_ONCE(fp_spi_hz, hz);
	ret = spi_setup(fp->spi);
	if (!ret) {
		dev_info(dev, "spi test: %u Hz, mode %u, settle %u ms, cs setup %u us\n",
			 hz, mode, settle, cs_us);
		v = fp_detect_once(fp, settle);
		if (v == FP_NONE)
			fp_detect_failed(fp);
		else
			dev_info(dev, "spi test: %s sensor answers\n",
				 fp_vendor_names[v]);
	}
	WRITE_ONCE(fp_spi_hz, FP_SPI_DETECT_HZ);
	fp->spi->mode = old_mode;
	fp->spi->cs_setup = old_setup;
	spi_setup(fp->spi);
	fp_clk_locked(fp, false);
out:
	mutex_unlock(&fp->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(spi_test);

/* ----------------------------------------------------------------- probe */

static void fp_put_spi(void *data)
{
	put_device(data);
}

static int fp_get_resources(struct mtk_fp *fp)
{
	struct device *dev = fp->dev;
	struct device_node *np;
	struct device *sdev;
	int gpio, ret;

	np = of_find_compatible_node(NULL, NULL, "goodix,goodix-fp");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return dev_err_probe(dev, -ENODEV, "no spi sensor node\n");
	}
	sdev = bus_find_device_by_of_node(&spi_bus_type, np);
	of_node_put(np);
	if (!sdev)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, fp_put_spi, sdev);
	if (ret)
		return ret;
	fp->spi = to_spi_device(sdev);
	fp->spi_ctlr = fp->spi->controller->dev.parent;

	fp->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fp->pinctrl))
		return dev_err_probe(dev, PTR_ERR(fp->pinctrl), "pinctrl\n");
	fp->st_rst_h = pinctrl_lookup_state(fp->pinctrl, "reset_high");
	fp->st_rst_l = pinctrl_lookup_state(fp->pinctrl, "reset_low");
	if (IS_ERR(fp->st_rst_h) || IS_ERR(fp->st_rst_l))
		return dev_err_probe(dev, -EINVAL, "reset pin states\n");

	gpio = of_get_named_gpio(dev->of_node, "fpc,gpio_irq", 0);
	if (gpio < 0)
		return dev_err_probe(dev, gpio, "fpc,gpio_irq\n");
	ret = devm_gpio_request_one(dev, gpio, GPIOF_IN, "fp-irq");
	if (ret)
		return dev_err_probe(dev, ret, "irq gpio\n");
	fp->irq_gpio = gpio_to_desc(gpio);

	fp->irq = platform_get_irq(to_platform_device(dev), 0);
	if (fp->irq < 0)
		return fp->irq;

	/*
	 * VFP (MT6360 LDO1, regulator-name "vfp") is looked up by name like
	 * the stock driver did, so no supply phandle is needed in the DT.
	 */
	fp->vfp = devm_regulator_get_optional(dev, "vfp");
	if (IS_ERR(fp->vfp)) {
		ret = PTR_ERR(fp->vfp);
		fp->vfp = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "no vfp regulator (%d), assuming always on\n",
			 ret);
	} else {
		ret = regulator_set_voltage(fp->vfp, FP_VFP_UV, FP_VFP_UV);
		if (ret)
			dev_warn(dev, "vfp 3.0 V: %d\n", ret);
	}
	return 0;
}

static int fp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	enum fp_vendor want;
	struct mtk_fp *fp;
	bool vfp_was_on;
	int ret;

	if (!enable)
		return -ENODEV;

	ret = sysfs_match_string(fp_vendor_names, vendor ? vendor : "");
	if (ret < 0) {
		dev_warn(dev, "unknown vendor=%s, using auto\n", vendor);
		ret = FP_NONE;
	}
	want = ret;

	fp = devm_kzalloc(dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	mutex_init(&fp->lock);

	ret = fp_get_resources(fp);
	if (ret)
		return ret;

	mutex_lock(&fp->lock);
	vfp_was_on = fp->vfp && regulator_is_enabled(fp->vfp) > 0;
	ret = fp_power_locked(fp, true);
	if (!ret && vfp_was_on && vfp_cycle) {
		dev_info(dev, "vfp left on by the bootloader, power-cycling\n");
		fp_vfp_cycle_locked(fp, 50);
	}
	if (!ret)
		ret = fp_clk_locked(fp, true);
	mutex_unlock(&fp->lock);
	if (ret)
		goto err_power;

	fp->spi->mode = SPI_MODE_0;
	fp->spi->bits_per_word = 8;
	fp_spi_cs_timing(fp->spi);
	ret = spi_setup(fp->spi);
	if (ret)
		goto err_power;

	if (want == FP_NONE) {
		fp->vendor = fp_detect(fp);
		if (fp->vendor == FP_NONE && gpio_probe) {
			fp->vendor = fp_gpio_probe(fp);
			if (fp->vendor != FP_NONE)
				dev_warn(dev, "%s answers on GPIO but not through the spi5 host\n",
					 fp_vendor_names[fp->vendor]);
		}
		want = fp->vendor == FP_NONE ? FP_BOTH : fp->vendor;
	} else {
		fp_reset(fp, 15, 2);
	}

	mutex_lock(&fp->lock);
	fp_clk_locked(fp, false);
	mutex_unlock(&fp->lock);

	fp->ws = wakeup_source_register(dev, "fp_wakesrc");
	ret = devm_request_threaded_irq(dev, fp->irq, NULL, fp_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT |
					IRQF_NO_AUTOEN, "mtk-fingerprint", fp);
	if (ret)
		goto err_ws;
	enable_irq_wake(fp->irq);

	g_fp = fp;
	platform_set_drvdata(pdev, fp);

	/* uuid_fp defaults to the FPC TA; mirror that unless Goodix is known */
	memcpy(&uuid_fp, want == FP_GOODIX ? &uuid_ta_goodix : &uuid_ta_fpc,
	       sizeof(uuid_fp));
	fp->ta = want == FP_GOODIX ? FP_GOODIX : FP_FPC;

	if (want != FP_FPC) {
		ret = gf_setup(fp);
		if (ret)
			goto err_irq;
	}
	if (want != FP_GOODIX) {
		ret = fpc_setup(fp);
		if (ret)
			goto err_gf;
		mutex_lock(&fp->lock);
		fp_irq_locked(fp, true);
		mutex_unlock(&fp->lock);
	}

	if (device_create_file(dev, &dev_attr_spi_test))
		dev_warn(dev, "spi_test attribute unavailable\n");
	if (device_create_file(dev, &dev_attr_gpio_test))
		dev_warn(dev, "gpio_test attribute unavailable\n");

	dev_info(dev, "sensor %s, interfaces:%s%s\n",
		 fp->vendor == FP_NONE ? "unknown (HAL/TA detects)" :
		 fp_vendor_names[fp->vendor],
		 fp->gf_on ? " /dev/goodix_fp" : "",
		 fp->fpc_on ? " fpc-sysfs" : "");
	return 0;

err_gf:
	gf_teardown(fp);
err_irq:
	g_fp = NULL;
	disable_irq_wake(fp->irq);
	devm_free_irq(dev, fp->irq, fp);
err_ws:
	wakeup_source_unregister(fp->ws);
err_power:
	mutex_lock(&fp->lock);
	fp_clk_locked(fp, false);
	fp_power_locked(fp, false);
	mutex_unlock(&fp->lock);
	return ret;
}

static void fp_remove(struct platform_device *pdev)
{
	struct mtk_fp *fp = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_spi_test);
	device_remove_file(&pdev->dev, &dev_attr_gpio_test);
	gf_teardown(fp);
	if (fp->fpc_on)
		sysfs_remove_group(&fp->dev->kobj, &fpc_group);

	disable_irq_wake(fp->irq);
	mutex_lock(&fp->lock);
	fp_irq_locked(fp, false);
	while (fp->clk_ref)
		fp_clk_locked(fp, false);
	fp_power_locked(fp, false);
	mutex_unlock(&fp->lock);
	wakeup_source_unregister(fp->ws);
	g_fp = NULL;
}

static const struct of_device_id fp_of_match[] = {
	{ .compatible = "mediatek,fpc1022_irq" },
	{ }
};
MODULE_DEVICE_TABLE(of, fp_of_match);

static struct platform_driver fp_driver = {
	.probe = fp_probe,
	.remove = fp_remove,
	.driver = {
		.name = "mtk-tee-fingerprint",
		.of_match_table = fp_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init fp_init(void)
{
	return platform_driver_register(&fp_driver);
}
/* After the SPI host and the MT6360 regulators, like the stock driver. */
late_initcall(fp_init);

MODULE_DESCRIPTION("Goodix/FPC fingerprint glue for the Microtrust TEE");
MODULE_LICENSE("GPL");
