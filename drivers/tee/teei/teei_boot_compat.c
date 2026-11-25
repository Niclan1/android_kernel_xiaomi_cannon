// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal MediaTek /sys/class/BOOT/BOOT/boot/{boot_mode,boot_type}
 * compatibility for vendor userspace.
 *
 * The Microtrust teei_daemon reads boot_type to choose the RPMB backend
 * (1: eMMC /dev/mmcblk0rpmb, otherwise UFS /dev/block/sdc) and fails all
 * RPMB accesses when the file is missing.
 */

#define pr_fmt(fmt) "teei-boot: " fmt

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/of.h>
#include <linux/sysfs.h>

#define BOOTDEV_UFS	2
#define NORMAL_BOOT	0

struct tag_bootmode {
	u32 size;
	u32 tag;
	u32 bootmode;
	u32 boottype;
};

static u32 teei_boot_mode = NORMAL_BOOT;
static u32 teei_boot_type = BOOTDEV_UFS;

static ssize_t boot_mode_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", teei_boot_mode);
}

static ssize_t boot_type_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", teei_boot_type);
}

static struct kobj_attribute boot_mode_attr = __ATTR_RO(boot_mode);
static struct kobj_attribute boot_type_attr = __ATTR_RO(boot_type);

static struct attribute *boot_attrs[] = {
	&boot_mode_attr.attr,
	&boot_type_attr.attr,
	NULL,
};

static const struct attribute_group boot_attr_group = {
	.attrs = boot_attrs,
};

static void __init teei_boot_parse_atag(void)
{
	const struct tag_bootmode *tag;
	struct device_node *np;
	int len;

	np = of_find_node_by_path("/chosen");
	if (!np)
		return;

	tag = of_get_property(np, "atag,boot", &len);
	if (tag && len >= sizeof(*tag)) {
		teei_boot_mode = tag->bootmode;
		if (tag->boottype <= BOOTDEV_UFS)
			teei_boot_type = tag->boottype;
	} else {
		pr_info("no atag,boot in /chosen, assuming UFS normal boot\n");
	}
	of_node_put(np);
}

static int __init teei_boot_compat_init(void)
{
	struct class *cls;
	struct device *dev;
	struct kobject *kobj;
	int ret;

	teei_boot_parse_atag();

	cls = class_create("BOOT");
	if (IS_ERR(cls))
		return PTR_ERR(cls);

	dev = device_create(cls, NULL, MKDEV(0, 0), NULL, "BOOT");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_class;
	}

	kobj = kobject_create_and_add("boot", &dev->kobj);
	if (!kobj) {
		ret = -ENOMEM;
		goto err_dev;
	}

	ret = sysfs_create_group(kobj, &boot_attr_group);
	if (ret)
		goto err_kobj;

	pr_info("boot_mode=%u boot_type=%u\n", teei_boot_mode, teei_boot_type);
	return 0;

err_kobj:
	kobject_put(kobj);
err_dev:
	device_unregister(dev);
err_class:
	class_destroy(cls);
	return ret;
}
device_initcall(teei_boot_compat_init);
