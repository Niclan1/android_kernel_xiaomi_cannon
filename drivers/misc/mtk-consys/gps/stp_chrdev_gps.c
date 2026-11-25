// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/stpgps for the MT6853 consys: GNSS NMEA/binary stream over WMT/STP.
 *
 * Adapted from the MediaTek 4.14 gps/gps_stp/stp_chrdev_gps.c (Copyright (c)
 * MediaTek Inc.) as built into the stock cannon kernel for MT6853: no L5
 * /dev/stpgps2, no LNA pin driver, GPS_FWCTL/HW suspend enabled and the
 * FWCTL ioctl disabled. Node name, read/write semantics and ioctl numbers
 * are unchanged for the vendor mnld. The chip is only powered through WMT
 * on the first open and powered down again on the last close.
 */

#define pr_fmt(fmt) "mtk-stpgps: " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "wmt_exp.h"
#include "stp_exp.h"
#include "gps_drv_init.h"

#define COMBO_IOC_GPS_IC_HW_VERSION	7
#define COMBO_IOC_GPS_IC_FW_VERSION	8
#define COMBO_IOC_D1_EFUSE_GET		9
#define COMBO_IOC_RTC_FLAG		10
#define COMBO_IOC_CO_CLOCK_FLAG		11
#define COMBO_IOC_TRIGGER_WMT_ASSERT	12
#define COMBO_IOC_WMT_STATUS		13
#define COMBO_IOC_TAKE_GPS_WAKELOCK	14
#define COMBO_IOC_GIVE_GPS_WAKELOCK	15
#define COMBO_IOC_GET_GPS_LNA_PIN	16
#define COMBO_IOC_GPS_FWCTL		17
#define COMBO_IOC_GPS_HW_SUSPEND	18
#define COMBO_IOC_GPS_HW_RESUME		19
#define COMBO_IOC_GPS_LISTEN_RST_EVT	20
#define COMBO_IOC_GPS_GET_MD_STATUS	21

/* Values mnld expects from COMBO_IOC_WMT_STATUS */
#define GPS_WMT_STATUS_RESETTING	(-888)
#define GPS_WMT_STATUS_RESET_DONE	(-889)

#define GPS_FWCTL_OPCODE_ENTER_STOP_MODE	4
#define GPS_FWCTL_OPCODE_EXIT_STOP_MODE		5
#define GPS_HW_SUSPEND_RX_LEN			3
#define GPS_CTRL_WAKE_FOR_MVCD			6

#define STP_GPS_BUFFER_SIZE	MTKSTP_BUFFER_SIZE

#define RST_NONE	0
#define RST_START	1
#define RST_END		2

enum gps_ctrl_status {
	GPS_CLOSED,
	GPS_OPENED,
	GPS_SUSPENDED,
	GPS_RESET_START,
	GPS_RESET_DONE,
};

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Create /dev/stpgps when the WMT core is initialised (0 = never)");

static DEFINE_MUTEX(open_lock);
static DEFINE_MUTEX(rd_lock);
static DEFINE_MUTEX(wr_lock);
static DEFINE_MUTEX(status_lock);
static DECLARE_WAIT_QUEUE_HEAD(rd_wq);
static DECLARE_WAIT_QUEUE_HEAD(rst_wq);
static struct wakeup_source *gps_ws;
static bool wake_held;
static unsigned int open_count;
static bool rx_flag;
static int rstflag;
static bool rst_or_close;
static bool fwctl_ready;
static enum gps_ctrl_status ctrl_status;
static bool registered;

static u8 i_buf[STP_GPS_BUFFER_SIZE];
static u8 o_buf[STP_GPS_BUFFER_SIZE];

static void gps_status_set(enum gps_ctrl_status to)
{
	mutex_lock(&status_lock);
	ctrl_status = to;
	if (to == GPS_RESET_START || to == GPS_RESET_DONE || to == GPS_CLOSED) {
		WRITE_ONCE(rst_or_close, true);
		wake_up_interruptible(&rst_wq);
	}
	mutex_unlock(&status_lock);
}

static bool gps_status_move(enum gps_ctrl_status from, enum gps_ctrl_status to)
{
	bool ok;

	mutex_lock(&status_lock);
	ok = ctrl_status == from;
	if (ok)
		ctrl_status = to;
	mutex_unlock(&status_lock);
	if (!ok)
		pr_warn("unexpected status %d, not changed from %d to %d\n",
			ctrl_status, from, to);
	return ok;
}

static void gps_hold_wake_lock(bool hold)
{
	mutex_lock(&status_lock);
	if (hold && !wake_held) {
		if (gps_ws)
			__pm_stay_awake(gps_ws);
		wake_held = true;
	} else if (!hold && wake_held) {
		if (gps_ws)
			__pm_relax(gps_ws);
		wake_held = false;
	}
	mutex_unlock(&status_lock);
}

static void gps_event_cb(void)
{
	WRITE_ONCE(rx_flag, true);
	wake_up_interruptible(&rd_wq);
}

static void gps_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
		       ENUM_WMTMSG_TYPE_T type, void *buf, unsigned int sz)
{
	ENUM_WMTRSTMSG_TYPE_T msg = 0;

	if (!buf || sz > sizeof(msg)) {
		pr_warn("invalid reset message size %u\n", sz);
		return;
	}
	memcpy(&msg, buf, sz);
	if (src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_GPS ||
	    type != WMTMSG_TYPE_RESET)
		return;

	switch (msg) {
	case WMTRSTMSG_RESET_START:
		pr_info("whole chip reset start\n");
		WRITE_ONCE(rstflag, RST_START);
		WRITE_ONCE(fwctl_ready, false);
		gps_status_set(GPS_RESET_START);
		break;
	case WMTRSTMSG_RESET_END:
	case WMTRSTMSG_RESET_END_FAIL:
		pr_info("whole chip reset %s\n",
			msg == WMTRSTMSG_RESET_END ? "end" : "fail");
		WRITE_ONCE(rstflag, RST_END);
		gps_status_set(GPS_RESET_DONE);
		break;
	default:
		break;
	}
}

static ssize_t gps_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	size_t len = min_t(size_t, count, STP_GPS_BUFFER_SIZE);
	int written;
	ssize_t ret;

	if (!count)
		return -EFAULT;

	mutex_lock(&wr_lock);
	if (copy_from_user(o_buf, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	written = mtk_wcn_stp_send_data(o_buf, len, GPS_TASK_INDX);
	if (written <= 0) {
		/* No STP window space; mnld retries after a delay */
		pr_err("write of %zu bytes failed (%d)\n", count, written);
		ret = -ENOSPC;
	} else {
		ret = written;
	}
out:
	mutex_unlock(&wr_lock);
	return ret;
}

static ssize_t gps_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	ssize_t ret;
	int got;

	if (count > STP_GPS_BUFFER_SIZE)
		count = STP_GPS_BUFFER_SIZE;

	mutex_lock(&rd_lock);
	if (READ_ONCE(rstflag) == RST_START && (filp->f_flags & O_NONBLOCK)) {
		ret = -EIO;
		goto out;
	}

	for (;;) {
		WRITE_ONCE(rx_flag, false);
		got = mtk_wcn_stp_receive_data(i_buf, count, GPS_TASK_INDX);
		if (got > 0)
			break;
		if (got < 0) {
			ret = -EIO;
			goto out;
		}
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		if (wait_event_interruptible(rd_wq, READ_ONCE(rx_flag))) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	ret = copy_to_user(buf, i_buf, got) ? -EFAULT : got;
out:
	mutex_unlock(&rd_lock);
	return ret;
}

static int gps_hw_suspend_ctrl(bool to_suspend, u8 mode)
{
	u8 tx[2] = { to_suspend ? GPS_FWCTL_OPCODE_ENTER_STOP_MODE :
				  GPS_FWCTL_OPCODE_EXIT_STOP_MODE, mode };
	u8 rx[GPS_HW_SUSPEND_RX_LEN] = { 0 };
	UINT32 rx_len = 0;
	INT32 st;

	if (!READ_ONCE(fwctl_ready))
		return -EINVAL;
	st = mtk_wmt_gps_mcu_ctrl(tx, sizeof(tx), rx, sizeof(rx), &rx_len);
	if (st || rx_len < 2 || rx[0]) {
		/* mode 2 asks firmware to wake for MVCD instead of suspending */
		if (rx[0] == GPS_CTRL_WAKE_FOR_MVCD && mode == 2)
			gps_status_move(GPS_SUSPENDED, GPS_OPENED);
		pr_warn("hw suspend ctrl %d mode %u: st=%d rx_len=%u rx0=%u\n",
			to_suspend, mode, st, rx_len, rx[0]);
		return -EINVAL;
	}
	pr_info("hw suspend ctrl %d mode %u done\n", to_suspend, mode);
	return 0;
}

static long gps_hw_suspend(u8 mode)
{
	if (READ_ONCE(ctrl_status) != GPS_OPENED) {
		pr_warn("hw suspend: status %d not opened\n", ctrl_status);
		return 0;
	}
	if (gps_hw_suspend_ctrl(true, mode))
		return -EINVAL;
	if (!mtk_wmt_gps_l1_suspend_ctrl(MTK_WCN_BOOL_TRUE))
		pr_warn("WMT GPS L1 suspend failed\n");
	/* Clears the GPS STP queue; the reset callback stays registered */
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, NULL);
	gps_hold_wake_lock(false);
	gps_status_move(GPS_OPENED, GPS_SUSPENDED);
	return 0;
}

static long gps_hw_resume(u8 mode)
{
	if (READ_ONCE(ctrl_status) != GPS_SUSPENDED) {
		pr_warn("hw resume: status %d not suspended\n", ctrl_status);
		return 0;
	}
	gps_hold_wake_lock(true);
	if (!mtk_wmt_gps_l1_suspend_ctrl(MTK_WCN_BOOL_FALSE))
		pr_warn("WMT GPS L1 resume failed\n");
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, gps_event_cb);
	if (gps_hw_suspend_ctrl(false, mode))
		return -EINVAL;
	gps_status_move(GPS_SUSPENDED, GPS_OPENED);
	return 0;
}

static long gps_listen_rst(void)
{
	return wait_event_interruptible(rst_wq, READ_ONCE(rst_or_close));
}

static long gps_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	UINT32 val;
	long ret = 0;

	switch (cmd) {
	case 0:	/* STP is owned by WMT */
	case 1:	/* raw data path is not exposed */
		return -EINVAL;
	case COMBO_IOC_GPS_IC_HW_VERSION:
		val = mtk_wcn_wmt_ic_info_get(WMTCHIN_HWVER);
		return copy_to_user(uarg, &val, sizeof(val)) ? -EFAULT : 0;
	case COMBO_IOC_GPS_IC_FW_VERSION:
		val = mtk_wcn_wmt_ic_info_get(WMTCHIN_FWVER);
		return copy_to_user(uarg, &val, sizeof(val)) ? -EFAULT : 0;
	case COMBO_IOC_RTC_FLAG:
		/* Stock returns "no RTC low power" for this platform */
		return 0;
	case COMBO_IOC_CO_CLOCK_FLAG:
		return mtk_wcn_wmt_co_clock_flag_get();
	case COMBO_IOC_D1_EFUSE_GET:
		/* Not an MT6735/MT6763 efuse platform; stock returns 0 */
		return 0;
	case COMBO_IOC_TRIGGER_WMT_ASSERT:
		pr_info("host triggered firmware assert, reason %lu\n", arg);
		return mtk_wcn_wmt_assert(WMTDRV_TYPE_GPS, (UINT32)arg) ==
		       MTK_WCN_BOOL_TRUE ? 0 : -EBUSY;
	case COMBO_IOC_WMT_STATUS:
		switch (READ_ONCE(rstflag)) {
		case RST_START:
			return GPS_WMT_STATUS_RESETTING;
		case RST_END:
			return GPS_WMT_STATUS_RESET_DONE;
		default:
			return 0;
		}
	case COMBO_IOC_TAKE_GPS_WAKELOCK:
		gps_hold_wake_lock(true);
		return 0;
	case COMBO_IOC_GIVE_GPS_WAKELOCK:
		gps_hold_wake_lock(false);
		return 0;
	case COMBO_IOC_GPS_HW_SUSPEND:
		mutex_lock(&open_lock);
		ret = gps_hw_suspend(arg & 0xff);
		mutex_unlock(&open_lock);
		return ret;
	case COMBO_IOC_GPS_HW_RESUME:
		mutex_lock(&open_lock);
		ret = gps_hw_resume(arg & 0xff);
		mutex_unlock(&open_lock);
		return ret;
	case COMBO_IOC_GPS_LISTEN_RST_EVT:
		return gps_listen_rst();
	case COMBO_IOC_GET_GPS_LNA_PIN:
		val = mtk_wmt_get_gps_lna_pin_num();
		return copy_to_user(uarg, &val, sizeof(val)) ? -EFAULT : 0;
	case COMBO_IOC_GPS_GET_MD_STATUS:
		/* No b13b14-status-addr on this board, same as stock */
	case COMBO_IOC_GPS_FWCTL:
		/* GPS_FWCTL_IOCTL_SUPPORT is not built in the stock driver */
	default:
		return -EFAULT;
	}
}

static int gps_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	if (READ_ONCE(rstflag) == RST_START) {
		pr_warn("whole chip resetting\n");
		return -EPERM;
	}

	mutex_lock(&open_lock);
	if (open_count) {
		open_count++;
		goto out;
	}

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_GPS) == MTK_WCN_BOOL_FALSE) {
		pr_warn("WMT turn on GPS failed\n");
		ret = -ENODEV;
		goto out;
	}
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_GPS, gps_rst_cb);
	WRITE_ONCE(rstflag, RST_NONE);

	if (!mtk_wcn_stp_is_ready()) {
		pr_err("STP is not ready\n");
		mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_GPS);
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_GPS);
		ret = -ENODEV;
		goto out;
	}
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, gps_event_cb);
	gps_hold_wake_lock(true);
	WRITE_ONCE(fwctl_ready, true);
	WRITE_ONCE(rst_or_close, false);
	gps_status_set(GPS_OPENED);
	open_count = 1;
	pr_info("GPS on (pid %d)\n", current->pid);
out:
	mutex_unlock(&open_lock);
	return ret;
}

static int gps_release(struct inode *inode, struct file *file)
{
	mutex_lock(&open_lock);
	if (WARN_ON_ONCE(!open_count) || --open_count)
		goto out;

	WRITE_ONCE(fwctl_ready, false);
	if (READ_ONCE(rstflag) == RST_START)
		pr_warn("whole chip resetting, GPS stays registered for WMT\n");
	else if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_GPS) == MTK_WCN_BOOL_FALSE)
		pr_warn("WMT turn off GPS failed\n");
	else
		WRITE_ONCE(rstflag, RST_NONE);
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_GPS);
	gps_hold_wake_lock(false);
	gps_status_set(GPS_CLOSED);
	pr_info("GPS off (pid %d)\n", current->pid);
out:
	mutex_unlock(&open_lock);
	return 0;
}

static const struct file_operations stpgps_fops = {
	.owner = THIS_MODULE,
	.open = gps_open,
	.release = gps_release,
	.read = gps_read,
	.write = gps_write,
	.unlocked_ioctl = gps_ioctl,
	.compat_ioctl = gps_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice stpgps_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "stpgps",
	.fops = &stpgps_fops,
};

/* Called from the WMT core once the connectivity drivers are brought up */
int mtk_wcn_stpgps_drv_init(void)
{
	int ret;

	if (!enable) {
		pr_info("disabled by mtk_stpgps.enable=0\n");
		return 0;
	}
	if (registered)
		return 0;

	gps_ws = wakeup_source_register(NULL, "gpswakelock");
	if (!gps_ws)
		pr_warn("wakeup source registration failed\n");

	ret = misc_register(&stpgps_misc);
	if (ret) {
		pr_err("misc_register failed (%d)\n", ret);
		wakeup_source_unregister(gps_ws);
		gps_ws = NULL;
		return ret;
	}

	registered = true;
	pr_info("/dev/stpgps registered\n");
	return 0;
}
