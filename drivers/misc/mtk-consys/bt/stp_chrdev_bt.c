// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/stpbt for the MT6853 consys: HCI over the WMT/STP core.
 *
 * Adapted from the MediaTek 4.14 bt/mt66xx/stp_chrdev_bt.c (Copyright (c)
 * 2019 MediaTek Inc.). The userspace ABI (node name, H4 byte stream, ioctls,
 * power on first open and off on close) is unchanged so that the vendor
 * Bluetooth HAL keeps working. Nothing touches the hardware at init time:
 * the chip is only powered through WMT when the node is opened.
 */

#define pr_fmt(fmt) "mtk-stpbt: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>

#include "wmt_exp.h"
#include "stp_exp.h"
#include "bluetooth_drv_init.h"

#define COMBO_IOC_MAGIC			0xb0
#define COMBO_IOCTL_FW_ASSERT		_IOW(COMBO_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_BT_SET_PSM		_IOW(COMBO_IOC_MAGIC, 1, bool)
#define COMBO_IOCTL_BT_IC_HW_VER	_IOR(COMBO_IOC_MAGIC, 2, void *)
#define COMBO_IOCTL_BT_IC_FW_VER	_IOR(COMBO_IOC_MAGIC, 3, void *)
#define COMBO_IOCTL_BT_HOST_DEBUG	_IOW(COMBO_IOC_MAGIC, 4, void *)

#define BT_BUFFER_SIZE		2048
#define BT_HOST_DEBUG_SIZE	32

/*
 * Whole chip reset state:
 * 0 normal, 1 reset started, 2 reset done and the Hardware Error event is
 * still to be delivered, 3 Hardware Error event delivered.
 */
#define RST_NONE	0
#define RST_START	1
#define RST_END		2
#define RST_REPORTED	3

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Create /dev/stpbt when the WMT core is initialised (0 = never)");

static const u8 hci_evt_hw_error[] = { 0x04, 0x10, 0x01, 0x00 };

static DEFINE_MUTEX(open_lock);
static DEFINE_MUTEX(rd_lock);
static DEFINE_MUTEX(wr_lock);
static DECLARE_WAIT_QUEUE_HEAD(rd_wq);
static struct wakeup_source *bt_ws;
static bool bt_on;
static unsigned int rstflag;
static size_t rd_offset;
static bool registered;

static u8 i_buf[BT_BUFFER_SIZE];
static u8 o_buf[BT_BUFFER_SIZE];

static bool bt_rx_empty(void)
{
	return mtk_wcn_stp_is_rxqueue_empty(BT_TASK_INDX);
}

static bool bt_readable(void)
{
	unsigned int rst = READ_ONCE(rstflag);

	if (rst == RST_END)
		return true;
	if (rst != RST_NONE)
		return false;
	return !bt_rx_empty();
}

static void bt_event_cb(void)
{
	/* Keep the system awake until the HAL thread has read the packet */
	if (bt_ws)
		__pm_wakeup_event(bt_ws, 100);
	wake_up_interruptible(&rd_wq);
}

static void bt_reset_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			ENUM_WMTMSG_TYPE_T type, PVOID buf, UINT32 sz)
{
	ENUM_WMTRSTMSG_TYPE_T msg;

	if (sz != sizeof(msg) || !buf)
		return;
	if (src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT ||
	    type != WMTMSG_TYPE_RESET)
		return;

	memcpy(&msg, buf, sizeof(msg));
	switch (msg) {
	case WMTRSTMSG_RESET_START:
		pr_info("whole chip reset start\n");
		WRITE_ONCE(rstflag, RST_START);
		break;
	case WMTRSTMSG_RESET_END:
	case WMTRSTMSG_RESET_END_FAIL:
		pr_info("whole chip reset %s\n",
			msg == WMTRSTMSG_RESET_END ? "end" : "failed");
		rd_offset = 0;
		WRITE_ONCE(rstflag, RST_END);
		wake_up_interruptible(&rd_wq);
		break;
	default:
		break;
	}
}

static __poll_t stpbt_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(filp, &rd_wq, wait);
	/* Re-check after queueing: the wake up may have come before poll_wait */
	if (bt_readable())
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static ssize_t stpbt_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	size_t count = iov_iter_count(from);
	ssize_t ret;

	if (!count)
		return 0;
	if (count > BT_BUFFER_SIZE)
		return -EINVAL;

	mutex_lock(&wr_lock);
	if (READ_ONCE(rstflag) != RST_NONE) {
		ret = -EIO;
		goto out;
	}
	if (copy_from_iter(o_buf, count, from) != count) {
		ret = -EFAULT;
		goto out;
	}

	ret = mtk_wcn_stp_send_data(o_buf, count, BT_TASK_INDX);
	if (ret == 0) {
		/* STP tx ring full: the caller retries */
		pr_info_ratelimited("tx queue full, %zu bytes not sent\n", count);
		ret = -EAGAIN;
	}
out:
	mutex_unlock(&wr_lock);
	return ret;
}

static ssize_t stpbt_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *f_pos)
{
	ssize_t ret;

	if (!count)
		return 0;
	if (count > BT_BUFFER_SIZE)
		count = BT_BUFFER_SIZE;

	mutex_lock(&rd_lock);
	for (;;) {
		unsigned int rst = READ_ONCE(rstflag);

		if (rst == RST_END) {
			size_t left = sizeof(hci_evt_hw_error) - rd_offset;

			ret = min(count, left);
			memcpy(i_buf, hci_evt_hw_error + rd_offset, ret);
			rd_offset += ret;
			if (rd_offset == sizeof(hci_evt_hw_error)) {
				rd_offset = 0;
				WRITE_ONCE(rstflag, RST_REPORTED);
			}
			break;
		}

		if (rst == RST_NONE) {
			ret = mtk_wcn_stp_receive_data(i_buf, count, BT_TASK_INDX);
			if (ret != 0)
				break;
		}

		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		ret = wait_event_interruptible(rd_wq, bt_readable());
		if (ret)
			goto out;
	}

	if (ret < 0)
		goto out;
	if (copy_to_user(buf, i_buf, ret)) {
		if (READ_ONCE(rstflag) == RST_REPORTED)
			WRITE_ONCE(rstflag, RST_END);
		ret = -EFAULT;
	}
out:
	mutex_unlock(&rd_lock);
	return ret;
}

static long stpbt_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	u8 dbg[BT_HOST_DEBUG_SIZE];
	u32 ver;

	switch (cmd) {
	case COMBO_IOCTL_FW_ASSERT:
		if (READ_ONCE(rstflag) != RST_NONE)
			return -EBUSY;
		pr_info("host triggers firmware assert, reason %lu\n", arg & 0xffff);
		if (mtk_wcn_wmt_assert(WMTDRV_TYPE_BT, arg & 0xffff) !=
		    MTK_WCN_BOOL_TRUE)
			return -EBUSY;
		return 0;
	case COMBO_IOCTL_BT_SET_PSM:
		return mtk_wcn_wmt_psm_ctrl((MTK_WCN_BOOL)arg);
	case COMBO_IOCTL_BT_IC_HW_VER:
		ver = mtk_wcn_wmt_ic_info_get(WMTCHIN_HWVER);
		break;
	case COMBO_IOCTL_BT_IC_FW_VER:
		ver = mtk_wcn_wmt_ic_info_get(WMTCHIN_FWVER);
		break;
	case COMBO_IOCTL_BT_HOST_DEBUG:
		/* Low power debug info of the stack; not used by this port */
		if (copy_from_user(dbg, (void __user *)arg, sizeof(dbg)))
			return -EFAULT;
		return 0;
	default:
		return -EOPNOTSUPP;
	}

	if (copy_to_user((u32 __user *)arg, &ver, sizeof(ver)))
		return -EFAULT;
	return 0;
}

static int stpbt_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&open_lock);
	if (bt_on) {
		pr_warn("already open\n");
		ret = -EIO;
		goto out;
	}

	pr_info("open (pid %d)\n", current->pid);
	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		pr_err("WMT turn on BT failed\n");
		ret = -EIO;
		goto out;
	}

	if (mtk_wcn_stp_is_ready() == MTK_WCN_BOOL_FALSE) {
		pr_err("STP is not ready\n");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		ret = -EIO;
		goto out;
	}

	/* Plain STP byte stream for the vendor HAL, not the BlueZ HCI mode */
	mtk_wcn_stp_set_bluez(0);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, bt_event_cb);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, bt_reset_cb);

	rd_offset = 0;
	WRITE_ONCE(rstflag, RST_NONE);
	bt_on = true;
	pr_info("WMT turn on BT ok, STP ready\n");
out:
	mutex_unlock(&open_lock);
	return ret;
}

static int stpbt_release(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&open_lock);
	if (!bt_on)
		goto out;

	pr_info("close (pid %d)\n", current->pid);
	bt_on = false;
	WRITE_ONCE(rstflag, RST_NONE);
	rd_offset = 0;

	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		pr_err("WMT turn off BT failed\n");
		ret = -EIO;
	} else {
		pr_info("WMT turn off BT ok\n");
	}
out:
	mutex_unlock(&open_lock);
	return ret;
}

static const struct file_operations stpbt_fops = {
	.owner = THIS_MODULE,
	.open = stpbt_open,
	.release = stpbt_release,
	.read = stpbt_read,
	.write_iter = stpbt_write_iter,
	.unlocked_ioctl = stpbt_ioctl,
	.compat_ioctl = stpbt_ioctl,
	.poll = stpbt_poll,
};

static struct miscdevice stpbt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "stpbt",
	.fops = &stpbt_fops,
};

/* Called from the WMT core once the connectivity drivers are brought up */
int mtk_wcn_stpbt_drv_init(void)
{
	int ret;

	if (!enable) {
		pr_info("disabled by mtk_stpbt.enable=0\n");
		return 0;
	}
	if (registered)
		return 0;

	bt_ws = wakeup_source_register(NULL, "bt_drv");
	if (!bt_ws)
		pr_warn("wakeup source registration failed\n");

	ret = misc_register(&stpbt_misc);
	if (ret) {
		pr_err("misc_register failed (%d)\n", ret);
		if (bt_ws)
			wakeup_source_unregister(bt_ws);
		bt_ws = NULL;
		return ret;
	}

	registered = true;
	pr_info("/dev/stpbt registered\n");
	return 0;
}
