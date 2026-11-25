// SPDX-License-Identifier: GPL-2.0
/*
 * MT6853 connectivity subsystem (consys) port, stage 1.
 *
 * Provides the /dev/wmtdetect node that the MediaTek userspace loader
 * (/vendor/bin/wmt_loader) opens first at boot. The ioctl numbers and the
 * meaning of their return values follow the vendor common_detect/wmt_detect.[ch],
 * so the unmodified loader can run its detection handshake and publish
 * vendor.connsys.driver.ready.
 *
 * This stage never touches the connectivity hardware: no register access, no
 * regulators, clocks, power domain or EMI reservation. The A-die cannot be
 * identified without powering the chip up, so GET_ADIE_CHIP_ID reports 0 (not
 * probed). The WMT/STP core and the /dev/stpwmt, /dev/stpbt and /dev/wmtWifi
 * nodes belong to later stages.
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#define pr_fmt(fmt) "mtk-consys: " fmt

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/printk.h>
#include <linux/sched.h>

#if IS_ENABLED(CONFIG_MTK_CONSYS_WMT)
#include "wmt/mtk-consys-hw.h"
#else
static inline bool mtk_consys_hw_detect_active(void)
{
	return false;
}
#endif

#define MTK_CONSYS_SOC_CHIPID		0x6853

#define WMT_DETECT_IOC_MAGIC		'w'
#define COMBO_IOCTL_GET_CHIP_ID		_IOR(WMT_DETECT_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_SET_CHIP_ID		_IOW(WMT_DETECT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_EXT_CHIP_DETECT	_IOR(WMT_DETECT_IOC_MAGIC, 2, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID	_IOR(WMT_DETECT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT	_IOR(WMT_DETECT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP	_IOR(WMT_DETECT_IOC_MAGIC, 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON	_IOR(WMT_DETECT_IOC_MAGIC, 6, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_OFF	_IOR(WMT_DETECT_IOC_MAGIC, 7, int)
#define COMBO_IOCTL_DO_SDIO_AUDOK	_IOR(WMT_DETECT_IOC_MAGIC, 8, int)
#define COMBO_IOCTL_GET_ADIE_CHIP_ID	_IOR(WMT_DETECT_IOC_MAGIC, 9, int)
#define COMBO_IOCTL_CONNSYS_SOC_HW_INIT	_IOR(WMT_DETECT_IOC_MAGIC, 10, int)

/* Chip id last stored by userspace; -1 until SET_CHIP_ID is called. */
static atomic_t consys_chipid = ATOMIC_INIT(-1);

/*
 * The arguments are plain integers, not user pointers, so the same handler
 * serves native and compat callers. Negative results are returned as -errno
 * and show up as -1 in the caller, which is what the loader checks for.
 */
static long mtk_consys_detect_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	const char *name;
	long ret;

	switch (cmd) {
	case COMBO_IOCTL_GET_CHIP_ID:
		name = "GET_CHIP_ID";
		ret = atomic_read(&consys_chipid);
		if (ret < 0)
			ret = -ENODATA;
		break;
	case COMBO_IOCTL_SET_CHIP_ID:
		name = "SET_CHIP_ID";
		atomic_set(&consys_chipid, (int)arg);
		ret = 0;
		break;
	case COMBO_IOCTL_CONNSYS_SOC_HW_INIT:
		/* The vendor driver registers the consys platform driver here. */
		name = "CONNSYS_SOC_HW_INIT";
		ret = 0;
		break;
	case COMBO_IOCTL_EXT_CHIP_PWR_ON:
		/* Must fail: tells the loader there is no external combo chip. */
		name = "EXT_CHIP_PWR_ON";
		ret = -ENODEV;
		break;
	case COMBO_IOCTL_EXT_CHIP_DETECT:
		name = "EXT_CHIP_DETECT";
		ret = -ENODEV;
		break;
	case COMBO_IOCTL_DO_SDIO_AUDOK:
		name = "DO_SDIO_AUDOK";
		ret = -ENODEV;
		break;
	case COMBO_IOCTL_EXT_CHIP_PWR_OFF:
		name = "EXT_CHIP_PWR_OFF";
		ret = 0;
		break;
	case COMBO_IOCTL_GET_SOC_CHIP_ID:
		name = "GET_SOC_CHIP_ID";
		ret = MTK_CONSYS_SOC_CHIPID;
		break;
	case COMBO_IOCTL_GET_ADIE_CHIP_ID:
		/* Must not be negative or the loader never sets driver.ready. */
		name = "GET_ADIE_CHIP_ID";
		ret = 0;
		break;
	case COMBO_IOCTL_MODULE_CLEANUP:
		name = "MODULE_CLEANUP";
		ret = 0;
		break;
	case COMBO_IOCTL_DO_MODULE_INIT:
		/* No WMT/STP/BT/Wi-Fi core to initialise at this stage. */
		name = "DO_MODULE_INIT";
		ret = 0;
		break;
	default:
		pr_warn_ratelimited("%s: unknown ioctl 0x%x\n", current->comm, cmd);
		return -ENOTTY;
	}

	if (ret < 0)
		pr_info("%s: %s(%#lx) = %pe\n", current->comm, name, arg,
			ERR_PTR(ret));
	else
		pr_info("%s: %s(%#lx) = %#lx\n", current->comm, name, arg, ret);
	return ret;
}

static const struct file_operations mtk_consys_detect_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mtk_consys_detect_ioctl,
	.compat_ioctl = mtk_consys_detect_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice mtk_consys_detect_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wmtdetect",
	.fops = &mtk_consys_detect_fops,
	.mode = 0660,
};

static int __init mtk_consys_init(void)
{
	struct device_node *np, *rmem_np;
	struct reserved_mem *rmem;
	int ret;

	/* Stay silent and inert on anything that is not an MT6853 consys. */
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6853-consys");
	if (!np)
		return -ENODEV;

	ret = of_device_is_available(np) ? 0 : -ENODEV;
	of_node_put(np);
	if (ret)
		return ret;

	/* Report where the consys EMI region landed; it is not mapped here. */
	rmem_np = of_find_compatible_node(NULL, NULL,
					  "mediatek,consys-reserve-memory");
	rmem = rmem_np ? of_reserved_mem_lookup(rmem_np) : NULL;
	if (rmem)
		pr_info("consys EMI region %pa + %pa\n", &rmem->base, &rmem->size);
	else
		pr_warn("consys EMI reserved memory not found\n");
	of_node_put(rmem_np);

	/* With mtk_consys.hw=1 the vendor detect driver owns the node name. */
	if (mtk_consys_hw_detect_active()) {
		pr_info("stage 1: vendor /dev/wmtdetect active, not registering ours\n");
		return 0;
	}

	ret = misc_register(&mtk_consys_detect_dev);
	if (ret) {
		pr_err("cannot register /dev/wmtdetect: %d\n", ret);
		return ret;
	}

	pr_info("stage 1: /dev/wmtdetect ready, hardware untouched\n");
	return 0;
}
device_initcall(mtk_consys_init);
