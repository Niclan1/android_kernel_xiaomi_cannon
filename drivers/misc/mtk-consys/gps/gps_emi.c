// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/gps_emi for the MT6853 consys: the GNSS share of the connsys EMI.
 *
 * Adapted from the MediaTek 4.14 gps/gps_stp/gps_emi.c (Copyright (c)
 * MediaTek Inc.) as built for MT6853 in the stock cannon kernel. The ioctl
 * numbers, return values and read semantics are unchanged for mnld:
 *
 *  1 EMI_MEMORY_INIT       map the GNSS window, returns 1
 *  2 MNL_NVRAM_FILE_TO_MEM no-op, returns 0
 *  3 MNL_NVRAM_MEM_TO_FILE no-op, returns 0
 *  4 ADC_CAPTURE_ADDR_GET  32-bit physical base of the GNSS window
 *
 * The window is consys EMI + 3 MiB + 64 KiB, 0xF0000 bytes, which ends
 * exactly at the end of the 4 MiB consys reserved-memory region. Unlike
 * stock it is mapped once, bounds checked against that region and unmapped
 * on the last close. Stock also programmed EMI MPU region 29 to "no
 * protection" for the AP and CONN domains over this window; the consys
 * platform code already sets region 27 that way over the whole consys
 * region (which contains the window), so no further MPU region is
 * programmed here.
 */

#define pr_fmt(fmt) "mtk-gps-emi: " fmt

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "connectivity_build_in_adapter.h"
#include "gps_drv_init.h"

#define IOCTL_EMI_MEMORY_INIT		1
#define IOCTL_MNL_NVRAM_FILE_TO_MEM	2
#define IOCTL_MNL_NVRAM_MEM_TO_FILE	3
#define IOCTL_ADC_CAPTURE_ADDR_GET	4

#define GPS_EMI_BASE_ADDR_OFFSET	(3 * SZ_1M + 0x10000)
#define GPS_EMI_MPU_SIZE		0xF0000
#define GPS_ADC_CAPTURE_BUFF_SIZE	0x50000
#define GPS_EMI_READ_CHUNK		SZ_4K

static DEFINE_MUTEX(emi_lock);
static void __iomem *emi_va;
static phys_addr_t emi_pa;
static unsigned int open_count;
static bool registered;

/* Returns the window base, or 0 if it does not fit the consys region */
static phys_addr_t gps_emi_window(void)
{
	phys_addr_t base = READ_ONCE(gConEmiPhyBase);
	unsigned long long size = READ_ONCE(gConEmiSize);

	if (!base || size < GPS_EMI_BASE_ADDR_OFFSET + GPS_EMI_MPU_SIZE)
		return 0;
	if (base + GPS_EMI_BASE_ADDR_OFFSET + GPS_EMI_MPU_SIZE - 1 < base)
		return 0;
	/* mnld only takes a 32-bit address from ADC_CAPTURE_ADDR_GET */
	if (upper_32_bits(base + GPS_EMI_BASE_ADDR_OFFSET + GPS_EMI_MPU_SIZE - 1))
		return 0;
	return base + GPS_EMI_BASE_ADDR_OFFSET;
}

static long gps_emi_map(void)
{
	phys_addr_t pa;

	lockdep_assert_held(&emi_lock);
	if (emi_va)
		return 1;
	pa = gps_emi_window();
	if (!pa) {
		pr_err("consys EMI %pa size %llx cannot hold the GNSS window\n",
		       &gConEmiPhyBase, gConEmiSize);
		return -ENODEV;
	}
	emi_va = ioremap(pa, GPS_EMI_MPU_SIZE);
	if (!emi_va) {
		pr_err("ioremap %pa failed\n", &pa);
		return -ENOMEM;
	}
	emi_pa = pa;
	pr_info("GNSS EMI window %pa size 0x%x mapped\n", &pa, GPS_EMI_MPU_SIZE);
	return 1;
}

static void gps_emi_unmap(void)
{
	lockdep_assert_held(&emi_lock);
	if (emi_va) {
		iounmap(emi_va);
		emi_va = NULL;
		emi_pa = 0;
	}
}

static long gps_emi_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	u32 pa32;
	long ret;

	switch (cmd) {
	case IOCTL_EMI_MEMORY_INIT:
		mutex_lock(&emi_lock);
		ret = gps_emi_map();
		mutex_unlock(&emi_lock);
		return ret;
	case IOCTL_ADC_CAPTURE_ADDR_GET:
		mutex_lock(&emi_lock);
		pa32 = lower_32_bits(emi_pa);
		mutex_unlock(&emi_lock);
		/* Stock returns -1 on a bad user pointer */
		return copy_to_user((u32 __user *)arg, &pa32, sizeof(pa32)) ?
		       -EPERM : 0;
	case IOCTL_MNL_NVRAM_FILE_TO_MEM:
	case IOCTL_MNL_NVRAM_MEM_TO_FILE:
	default:
		/* Stock accepts every other command as a no-op */
		return 0;
	}
}

/*
 * Stock copies up to 0x50000 bytes of the window to the caller and then
 * returns 0; mnld relies on the buffer contents, not the return value.
 */
static ssize_t gps_emi_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	size_t done = 0, n;
	void *bounce;
	ssize_t ret = 0;

	count = min_t(size_t, count, GPS_ADC_CAPTURE_BUFF_SIZE);
	bounce = kmalloc(GPS_EMI_READ_CHUNK, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	mutex_lock(&emi_lock);
	if (!emi_va)
		goto out;
	while (done < count) {
		n = min_t(size_t, count - done, GPS_EMI_READ_CHUNK);
		memcpy_fromio(bounce, emi_va + done, n);
		if (copy_to_user(buf + done, bounce, n)) {
			pr_info("copy to user failed\n");
			break;
		}
		done += n;
	}
out:
	mutex_unlock(&emi_lock);
	kfree(bounce);
	return ret;
}

static ssize_t gps_emi_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	return 0;
}

static int gps_emi_open(struct inode *inode, struct file *file)
{
	mutex_lock(&emi_lock);
	open_count++;
	mutex_unlock(&emi_lock);
	return nonseekable_open(inode, file);
}

static int gps_emi_release(struct inode *inode, struct file *file)
{
	mutex_lock(&emi_lock);
	if (!WARN_ON_ONCE(!open_count) && !--open_count)
		gps_emi_unmap();
	mutex_unlock(&emi_lock);
	return 0;
}

static const struct file_operations gps_emi_fops = {
	.owner = THIS_MODULE,
	.open = gps_emi_open,
	.release = gps_emi_release,
	.read = gps_emi_read,
	.write = gps_emi_write,
	.unlocked_ioctl = gps_emi_ioctl,
	.compat_ioctl = gps_emi_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice gps_emi_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gps_emi",
	.fops = &gps_emi_fops,
};

int mtk_wcn_gps_emi_init(void)
{
	int ret;

	if (registered)
		return 0;
	ret = misc_register(&gps_emi_misc);
	if (ret) {
		pr_err("misc_register failed (%d)\n", ret);
		return ret;
	}
	registered = true;
	pr_info("/dev/gps_emi registered\n");
	return 0;
}
