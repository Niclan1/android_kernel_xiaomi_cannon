// SPDX-License-Identifier: GPL-2.0-only
/*
 * Camera EEPROM (cam_cal v2) for the stock MT6853 camera HAL.
 *
 * Port of the 4.14 drivers/misc/mediatek/cam_cal/src/common/v2 driver and
 * the mt6853 sensor list. One char device /dev/camera_eepromN is created
 * per "mediatek,camera_eeprom" I2C node named camera_eepromN. The HAL
 * (libfeatureiodrv_mem CamCalDrv) selects the sensor with
 * CAM_CALIOC_S_SENSOR_INFO and then uses lseek()+read().
 *
 * Probing touches no hardware; reads only happen on behalf of the HAL,
 * after it powered the sensor through /dev/kd_camera_hw. Writes are
 * refused so factory calibration data can never be overwritten.
 */

#define pr_fmt(fmt) "cam_cal: " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "kd_imgsensor.h"
#include "mtk_cam.h"

#define CAM_CAL_MAGIC		'i'

struct CAM_CAL_SENSOR_INFO {
	u32 sensor_id;
};

#define CAM_CALIOC_S_SENSOR_INFO \
	_IOW(CAM_CAL_MAGIC, 10, struct CAM_CAL_SENSOR_INFO)

#define DEV_NODE_NAME_PREFIX		"camera_eeprom"
#define MAX_EEPROM_NUMBER		5
#define DEFAULT_MAX_EEPROM_SIZE_8K	0x2000
#define MAX_EEPROM_SIZE_16K		0x4000
#define EEPROM_I2C_READ_MSG_LENGTH_MAX	1024
#define EEPROM_SEEK_END_POS		16192
#define OV02B10_OTP_SIZE		19

typedef unsigned int (*cam_cal_read_fn)(struct i2c_client *client,
					unsigned int addr,
					unsigned char *data,
					unsigned int size);

struct cam_cal_list_entry {
	u32 sensor_id;
	u32 slave_id;
	cam_cal_read_fn read;
	u32 max_size;
};

struct cam_cal_eeprom {
	dev_t devt;
	struct cdev cdev;
	struct device *dev;
	struct i2c_client *client;
	struct mutex lock;
	bool registered;
};

struct cam_cal_fd {
	struct cam_cal_eeprom *ee;
	u32 sensor_id;
};

static struct cam_cal_eeprom eeproms[MAX_EEPROM_NUMBER];
static dev_t cam_cal_devt_base;
static const struct class cam_cal_class = {
	.name = "camera_eeprom",
};

static int cam_cal_i2c_read(struct i2c_client *client, u16 addr,
			    u32 len, u8 *buf)
{
	u8 cmd[2] = { addr >> 8, addr & 0xff };
	struct i2c_msg msg[2] = {
		{
			.addr = client->addr,
			.flags = client->flags & I2C_M_TEN,
			.len = 2,
			.buf = cmd,
		}, {
			.addr = client->addr,
			.flags = (client->flags & I2C_M_TEN) | I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};

	return i2c_transfer(client->adapter, msg, 2) == 2 ? 0 : -EIO;
}

static unsigned int common_read_region(struct i2c_client *client,
				       unsigned int addr, unsigned char *data,
				       unsigned int size)
{
	unsigned int done = 0;

	while (done < size) {
		u32 n = min_t(u32, size - done, EEPROM_I2C_READ_MSG_LENGTH_MAX);

		if (cam_cal_i2c_read(client, addr + done, n, data + done))
			return 0;
		done += n;
	}
	return size;
}

/* The OV02B10 "EEPROM" is the sensor OTP cached by the sensor driver. */
unsigned int ov02b10ofilm_read_otp_info(struct i2c_client *client,
					unsigned int addr,
					unsigned char *data,
					unsigned int size);

static unsigned int ov02b10_otp_read_region(struct i2c_client *client,
					    unsigned int addr,
					    unsigned char *data,
					    unsigned int size)
{
	u8 otp[OV02B10_OTP_SIZE];

	if (addr >= OV02B10_OTP_SIZE || size > OV02B10_OTP_SIZE - addr)
		return 0;
	ov02b10ofilm_read_otp_info(client, 0, otp, OV02B10_OTP_SIZE);
	memcpy(data, otp + addr, size);
	return size;
}

/* from the 4.14 cam_cal/src/mt6853/cam_cal_list.c (Xiaomi cannon) */
static const struct cam_cal_list_entry cam_cal_list[] = {
	{ S5KGM1STOFILM_SENSOR_ID, 0xA0, common_read_region, 0 },
	{ S5K3L6OFILM_SENSOR_ID, 0xA2, common_read_region, 0 },
	{ HI259OFILM_SENSOR_ID, 0xA2, common_read_region, 0 },
	{ OV02B10OFILM_SENSOR_ID, 0x78, ov02b10_otp_read_region,
	  OV02B10_OTP_SIZE },
	{ OV8856OFILM_SENSOR_ID, 0xA0, common_read_region, 0 },
};

static const struct cam_cal_list_entry *cam_cal_lookup(u32 sensor_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cam_cal_list); i++)
		if (cam_cal_list[i].sensor_id == sensor_id)
			return &cam_cal_list[i];
	return NULL;
}

static unsigned int cam_cal_read_region(struct cam_cal_fd *fdd, u8 *buf,
					unsigned int offset, unsigned int size)
{
	const struct cam_cal_list_entry *e = cam_cal_lookup(fdd->sensor_id);
	struct i2c_client *client = fdd->ee->client;
	unsigned short dts_addr;
	unsigned int ret;

	mutex_lock(&fdd->ee->lock);
	if (e) {
		dts_addr = client->addr;
		client->addr = e->slave_id >> 1;
		ret = e->read(client, offset, buf, size);
		client->addr = dts_addr;
	} else {
		ret = common_read_region(client, offset, buf, size);
	}
	mutex_unlock(&fdd->ee->lock);
	return ret;
}

static int cam_cal_open(struct inode *inode, struct file *file)
{
	struct cam_cal_fd *fdd = kzalloc(sizeof(*fdd), GFP_KERNEL);

	if (!fdd)
		return -ENOMEM;
	fdd->ee = container_of(inode->i_cdev, struct cam_cal_eeprom, cdev);
	file->private_data = fdd;
	return 0;
}

static int cam_cal_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t cam_cal_read(struct file *file, char __user *ubuf,
			    size_t size, loff_t *ppos)
{
	struct cam_cal_fd *fdd = file->private_data;
	const struct cam_cal_list_entry *e = cam_cal_lookup(fdd->sensor_id);
	u32 limit = (e && e->max_size) ? e->max_size :
		    DEFAULT_MAX_EEPROM_SIZE_8K;
	loff_t pos = *ppos;
	u8 *kbuf;

	if (!size)
		return 0;
	if (pos < 0 || pos >= limit || size > limit - pos)
		return -EFAULT;

	kbuf = kmalloc(size, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (cam_cal_read_region(fdd, kbuf, pos, size) != size ||
	    copy_to_user(ubuf, kbuf, size)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	*ppos = pos + size;
	return size;
}

static ssize_t cam_cal_write(struct file *file, const char __user *ubuf,
			     size_t size, loff_t *ppos)
{
	pr_warn_ratelimited("%s: EEPROM write refused\n", current->comm);
	return -EPERM;
}

static loff_t cam_cal_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t pos;

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	case SEEK_END:
		pos = EEPROM_SEEK_END_POS + offset;
		break;
	default:
		return -EINVAL;
	}
	if (pos < 0)
		return -EINVAL;
	file->f_pos = pos;
	return pos;
}

static long cam_cal_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct cam_cal_fd *fdd = file->private_data;
	struct CAM_CAL_SENSOR_INFO info;

	if (cmd != CAM_CALIOC_S_SENSOR_INFO)
		return -EPERM;
	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;
	fdd->sensor_id = info.sensor_id;
	pr_debug("eeprom%u sensor id 0x%x\n",
		 MINOR(fdd->ee->devt), info.sensor_id);
	return 0;
}

static const struct file_operations cam_cal_fops = {
	.owner = THIS_MODULE,
	.open = cam_cal_open,
	.release = cam_cal_release,
	.read = cam_cal_read,
	.write = cam_cal_write,
	.llseek = cam_cal_llseek,
	.unlocked_ioctl = cam_cal_ioctl,
};

static int cam_cal_index(struct i2c_client *client, unsigned int *index)
{
	const char *name = client->dev.of_node ?
			   client->dev.of_node->name : "";
	size_t plen = strlen(DEV_NODE_NAME_PREFIX);

	if (strncmp(name, DEV_NODE_NAME_PREFIX, plen) ||
	    kstrtouint(name + plen, 10, index) ||
	    *index >= MAX_EEPROM_NUMBER) {
		dev_err(&client->dev, "bad node name %s\n", name);
		return -EINVAL;
	}
	return 0;
}

static int cam_cal_probe(struct i2c_client *client)
{
	struct cam_cal_eeprom *ee;
	unsigned int index;
	int ret;

	ret = cam_cal_index(client, &index);
	if (ret)
		return ret;
	ee = &eeproms[index];
	if (ee->registered)
		return -EBUSY;

	ee->devt = MKDEV(MAJOR(cam_cal_devt_base), index);
	ee->client = client;
	mutex_init(&ee->lock);
	cdev_init(&ee->cdev, &cam_cal_fops);
	ret = cdev_add(&ee->cdev, ee->devt, 1);
	if (ret)
		return ret;
	ee->dev = device_create(&cam_cal_class, &client->dev, ee->devt, NULL,
				DEV_NODE_NAME_PREFIX "%u", index);
	if (IS_ERR(ee->dev)) {
		cdev_del(&ee->cdev);
		return PTR_ERR(ee->dev);
	}
	ee->registered = true;
	i2c_set_clientdata(client, ee);
	dev_info(&client->dev, "/dev/" DEV_NODE_NAME_PREFIX "%u\n", index);
	return 0;
}

static void cam_cal_remove(struct i2c_client *client)
{
	struct cam_cal_eeprom *ee = i2c_get_clientdata(client);

	if (!ee || !ee->registered)
		return;
	device_destroy(&cam_cal_class, ee->devt);
	cdev_del(&ee->cdev);
	ee->registered = false;
}

static const struct of_device_id cam_cal_of_match[] = {
	{ .compatible = "mediatek,camera_eeprom" },
	{ }
};
MODULE_DEVICE_TABLE(of, cam_cal_of_match);

static struct i2c_driver cam_cal_driver = {
	.driver = {
		.name = "mtk-camera-eeprom",
		.of_match_table = cam_cal_of_match,
	},
	.probe = cam_cal_probe,
	.remove = cam_cal_remove,
};

static int __init cam_cal_init(void)
{
	int ret;

	if (!mtk_cam_hw_enabled()) {
		pr_info("disabled (mtk_cam.hw=0)\n");
		return 0;
	}

	ret = alloc_chrdev_region(&cam_cal_devt_base, 0, MAX_EEPROM_NUMBER,
				  DEV_NODE_NAME_PREFIX);
	if (ret)
		return ret;
	ret = class_register(&cam_cal_class);
	if (ret)
		goto err_region;
	ret = i2c_add_driver(&cam_cal_driver);
	if (ret)
		goto err_class;
	return 0;

err_class:
	class_unregister(&cam_cal_class);
err_region:
	unregister_chrdev_region(cam_cal_devt_base, MAX_EEPROM_NUMBER);
	pr_err("init failed: %d\n", ret);
	return ret;
}
late_initcall(cam_cal_init);
