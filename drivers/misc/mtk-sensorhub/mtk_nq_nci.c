// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP PN553 NFC controller, /dev/nq-nci interface for the MT6853
 * (Redmi Note 9T).
 *
 * Minimal port of the vendor pn553 driver (drivers/nfc/pn553.c of the
 * stock 4.14 kernel): raw NCI frames over I2C through read()/write(),
 * plus the pn544 power-control ioctls that the NXP NCI HAL
 * (nfc_nci.nqx.default) uses. eSE/SPI arbitration is not implemented.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define NQ_MAX_BUF		512
#define NQ_NCI_HDR_LEN		3
#define NQ_RESET_POLLS		10
#define NQ_RESET_POLL_MS		10

#define PN544_MAGIC		0xE9
#define PN544_SET_PWR		_IOW(PN544_MAGIC, 0x01, unsigned int)
#define P61_SET_SPI_PWR		_IOW(PN544_MAGIC, 0x02, unsigned int)
#define P61_GET_PWR_STATUS	_IOR(PN544_MAGIC, 0x03, unsigned int)
#define P61_SET_WIRED_ACCESS	_IOW(PN544_MAGIC, 0x04, unsigned int)
#define P544_SET_NFC_SERVICE_PID _IOW(PN544_MAGIC, 0x05, unsigned int)
#define P544_GET_ESE_ACCESS	_IOW(PN544_MAGIC, 0x06, unsigned int)
#define P544_SET_POWER_SCHEME	_IOW(PN544_MAGIC, 0x07, unsigned int)
#define P544_REL_SVDD_WAIT	_IOW(PN544_MAGIC, 0x08, unsigned int)
#define PN544_SET_DWNLD_STATUS	_IOW(PN544_MAGIC, 0x09, unsigned int)
#define P544_REL_DWPONOFF_WAIT	_IOW(PN544_MAGIC, 0x0A, unsigned int)
#define P544_SECURE_TIMER_SESSION _IOW(PN544_MAGIC, 0x0B, unsigned int)
#define NFCC_INITIAL_CORE_RESET_NTF _IOW(PN544_MAGIC, 0x10, unsigned int)

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Bind the PN553 /dev/nq-nci driver");

struct nq_dev {
	struct i2c_client *client;
	struct miscdevice misc;
	struct gpio_desc *ven;
	struct gpio_desc *firm;
	struct gpio_desc *irq_gpio;
	wait_queue_head_t read_wq;
	struct mutex read_mutex;	/* one reader at a time */
	struct mutex ioctl_mutex;	/* power sequencing */
	spinlock_t irq_lock;		/* protects irq_enabled */
	bool irq_enabled;
	u8 buf_rd[NQ_MAX_BUF];
	u8 buf_wr[NQ_MAX_BUF];
};

static void nq_disable_irq(struct nq_dev *nq)
{
	unsigned long flags;

	spin_lock_irqsave(&nq->irq_lock, flags);
	if (nq->irq_enabled) {
		disable_irq_nosync(nq->client->irq);
		nq->irq_enabled = false;
	}
	spin_unlock_irqrestore(&nq->irq_lock, flags);
}

static void nq_enable_irq(struct nq_dev *nq)
{
	unsigned long flags;

	spin_lock_irqsave(&nq->irq_lock, flags);
	if (!nq->irq_enabled) {
		nq->irq_enabled = true;
		enable_irq(nq->client->irq);
	}
	spin_unlock_irqrestore(&nq->irq_lock, flags);
}

static irqreturn_t nq_irq_handler(int irq, void *data)
{
	struct nq_dev *nq = data;

	nq_disable_irq(nq);
	wake_up(&nq->read_wq);
	return IRQ_HANDLED;
}

static void nq_set_ven(struct nq_dev *nq, int v)
{
	gpiod_set_value_cansleep(nq->ven, v);
}

static void nq_set_firm(struct nq_dev *nq, int v)
{
	if (nq->firm)
		gpiod_set_value_cansleep(nq->firm, v);
}

static ssize_t nq_read(struct file *filp, char __user *buf, size_t count,
		       loff_t *off)
{
	struct nq_dev *nq = container_of(filp->private_data, struct nq_dev,
					 misc);
	int ret;

	count = min_t(size_t, count, NQ_MAX_BUF);

	mutex_lock(&nq->read_mutex);
	while (!gpiod_get_value_cansleep(nq->irq_gpio)) {
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		nq_enable_irq(nq);
		ret = wait_event_interruptible(nq->read_wq, !nq->irq_enabled);
		nq_disable_irq(nq);
		if (ret)
			goto out;
	}

	ret = i2c_master_recv(nq->client, nq->buf_rd, count);
	/* The PN553 needs a short pause between I2C transactions. */
	usleep_range(1000, 1100);
	if (ret < 0)
		goto out;
	if (ret > count) {
		ret = -EIO;
		goto out;
	}
	if (copy_to_user(buf, nq->buf_rd, ret))
		ret = -EFAULT;
out:
	mutex_unlock(&nq->read_mutex);
	return ret;
}

static ssize_t nq_write(struct file *filp, const char __user *buf,
			size_t count, loff_t *off)
{
	struct nq_dev *nq = container_of(filp->private_data, struct nq_dev,
					 misc);
	int ret;

	count = min_t(size_t, count, NQ_MAX_BUF);

	mutex_lock(&nq->ioctl_mutex);
	if (copy_from_user(nq->buf_wr, buf, count)) {
		ret = -EFAULT;
		goto out;
	}
	ret = i2c_master_send(nq->client, nq->buf_wr, count);
	if (ret != count && ret >= 0)
		ret = -EIO;
	usleep_range(1000, 1100);
out:
	mutex_unlock(&nq->ioctl_mutex);
	return ret;
}

static int nq_set_pwr(struct nq_dev *nq, unsigned long arg)
{
	switch (arg) {
	case 0:		/* power off */
		nq_set_firm(nq, 0);
		nq_set_ven(nq, 0);
		msleep(10);
		break;
	case 1:		/* power on */
		nq_set_firm(nq, 0);
		nq_set_ven(nq, 1);
		msleep(10);
		break;
	case 2:		/* firmware download mode */
		if (!nq->firm)
			return -EOPNOTSUPP;
		nq_set_ven(nq, 1);
		nq_set_firm(nq, 1);
		msleep(10);
		nq_set_ven(nq, 0);
		msleep(50);
		nq_set_ven(nq, 1);
		msleep(10);
		break;
	case 3:		/* eSE ISO reset */
		return -EOPNOTSUPP;
	case 4:
		if (!nq->firm)
			return -EOPNOTSUPP;
		nq_set_firm(nq, 1);
		break;
	case 5:		/* VEN reset pulse */
		nq_set_ven(nq, 0);
		msleep(10);
		nq_set_ven(nq, 1);
		msleep(10);
		break;
	case 6:
		if (!nq->firm)
			return -EOPNOTSUPP;
		nq_set_firm(nq, 0);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static long nq_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct nq_dev *nq = container_of(filp->private_data, struct nq_dev,
					 misc);
	long ret = 0;

	mutex_lock(&nq->ioctl_mutex);
	switch (cmd) {
	case PN544_SET_PWR:
		ret = nq_set_pwr(nq, arg);
		break;
	case P61_GET_PWR_STATUS:
	case PN544_SET_DWNLD_STATUS:
	case P61_SET_SPI_PWR:
	case P61_SET_WIRED_ACCESS:
	case P544_SET_NFC_SERVICE_PID:
	case P544_GET_ESE_ACCESS:
	case P544_SET_POWER_SCHEME:
	case P544_REL_SVDD_WAIT:
	case P544_REL_DWPONOFF_WAIT:
	case P544_SECURE_TIMER_SESSION:
	case NFCC_INITIAL_CORE_RESET_NTF:
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&nq->ioctl_mutex);
	return ret;
}

static const struct file_operations nq_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.read = nq_read,
	.write = nq_write,
	.unlocked_ioctl = nq_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct gpio_desc *nq_get_gpio(struct device *dev, const char *prop,
				     unsigned long flags, bool optional)
{
	int gpio, ret;

	gpio = of_get_named_gpio(dev->of_node, prop, 0);
	if (gpio < 0)
		return optional ? NULL : ERR_PTR(gpio);

	ret = devm_gpio_request_one(dev, gpio, flags, prop);
	if (ret)
		return optional ? NULL : ERR_PTR(ret);

	return gpio_to_desc(gpio);
}

static int nq_recv_reset_rsp(struct nq_dev *nq)
{
	u8 hdr[NQ_NCI_HDR_LEN], payload[3];
	int ret, i;

	for (i = 0; ; i++) {
		ret = gpiod_get_value_cansleep(nq->irq_gpio);
		if (ret < 0)
			return ret;
		if (ret)
			break;
		if (i == NQ_RESET_POLLS)
			return -ETIMEDOUT;
		msleep(NQ_RESET_POLL_MS);
	}

	ret = i2c_master_recv(nq->client, hdr, sizeof(hdr));
	if (ret != sizeof(hdr))
		return ret < 0 ? ret : -EIO;
	if (hdr[0] != 0x40 || hdr[1] != 0x00 ||
	    (hdr[2] != 1 && hdr[2] != sizeof(payload)))
		return -EPROTO;

	usleep_range(1000, 1100);
	ret = i2c_master_recv(nq->client, payload, hdr[2]);
	if (ret != hdr[2])
		return ret < 0 ? ret : -EIO;
	if (payload[0])
		return -EREMOTEIO;

	/* NCI 1.x carries version/config status; NCI 2.0 carries only status. */
	if (hdr[2] == sizeof(payload) &&
	    ((payload[1] != 0x10 && payload[1] != 0x11) || payload[2] > 1))
		return -EPROTO;

	return 0;
}

/* Verify a complete CORE_RESET_RSP, not just an acknowledged I2C write. */
static int nq_hw_check(struct nq_dev *nq)
{
	static const u8 core_reset[] = { 0x20, 0x00, 0x01, 0x00 };
	int ret;

	ret = i2c_master_send(nq->client, core_reset, sizeof(core_reset));
	if (ret != sizeof(core_reset)) {
		/* The controller may be stuck in download mode. */
		ret = nq_set_pwr(nq, 2);
		if (ret)
			return ret;
		nq_set_pwr(nq, 1);
		ret = i2c_master_send(nq->client, core_reset,
				      sizeof(core_reset));
		if (ret != sizeof(core_reset))
			return ret < 0 ? ret : -EIO;
	}

	ret = nq_recv_reset_rsp(nq);
	if (ret)
		return ret;

	/* Leave a freshly reset controller for the HAL's own NCI handshake. */
	return nq_set_pwr(nq, 5);
}

static int nq_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pinctrl *pinctrl;
	struct pinctrl_state *st;
	struct nq_dev *nq;
	int ret;

	if (!enable)
		return -ENODEV;

	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");

	nq = devm_kzalloc(dev, sizeof(*nq), GFP_KERNEL);
	if (!nq)
		return -ENOMEM;
	nq->client = client;
	init_waitqueue_head(&nq->read_wq);
	mutex_init(&nq->read_mutex);
	mutex_init(&nq->ioctl_mutex);
	spin_lock_init(&nq->irq_lock);

	nq->irq_gpio = nq_get_gpio(dev, "qcom,nq-irq", GPIOF_IN, false);
	if (IS_ERR(nq->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(nq->irq_gpio), "irq gpio\n");
	nq->ven = nq_get_gpio(dev, "qcom,nq-ven", GPIOF_OUT_INIT_LOW, false);
	if (IS_ERR(nq->ven))
		return dev_err_probe(dev, PTR_ERR(nq->ven), "ven gpio\n");
	nq->firm = nq_get_gpio(dev, "qcom,nq-firm", GPIOF_OUT_INIT_LOW, true);

	pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(pinctrl)) {
		st = pinctrl_lookup_state(pinctrl, "nfc_active");
		if (!IS_ERR(st))
			pinctrl_select_state(pinctrl, st);
	}

	ret = devm_request_irq(dev, client->irq, nq_irq_handler,
			       IRQF_NO_AUTOEN, "nq-nci", nq);
	if (ret)
		return dev_err_probe(dev, ret, "request irq\n");

	nq_set_pwr(nq, 1);
	ret = nq_hw_check(nq);
	if (ret) {
		nq_set_pwr(nq, 0);
		return dev_err_probe(dev, ret, "NCI reset check failed\n");
	}

	nq->misc.minor = MISC_DYNAMIC_MINOR;
	nq->misc.name = "nq-nci";
	nq->misc.fops = &nq_fops;
	nq->misc.parent = dev;
	ret = misc_register(&nq->misc);
	if (ret) {
		nq_set_pwr(nq, 0);
		return dev_err_probe(dev, ret, "misc register\n");
	}

	i2c_set_clientdata(client, nq);
	dev_info(dev, "NCI reset response verified; registered /dev/nq-nci\n");
	return 0;
}

static void nq_remove(struct i2c_client *client)
{
	struct nq_dev *nq = i2c_get_clientdata(client);

	misc_deregister(&nq->misc);
	nq_disable_irq(nq);
	nq_set_pwr(nq, 0);
}

static const struct of_device_id nq_of_match[] = {
	{ .compatible = "qcom,nq-nci" },
	{ }
};
MODULE_DEVICE_TABLE(of, nq_of_match);

static const struct i2c_device_id nq_id[] = {
	{ "nq-nci" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nq_id);

static struct i2c_driver nq_driver = {
	.probe = nq_probe,
	.remove = nq_remove,
	.id_table = nq_id,
	.driver = {
		.name = "mtk-nq-nci",
		.of_match_table = nq_of_match,
	},
};
module_i2c_driver(nq_driver);

MODULE_DESCRIPTION("NXP PN553 NFC /dev/nq-nci driver");
MODULE_LICENSE("GPL");
