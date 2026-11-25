// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up aid: mirror the kernel log to a raw region of a named partition
 * early in boot, so logs survive watchdog resets that do not retain DRAM.
 * Enabled only with "diag_blklog=<partition name>" on the command line.
 *
 * "diag_bcb=<secs>" arms the recovery boot-command in the misc partition for
 * the first <secs> seconds of uptime, so a boot that panics or hangs lands in
 * recovery by itself instead of looping until someone holds the keys. An
 * orderly reboot/poweroff disarms it (except "reboot recovery") unless
 * "diag_bcb_keep=1" is given.
 *
 * oops=panic is overridden (init sets panic_on_oops=1): an oops in a task is
 * logged and flushed instead of resetting the SoC before anything is saved.
 */
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdebug.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/io.h>
#include <linux/panic.h>
#include <linux/panic_notifier.h>
#include <linux/timekeeping.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#define DBL_OFFSET	(16ULL << 20)
#define DBL_SIZE	(2U << 20)
#define DBL_HDR		64
#define DBL_POFF	(20ULL << 20)
#define DBL_PSIZE	(4U << 20)
#define DBL_BCB_COMMAND_SIZE	32

static char dbl_part[PARTITION_META_INFO_VOLNAMELTH];

static int __init dbl_setup(char *s)
{
	strscpy(dbl_part, s, sizeof(dbl_part));
	return 1;
}
__setup("diag_blklog=", dbl_setup);

static unsigned int dbl_autoreboot;
static int __init dbl_autoreboot_setup(char *s)
{
	return kstrtouint(s, 0, &dbl_autoreboot) == 0;
}
__setup("diag_autoreboot=", dbl_autoreboot_setup);

static unsigned int dbl_bcb_secs;
static int __init dbl_bcb_setup(char *s)
{
	unsigned int secs;

	if (kstrtouint(s, 0, &secs) || !secs || secs > 3600)
		return 0;
	dbl_bcb_secs = secs;
	return 1;
}
__setup("diag_bcb=", dbl_bcb_setup);

/*
 * An orderly reboot/poweroff is not a hang: drop the armed recovery BCB so
 * the next boot is a normal one ("reboot recovery" keeps it). Set
 * diag_bcb_keep=1 to restore the old keep-until-timeout behaviour.
 */
static bool dbl_bcb_keep;
static int __init dbl_bcb_keep_setup(char *s)
{
	return kstrtobool(s, &dbl_bcb_keep) == 0;
}
__setup("diag_bcb_keep=", dbl_bcb_keep_setup);

/*
 * The crumb lands in TOPRGU NONRST_REG, which preloader/LK read back as the
 * saved WDT_STATUS (reset reason). Leave it off unless explicitly requested.
 */
static bool dbl_rgu_crumb;
static int __init dbl_rgu_crumb_setup(char *s)
{
	return kstrtobool(s, &dbl_rgu_crumb) == 0;
}
__setup("diag_rgu_crumb=", dbl_rgu_crumb_setup);

static int dbl_match(struct device *dev, const void *name)
{
	struct block_device *bdev = container_of(dev, struct block_device,
						 bd_device);

	return bdev_is_partition(bdev) && bdev->bd_meta_info &&
	       !strcmp(bdev->bd_meta_info->volname, name);
}

static char *dbl_buf;

static struct file *dbl_open_part(const char *part)
{
	struct device *dev;
	struct file *f;

	dev = class_find_device(&block_class, NULL, part, dbl_match);
	if (!dev)
		return NULL;
	f = bdev_file_open_by_dev(dev->devt, BLK_OPEN_READ | BLK_OPEN_WRITE,
				  NULL, NULL);
	put_device(dev);
	if (IS_ERR(f)) {
		pr_err_ratelimited("diag_blklog: open %s failed %ld\n", part,
				   PTR_ERR(f));
		return NULL;
	}
	return f;
}

static struct file *dbl_open(void)
{
	struct file *f = dbl_open_part(dbl_part);

	if (!f)
		return NULL;
	pr_info("diag_blklog: logging to %s@%llu\n", dbl_part, DBL_OFFSET);
	if (dbl_buf) {
		loff_t rpos = DBL_OFFSET, wpos = DBL_OFFSET + DBL_SIZE;

		if (kernel_read(f, dbl_buf, DBL_SIZE, &rpos) == DBL_SIZE &&
		    !memcmp(dbl_buf, "DIAGBLKLOG", 10)) {
			kernel_write(f, dbl_buf, DBL_SIZE, &wpos);
			pr_info("diag_blklog: saved previous log to slot 2\n");
		}
	}
	return f;
}

static bool dbl_bcb_ours;
static DEFINE_MUTEX(dbl_bcb_lock);

static bool dbl_bcb_matches(const char *command)
{
	static const char recovery_command[] = "boot-recovery";
	char expected[DBL_BCB_COMMAND_SIZE] = {};

	memcpy(expected, recovery_command, sizeof(recovery_command) - 1);
	return !memcmp(command, expected, sizeof(expected));
}

static void dbl_bcb_tick(unsigned int uptime)
{
	char command[DBL_BCB_COMMAND_SIZE];
	struct file *f;
	loff_t pos = 0;
	ssize_t ret;
	ssize_t write_ret;
	int sync_ret;

	if (!dbl_bcb_secs || uptime >= dbl_bcb_secs) {
		if (!dbl_bcb_ours)
			return;
	} else if (dbl_bcb_ours) {
		return;
	}

	f = dbl_open_part("misc");
	if (!f) {
		pr_warn_ratelimited("diag_blklog: misc unavailable for BCB update\n");
		return;
	}

	ret = kernel_read(f, command, sizeof(command), &pos);
	if (ret != sizeof(command)) {
		pr_warn_ratelimited("diag_blklog: misc BCB read failed %zd\n", ret);
		goto out;
	}

	if (!dbl_bcb_ours) {
		static const char recovery_command[] = "boot-recovery";

		if (memchr_inv(command, 0, sizeof(command))) {
			pr_warn_ratelimited("diag_blklog: BCB is not empty; leaving it unchanged\n");
			goto out;
		}

		memset(command, 0, sizeof(command));
		memcpy(command, recovery_command, sizeof(recovery_command) - 1);
		pos = 0;
		write_ret = kernel_write(f, command, sizeof(command), &pos);
		sync_ret = write_ret == sizeof(command) ? vfs_fsync(f, 0) : -EIO;
		pos = 0;
		ret = kernel_read(f, command, sizeof(command), &pos);
		if (ret == sizeof(command) && dbl_bcb_matches(command)) {
			dbl_bcb_ours = true;
			pr_info("diag_blklog: armed recovery BCB until %us uptime\n",
				dbl_bcb_secs);
			if (sync_ret)
				pr_err_ratelimited("diag_blklog: BCB fsync failed %d\n",
						   sync_ret);
		} else {
			pr_err_ratelimited("diag_blklog: BCB arm verification failed write=%zd fsync=%d read=%zd\n",
					   write_ret, sync_ret, ret);
		}
		goto out;
	}

	if (uptime < dbl_bcb_secs)
		goto out;

	if (!dbl_bcb_matches(command)) {
		if (!memchr_inv(command, 0, sizeof(command))) {
			sync_ret = vfs_fsync(f, 0);
			if (!sync_ret) {
				dbl_bcb_ours = false;
				pr_info("diag_blklog: BCB already cleared\n");
			} else {
				pr_err_ratelimited("diag_blklog: BCB clear fsync failed %d\n",
						   sync_ret);
			}
		} else {
			pr_warn("diag_blklog: BCB changed externally; not clearing it\n");
			dbl_bcb_ours = false;
		}
		goto out;
	}

	memset(command, 0, sizeof(command));
	pos = 0;
	write_ret = kernel_write(f, command, sizeof(command), &pos);
	sync_ret = write_ret == sizeof(command) ? vfs_fsync(f, 0) : -EIO;
	pos = 0;
	ret = kernel_read(f, command, sizeof(command), &pos);
	if (ret == sizeof(command) && !memchr_inv(command, 0, sizeof(command)) &&
	    !sync_ret) {
		dbl_bcb_ours = false;
		pr_info("diag_blklog: disarmed recovery BCB at %us uptime\n",
			uptime);
	} else {
		pr_err_ratelimited("diag_blklog: BCB clear verification failed read=%zd fsync=%d\n",
				   ret, sync_ret);
	}
out:
	bdev_fput(f);
}

static DEFINE_MUTEX(dbl_lock);
static DEFINE_MUTEX(dbl_plock);
static char *dbl_pbuf;
static size_t dbl_pused, dbl_pflushed;
static unsigned int dbl_pwraps, dbl_pwraps_flushed;
static void __iomem *dbl_rgu;

void diag_blklog_pmsg(const char __user *buf, size_t count)
{
	size_t n;

	if (!dbl_pbuf)
		return;
	mutex_lock(&dbl_plock);
	while (count) {
		n = min_t(size_t, count, DBL_PSIZE - DBL_HDR - dbl_pused);
		if (copy_from_user(dbl_pbuf + DBL_HDR + dbl_pused, buf, n))
			break;
		dbl_pused += n;
		buf += n;
		count -= n;
		if (dbl_pused == DBL_PSIZE - DBL_HDR) {
			dbl_pused = 0;
			dbl_pwraps++;
		}
	}
	mutex_unlock(&dbl_plock);
}

static void dbl_crumb(unsigned int code)
{
	if (dbl_rgu && dbl_rgu_crumb)
		writel(0xda000000 | ((ktime_get_boottime_seconds() & 0xffff) << 8) |
		       code, dbl_rgu + 0x20);
}

static bool dbl_pflush(struct file *f)
{
	loff_t pos, start;
	size_t used;
	ssize_t ret;

	mutex_lock(&dbl_plock);
	used = dbl_pused;
	if (used == dbl_pflushed && dbl_pwraps == dbl_pwraps_flushed) {
		mutex_unlock(&dbl_plock);
		return false;
	}
	snprintf(dbl_pbuf, DBL_HDR, "DIAGPMSG used=%zu wraps=%u\n", used,
		 dbl_pwraps);
	pos = DBL_POFF;
	ret = kernel_write(f, dbl_pbuf, DBL_HDR, &pos);
	if (ret != DBL_HDR) {
		pr_err_ratelimited("diag_blklog: pmsg header write failed %zd\n",
				   ret);
		mutex_unlock(&dbl_plock);
		return false;
	}
	start = dbl_pwraps != dbl_pwraps_flushed ? 0 :
		round_down(dbl_pflushed, 4096);
	if (dbl_pwraps != dbl_pwraps_flushed)
		used = DBL_PSIZE - DBL_HDR;
	pos = DBL_POFF + DBL_HDR + start;
	ret = kernel_write(f, dbl_pbuf + DBL_HDR + start, used - start, &pos);
	if (ret != used - start) {
		pr_err_ratelimited("diag_blklog: pmsg data write failed %zd\n",
				   ret);
		mutex_unlock(&dbl_plock);
		return false;
	}
	dbl_pflushed = dbl_pused;
	dbl_pwraps_flushed = dbl_pwraps;
	mutex_unlock(&dbl_plock);
	return true;
}
static struct file *dbl_file;
static unsigned int dbl_seq;
static u64 dbl_last_seq;
static DECLARE_WAIT_QUEUE_HEAD(dbl_waitq);
static atomic_t dbl_wakeup = ATOMIC_INIT(0);

static void dbl_flush(const char *why)
{
	struct kmsg_dump_iter iter = {};
	size_t len = 0;
	loff_t pos = DBL_OFFSET;
	u64 end_seq;
	bool force;
	bool wrote = false;
	ssize_t ret;
	int sync_ret;

	if (!why)
		why = "unknown";
	force = strcmp(why, "tick") != 0;
	mutex_lock(&dbl_lock);
	if (!dbl_part[0] || !dbl_buf)
		goto out;
	if (!dbl_file)
		dbl_file = dbl_open();
	if (dbl_file && dbl_buf) {
		kmsg_dump_rewind(&iter);
		end_seq = iter.next_seq;
		if (!force && end_seq == dbl_last_seq) {
			if (dbl_pbuf && dbl_pflush(dbl_file)) {
				sync_ret = vfs_fsync(dbl_file, 0);
				if (sync_ret)
					pr_err_ratelimited("diag_blklog: fsync failed %d\n",
							   sync_ret);
			}
			goto out;
		}

		memset(dbl_buf, 0, DBL_SIZE);
		kmsg_dump_get_buffer(&iter, true, dbl_buf + DBL_HDR,
				     DBL_SIZE - DBL_HDR, &len);
		snprintf(dbl_buf, DBL_HDR, "DIAGBLKLOG seq=%u len=%zu %s\n",
			 ++dbl_seq, len, why);
		ret = kernel_write(dbl_file, dbl_buf, DBL_SIZE, &pos);
		if (ret != DBL_SIZE) {
			pr_err_ratelimited("diag_blklog: log write failed %zd\n",
					   ret);
			goto out;
		}
		wrote = true;
		if (dbl_pbuf)
			wrote |= dbl_pflush(dbl_file);
		if (wrote) {
			sync_ret = vfs_fsync(dbl_file, 0);
			if (sync_ret)
				pr_err_ratelimited("diag_blklog: fsync failed %d\n",
						   sync_ret);
			else
				dbl_last_seq = end_seq;
		}
	}
out:
	mutex_unlock(&dbl_lock);
}

void diag_blklog_flush(const char *why)
{
	if (dbl_part[0] && dbl_buf) {
		dbl_flush(why);
		atomic_set(&dbl_wakeup, 1);
		wake_up(&dbl_waitq);
	}
}

static int dbl_reboot(struct notifier_block *nb, unsigned long action,
		      void *data)
{
	const char *cmd = data;

	pr_emerg("diag_blklog: reboot action %lu cmd %s by %s[%d]\n", action,
		 cmd ? cmd : "none", current->comm, current->pid);
	dbl_crumb(2);
	if (dbl_bcb_secs && !dbl_bcb_keep &&
	    !(action == SYS_RESTART && cmd && !strcmp(cmd, "recovery"))) {
		mutex_lock(&dbl_bcb_lock);
		/* uptime past the window makes the tick clear our BCB */
		dbl_bcb_tick(UINT_MAX);
		dbl_bcb_secs = 0;
		pr_emerg("diag_blklog: orderly shutdown, recovery BCB %s\n",
			 dbl_bcb_ours ? "still armed (clear failed)" : "disarmed");
		mutex_unlock(&dbl_bcb_lock);
	}
	dbl_flush("reboot");
	return NOTIFY_DONE;
}

static int dbl_panic(struct notifier_block *nb, unsigned long action,
		     void *data)
{
	dbl_crumb(1);
	return NOTIFY_DONE;
}

static struct notifier_block dbl_panic_nb = {
	.notifier_call = dbl_panic,
	.priority = INT_MAX,
};

static struct notifier_block dbl_reboot_nb = {
	.notifier_call = dbl_reboot,
	.priority = INT_MAX,
};

static int dbl_die(struct notifier_block *nb, unsigned long val, void *data)
{
	if (val == DIE_OOPS)
		panic_on_oops = 0;
	return NOTIFY_DONE;
}

static struct notifier_block dbl_die_nb = {
	.notifier_call = dbl_die,
	.priority = INT_MAX,
};

static int dbl_thread(void *unused)
{
	void __iomem *rgu = dbl_rgu;
	u64 uptime;
	u64 last_rgu = 0;
	unsigned int interval;

	while (!kthread_should_stop()) {
		uptime = ktime_get_boottime_seconds();
		if (rgu && uptime < 60 && uptime >= last_rgu + 3) {
			pr_info("diag_blklog: rgu mode=%08x len=%08x sta=%08x cnt=%08x 24=%08x\n",
				readl(rgu), readl(rgu + 4), readl(rgu + 0xc),
				readl(rgu + 0x514), readl(rgu + 0x24));
			last_rgu = uptime;
		}
		if (dbl_part[0] && dbl_buf)
			panic_on_oops = 0;
		dbl_flush("tick");
		mutex_lock(&dbl_bcb_lock);
		dbl_bcb_tick(uptime);
		mutex_unlock(&dbl_bcb_lock);
		if (dbl_autoreboot && uptime >= dbl_autoreboot) {
			pr_emerg("diag_blklog: autoreboot after %us\n",
				 dbl_autoreboot);
			dbl_flush("autoreboot");
			panic("diag: autoreboot");
		}
		interval = !dbl_file ? 100 :
			uptime < 60 ? 150 : uptime < 180 ? 500 : 2000;
		wait_event_timeout(dbl_waitq,
				   kthread_should_stop() ||
				   atomic_xchg(&dbl_wakeup, 0),
				   msecs_to_jiffies(interval));
	}
	return 0;
}

static int __init dbl_init(void)
{
	struct task_struct *task;

	if (!dbl_part[0] && !dbl_bcb_secs)
		return 0;

	if (dbl_part[0]) {
		dbl_buf = vmalloc(DBL_SIZE);
		if (!dbl_buf && !dbl_bcb_secs)
			return -ENOMEM;
		if (dbl_buf) {
			dbl_pbuf = vzalloc(DBL_PSIZE);
			dbl_rgu = ioremap(0x10007000, 0x1000);
			panic_on_oops = 0;
			if (register_die_notifier(&dbl_die_nb))
				pr_err("diag_blklog: die notifier registration failed\n");
			if (atomic_notifier_chain_register(&panic_notifier_list,
							   &dbl_panic_nb))
				pr_err("diag_blklog: panic notifier registration failed\n");
		} else
			pr_info("diag_blklog: expdb logging unavailable\n");
	}

	if ((dbl_buf || dbl_bcb_secs) && register_reboot_notifier(&dbl_reboot_nb))
		pr_err("diag_blklog: reboot notifier registration failed\n");

	task = kthread_run(dbl_thread, NULL, "diag_blklog");
	if (IS_ERR(task)) {
		pr_err("diag_blklog: thread creation failed %ld\n",
		       PTR_ERR(task));
		return PTR_ERR(task);
	}
	return 0;
}
late_initcall(dbl_init);
