// SPDX-License-Identifier: GPL-2.0
/*
 * Boot from a built-in DTB while importing the bootloader's runtime data.
 *
 * MediaTek LK ignores the DTB in the boot image and always passes its own
 * vendor SoC tree (plus the dtbo overlay). That tree targets the vendor
 * kernel, so the kernel carries its own tree and only takes the per-boot
 * information LK generates: /chosen, memory, reserved-memory carve-outs,
 * nodes LK creates from scratch and a few properties LK patches at runtime.
 */

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/setup.h>

static u8 builtin_fdt_buf[SZ_512K] __aligned(SZ_4K);

/* Properties LK fills in at runtime on nodes that exist in the base tree. */
static const char *const lk_runtime_props[][2] __initconst = {
	{ "/sleep@10006000", "spmfw_version" },
	{ "/mddriver", "ccci,modem_info_v2" },
	{ "/emicen@10219000", "rk_size" },
	{ "/emicen@10219000", "rk_cnt" },
	{ "/emicen@10219000", "ch_cnt" },
	{ "/emiisu", "con_addr" },
	{ "/emiisu", "ver_addr" },
	{ "/emiisu", "buf_addr" },
	{ "/emiisu", "buf_size" },
	{ "/emimpu@10226000", "dram_start" },
	{ "/emimpu@10226000", "dram_end" },
	{ "/dramc@10230000", "dram_type" },
	{ "/dramc@10230000", "support_ch_cnt" },
	{ "/dramc@10230000", "ch_cnt" },
	{ "/dramc@10230000", "rk_cnt" },
	{ "/dramc@10230000", "mr_cnt" },
	{ "/dramc@10230000", "freq_cnt" },
	{ "/dramc@10230000", "rk_size" },
	{ "/dramc@10230000", "mr" },
	{ "/dramc@10230000", "freq_step" },
	{ "/scp@10500000", "status" },
	{ "/adsp_common@10800000", "load" },
	{ "/adsp_common@10800000", "adsp-rsv-audio" },
	{ "/adsp_core0@10820000", "sysram" },
	{ "/adsp_core0@10820000", "sysram_size" },
	{ "/vpu_core0@19030000", "bin-phy-addr" },
	{ "/vpu_core0@19030000", "bin-size" },
	{ "/vpu_core0@19030000", "img-head" },
	{ "/vpu_core0@19030000", "pre-bin" },
	{ "/vpu_core0@19030000", "algo" },
	{ "/vpu_core0@19030000", "iram-data" },
	{ "/vpu_core1@19031000", "iram-data" },
};

static bool __init is_phandle_prop(const char *name)
{
	return !strcmp(name, "phandle") || !strcmp(name, "linux,phandle");
}

/*
 * Copy all properties of @snode onto @dnode. Phandles are only copied onto
 * newly created nodes and only when the value is unused in @dst, so nothing
 * in the built-in tree can be redirected to a bootloader node.
 */
static int __init copy_props(void *dst, int dnode, const void *src, int snode,
			     bool new_node)
{
	const char *name;
	const void *val;
	int prop, len, ret;

	fdt_for_each_property_offset(prop, src, snode) {
		val = fdt_getprop_by_offset(src, prop, &name, &len);
		if (!val)
			return len;

		if (is_phandle_prop(name)) {
			if (!new_node || len != sizeof(fdt32_t) ||
			    fdt_node_offset_by_phandle(dst,
					fdt32_to_cpu(*(const fdt32_t *)val)) >= 0)
				continue;
		}

		ret = fdt_setprop(dst, dnode, name, val, len);
		if (ret)
			return ret;
	}

	return 0;
}

static int __init copy_node(void *dst, int dparent, const void *src, int snode)
{
	int dnode, child, ret;

	dnode = fdt_add_subnode(dst, dparent, fdt_get_name(src, snode, NULL));
	if (dnode < 0)
		return dnode;

	ret = copy_props(dst, dnode, src, snode, true);
	if (ret)
		return ret;

	fdt_for_each_subnode(child, src, snode) {
		ret = copy_node(dst, dnode, src, child);
		if (ret)
			return ret;
	}

	return 0;
}

/* Merge @spath's properties and children into the same path in @dst. */
static int __init merge_node(void *dst, const void *src, const char *spath)
{
	int snode, dnode, child, dchild, ret;
	const char *name;

	snode = fdt_path_offset(src, spath);
	if (snode < 0)
		return 0;

	dnode = fdt_path_offset(dst, spath);
	if (dnode < 0)
		return copy_node(dst, 0, src, snode);

	ret = copy_props(dst, dnode, src, snode, false);
	if (ret)
		return ret;

	fdt_for_each_subnode(child, src, snode) {
		name = fdt_get_name(src, child, NULL);
		dnode = fdt_path_offset(dst, spath);
		dchild = fdt_subnode_offset(dst, dnode, name);
		if (dchild >= 0)
			ret = copy_props(dst, dchild, src, child, false);
		else
			ret = copy_node(dst, dnode, src, child);
		if (ret)
			return ret;
	}

	return 0;
}

static int __init import_root_nodes(void *dst, const void *src)
{
	const char *name, *type;
	char path[64];
	int child, ret;

	fdt_for_each_subnode(child, src, 0) {
		name = fdt_get_name(src, child, NULL);
		type = fdt_getprop(src, child, "device_type", NULL);

		if (fdt_subnode_offset(dst, 0, name) < 0) {
			ret = copy_node(dst, 0, src, child);
		} else if (type && !strcmp(type, "memory")) {
			snprintf(path, sizeof(path), "/%s", name);
			ret = merge_node(dst, src, path);
		} else {
			ret = 0;
		}
		if (ret)
			return ret;
	}

	return 0;
}

static int __init import_runtime_props(void *dst, const void *src)
{
	const void *val;
	int i, snode, dnode, len, ret;

	for (i = 0; i < ARRAY_SIZE(lk_runtime_props); i++) {
		snode = fdt_path_offset(src, lk_runtime_props[i][0]);
		dnode = fdt_path_offset(dst, lk_runtime_props[i][0]);
		if (snode < 0 || dnode < 0)
			continue;

		val = fdt_getprop(src, snode, lk_runtime_props[i][1], &len);
		if (!val)
			continue;

		ret = fdt_setprop(dst, dnode, lk_runtime_props[i][1], val, len);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Regions LK carves out for the vendor SSMR/zone-movable CMA scheme (about
 * 850 MiB, a fifth of the RAM). Nothing in this kernel claims them, so hand
 * them back to the page allocator.
 */
static const char *const unused_reserved_mem[] __initconst = {
	"zone-movable-cma-memory",
	"zmc-default",
};

static int __init release_unused_reserved_mem(void *dst)
{
	int parent, node, i, ret;

	for (i = 0; i < ARRAY_SIZE(unused_reserved_mem); i++) {
		parent = fdt_path_offset(dst, "/reserved-memory");
		if (parent < 0)
			return 0;

		node = fdt_subnode_offset(dst, parent, unused_reserved_mem[i]);
		if (node < 0)
			continue;

		ret = fdt_setprop_string(dst, node, "status", "disabled");
		if (ret)
			return ret;
	}

	return 0;
}

void * __init arm64_builtin_dtb_merge(const void *boot_fdt)
{
	void *dst = builtin_fdt_buf;
	const char *args;
	int ret;

	ret = boot_fdt ? fdt_path_offset(boot_fdt, "/chosen") : -1;
	args = ret >= 0 ? fdt_getprop(boot_fdt, ret, "bootargs", NULL) : NULL;
	if (args && strstr(args, "builtin_dtb=off")) {
		pr_info("builtin-dtb: disabled on the command line\n");
		return NULL;
	}

	if (fdt_check_header(__dtb_start)) {
		pr_err("builtin-dtb: invalid built-in DTB\n");
		return NULL;
	}

	ret = fdt_open_into(__dtb_start, dst, sizeof(builtin_fdt_buf));
	if (ret)
		goto err;

	if (!boot_fdt || fdt_check_header(boot_fdt)) {
		pr_warn("builtin-dtb: no usable bootloader DTB to import\n");
		goto out;
	}

	ret = merge_node(dst, boot_fdt, "/chosen");
	if (!ret)
		ret = merge_node(dst, boot_fdt, "/reserved-memory");
	if (!ret)
		ret = import_root_nodes(dst, boot_fdt);
	if (!ret)
		ret = import_runtime_props(dst, boot_fdt);
	if (!ret)
		ret = release_unused_reserved_mem(dst);
	if (ret)
		goto err;

out:
	fdt_pack(dst);
	pr_info("builtin-dtb: using built-in DTB (%u bytes)\n",
		fdt_totalsize(dst));
	return dst;

err:
	pr_err("builtin-dtb: merge failed (%d), using bootloader DTB\n", ret);
	return NULL;
}
