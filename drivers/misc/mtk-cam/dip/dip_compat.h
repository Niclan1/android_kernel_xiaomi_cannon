/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Glue between the MT6853 4.14 vendor DIP (ISP P2) driver and mainline 6.18.
 *
 *  - SMI larb control is replaced by runtime PM: imgsys_config carries the
 *    larb9 "iommus" and imgsys2_config the larb11 ones, so mtk_iommu links
 *    each to its larb; resuming them powers ISP/ISP2 and the larbs.
 *  - The vendor CMDQ core does not exist; the MDP compat driver
 *    (../mdp) runs the GCE threads, so its DIP callbacks are dropped.
 *  - M4U fault callbacks, MET, AEE/ION dump buffers and the printk
 *    rate-limit hooks do not exist.
 */
#ifndef __MTK_CAM_DIP_COMPAT_H__
#define __MTK_CAM_DIP_COMPAT_H__

#include <linux/io.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/proc_fs.h>

#define EP_CODE_MARK_CMDQ

/* <mt-plat/sync_write.h> */
#define mt_reg_sync_writel(v, a)			\
	do {						\
		writel((v), (void __iomem *)(a));	\
		mb();					\
	} while (0)

/* cmdq-util.h: register dumps go to the kernel log */
#define cmdq_util_err(fmt, ...) pr_info("[DIP] " fmt "\n", ##__VA_ARGS__)
#define CMDQ_ERR(fmt, ...) pr_err("[DIP] " fmt, ##__VA_ARGS__)

static inline const char *cmdq_core_query_first_err_mod(void)
{
	return NULL;
}

/* m4u.h: translation faults are reported by mtk_iommu itself */
enum m4u_callback_ret_t {
	M4U_CALLBACK_HANDLED,
	M4U_CALLBACK_NOT_HANDLED,
};
#define m4u_register_fault_callback(...) do { } while (0)

#endif
