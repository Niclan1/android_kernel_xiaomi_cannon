// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/gps_pwr for the MT6853 consys: low-battery throttling level for mnld.
 *
 * Adapted from the MediaTek 4.14 gps/gps_pwr/gps_pwr.c (Copyright (c)
 * MediaTek Inc.). open/release report the GNSS driver on/off to the
 * connsys power throttling core, read returns the current level as an int
 * and poll signals a new level.
 *
 * The stock core (power_throttling/core.c, conn_pwr_arbitrate()) only
 * computes levels for the MT6637 A-die and leaves every radio at
 * CONN_PWR_THR_LV_0 otherwise; this board has an MT6631 A-die. This file
 * carries the subset of that core the GNSS path uses: per-driver on/off
 * status, the level table and the event callback, so a level change
 * reaches mnld exactly as on stock.
 */

#define pr_fmt(fmt) "mtk-gps-pwr: " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "gps_drv_init.h"

enum conn_pwr_drv_type {
	CONN_PWR_DRV_BT = 0,
	CONN_PWR_DRV_FM = 1,
	CONN_PWR_DRV_GPS = 2,
	CONN_PWR_DRV_WIFI = 3,
	CONN_PWR_DRV_MAX
};

enum conn_pwr_low_battery_level {
	CONN_PWR_THR_LV_0 = 0,
	CONN_PWR_THR_LV_5 = 5,
	CONN_PWR_LOW_BATTERY_MAX
};

enum conn_pwr_event_type {
	CONN_PWR_EVENT_LEVEL = 0,
	CONN_PWR_EVENT_MAX_TEMP = 1,
	CONN_PWR_EVENT_MAX
};

/* --- stock conn_pwr subset ---------------------------------------------- */

static DEFINE_SPINLOCK(conn_pwr_lock);
static bool conn_pwr_drv_on[CONN_PWR_DRV_MAX];
static int conn_pwr_level[CONN_PWR_DRV_MAX];

static int conn_pwr_drv_pre_on(enum conn_pwr_drv_type type, int *level)
{
	if (type < 0 || type >= CONN_PWR_DRV_MAX)
		return -1;
	spin_lock(&conn_pwr_lock);
	conn_pwr_drv_on[type] = true;
	*level = conn_pwr_level[type];
	spin_unlock(&conn_pwr_lock);
	return 0;
}

static int conn_pwr_drv_post_off(enum conn_pwr_drv_type type)
{
	if (type < 0 || type >= CONN_PWR_DRV_MAX)
		return -1;
	spin_lock(&conn_pwr_lock);
	conn_pwr_drv_on[type] = false;
	spin_unlock(&conn_pwr_lock);
	return 0;
}

/* --- /dev/gps_pwr -------------------------------------------------------- */

static DECLARE_WAIT_QUEUE_HEAD(gps_pwr_wq);
static DEFINE_MUTEX(gps_pwr_lock);
static int battery_level;
static bool has_new_level;
static unsigned int open_count;
static bool registered;

/*
 * Stock conn_pwr_notify_event() target for DRV_TYPE_GPS. Level changes are
 * only produced by the MT6637 arbitration, so on this board it is only
 * reached through mtk_wcn_gps_pwr_set_level().
 */
static int gps_pwr_msg_cb(enum conn_pwr_event_type type, void *data)
{
	int level;

	if (type != CONN_PWR_EVENT_LEVEL || !data)
		return 0;
	level = *(int *)data;
	if (level < CONN_PWR_THR_LV_0 || level > CONN_PWR_THR_LV_5)
		return -EINVAL;
	spin_lock(&conn_pwr_lock);
	conn_pwr_level[CONN_PWR_DRV_GPS] = level;
	battery_level = level;
	WRITE_ONCE(has_new_level, true);
	spin_unlock(&conn_pwr_lock);
	wake_up_interruptible(&gps_pwr_wq);
	return 0;
}

int mtk_wcn_gps_pwr_set_level(int level)
{
	return gps_pwr_msg_cb(CONN_PWR_EVENT_LEVEL, &level);
}

static int gps_pwr_open(struct inode *inode, struct file *file)
{
	int level = CONN_PWR_THR_LV_0;
	int ret;

	mutex_lock(&gps_pwr_lock);
	if (!open_count++) {
		ret = conn_pwr_drv_pre_on(CONN_PWR_DRV_GPS, &level);
		spin_lock(&conn_pwr_lock);
		battery_level = level;
		WRITE_ONCE(has_new_level, true);
		spin_unlock(&conn_pwr_lock);
		pr_info("pre_on ret %d level %d\n", ret, level);
	}
	mutex_unlock(&gps_pwr_lock);
	return nonseekable_open(inode, file);
}

static int gps_pwr_release(struct inode *inode, struct file *file)
{
	int ret;

	mutex_lock(&gps_pwr_lock);
	if (!WARN_ON_ONCE(!open_count) && !--open_count) {
		ret = conn_pwr_drv_post_off(CONN_PWR_DRV_GPS);
		pr_info("post_off ret %d\n", ret);
	}
	mutex_unlock(&gps_pwr_lock);
	return 0;
}

/* Stock always copies a full int, whatever count is */
static ssize_t gps_pwr_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	int level;

	spin_lock(&conn_pwr_lock);
	level = battery_level;
	spin_unlock(&conn_pwr_lock);
	if (count < sizeof(level))
		return -EINVAL;
	if (copy_to_user(buf, &level, sizeof(level)))
		return -EFAULT;
	WRITE_ONCE(has_new_level, false);
	return sizeof(level);
}

static ssize_t gps_pwr_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	return 0;
}

static __poll_t gps_pwr_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &gps_pwr_wq, wait);
	return READ_ONCE(has_new_level) ? EPOLLIN | EPOLLRDNORM : 0;
}

static long gps_pwr_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	/* Stock gps_pwr defines no ioctls and accepts all as no-ops */
	return 0;
}

static const struct file_operations gps_pwr_fops = {
	.owner = THIS_MODULE,
	.open = gps_pwr_open,
	.release = gps_pwr_release,
	.read = gps_pwr_read,
	.write = gps_pwr_write,
	.poll = gps_pwr_poll,
	.unlocked_ioctl = gps_pwr_ioctl,
	.compat_ioctl = gps_pwr_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice gps_pwr_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gps_pwr",
	.fops = &gps_pwr_fops,
};

int mtk_wcn_gps_pwr_init(void)
{
	int ret;

	if (registered)
		return 0;
	ret = misc_register(&gps_pwr_misc);
	if (ret) {
		pr_err("misc_register failed (%d)\n", ret);
		return ret;
	}
	registered = true;
	pr_info("/dev/gps_pwr registered\n");
	return 0;
}
