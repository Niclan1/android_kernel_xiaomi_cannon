// SPDX-License-Identifier: GPL-2.0
/*
 * MT6853 consys: start-up gate for the vendor WMT/STP core.
 *
 * The vendor core is linked in but stays dormant. Its character devices and
 * platform drivers are only registered from here, and only when the kernel
 * command line carries mtk_consys.hw=1. With the default (0) this file logs
 * one line and the stage 1 /dev/wmtdetect handshake stays in charge.
 *
 * mtk_consys.power=0 keeps the chip unpowered while everything else is
 * registered, which separates probe problems from power-up problems.
 */

#define pr_fmt(fmt) "mtk-consys-hw: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>

#include "mtk-consys-hw.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "mtk_consys."

bool mtk_consys_hw = true;
module_param_named(hw, mtk_consys_hw, bool, 0444);
MODULE_PARM_DESC(hw, "Start the connectivity core (default on; 0 = hardware untouched)");

bool mtk_consys_power = true;
module_param_named(power, mtk_consys_power, bool, 0444);
MODULE_PARM_DESC(power, "With hw=1, allow powering up the chip (0 = register drivers only)");

int wmt_detect_driver_init(void);
int mtk_btif_driver_init(void);

static bool mtk_consys_hw_detect_up;

bool mtk_consys_hw_detect_active(void)
{
	return mtk_consys_hw_detect_up;
}

static bool mtk_consys_hw_present(void)
{
	struct device_node *np;
	bool ok;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6853-consys");
	if (!np)
		return false;
	ok = of_device_is_available(np);
	of_node_put(np);
	return ok;
}

/*
 * Registers the vendor /dev/wmtdetect. It runs before the stage 1 initcall,
 * which then skips its own misc device: Android's /dev is a tmpfs filled by
 * ueventd, so two devices named wmtdetect would race for the node.
 */
static int __init mtk_consys_hw_early_init(void)
{
	int ret;

	if (!mtk_consys_hw) {
		pr_info("mtk_consys.hw=0, vendor core dormant, hardware untouched\n");
		return 0;
	}
	if (!mtk_consys_hw_present()) {
		pr_info("mtk_consys.hw=1 but no mt6853 consys node, nothing to do\n");
		return 0;
	}

	MTK_CONSYS_STEP("hw step 1: mtk_consys.hw=1 power=%d, registering vendor /dev/wmtdetect\n",
			mtk_consys_power);
	ret = wmt_detect_driver_init();
	if (ret) {
		MTK_CONSYS_STEP("hw step 1: wmt_detect_driver_init failed (%d), stage 1 handshake stays\n",
				ret);
		return 0;
	}
	mtk_consys_hw_detect_up = true;
	MTK_CONSYS_STEP("hw step 1: vendor /dev/wmtdetect registered\n");
	return 0;
}
fs_initcall(mtk_consys_hw_early_init);

/*
 * The BTIF platform device needs its clocks, so its driver is registered
 * after the clock providers had their chance to probe.
 */
static int __init mtk_consys_hw_late_init(void)
{
	int ret;

	if (!mtk_consys_hw_detect_up)
		return 0;

	MTK_CONSYS_STEP("hw step 2: registering btif driver (STP transport)\n");
	ret = mtk_btif_driver_init();
	if (ret)
		MTK_CONSYS_STEP("hw step 2: btif init failed (%d), chip cannot be powered on\n",
				ret);
	else
		MTK_CONSYS_STEP("hw step 2: btif ready\n");
	return 0;
}
late_initcall(mtk_consys_hw_late_init);
