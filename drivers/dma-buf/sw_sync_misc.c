// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/mdp_sync and /dev/fmt_sync for the MediaTek MDP and codec userspace.
 *
 * DpSync (libdpframework) and FmtSync (codec format converter) create the
 * release fences of their jobs on a timeline they get by opening these nodes.
 * Vendor kernels provided them from private copies of sw_sync; reuse the
 * sw_sync file operations instead, so each open gets its own timeline with
 * the same ioctl ABI.
 */
#include <linux/init.h>
#include <linux/miscdevice.h>

#include "sync_debug.h"

static struct miscdevice mdp_sync_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mdp_sync",
	.fops = &sw_sync_debugfs_fops,
};

static struct miscdevice fmt_sync_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "fmt_sync",
	.fops = &sw_sync_debugfs_fops,
};

static int __init mdp_sync_init(void)
{
	int ret;

	ret = misc_register(&mdp_sync_dev);
	if (ret)
		return ret;

	ret = misc_register(&fmt_sync_dev);
	if (ret)
		misc_deregister(&mdp_sync_dev);

	return ret;
}
device_initcall(mdp_sync_init);
