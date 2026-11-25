/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mt-plat/aee.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_AEE_H
#define __MTK_CCCI_AEE_H
#include <linux/printk.h>

/* There is no AEE (MediaTek crash reporter) here; reports go to the log. */
#define DB_OPT_DEFAULT		(0)
#define DB_OPT_FTRACE		(1 << 0)

#define aee_kernel_warning(module, msg...)	pr_warn("ccci aee: " msg)
#define aee_kernel_warning_api(file, line, db_opt, module, msg...)	\
	pr_warn("ccci aee(%s): " msg)
#define aee_kernel_exception(module, msg...)	pr_err("ccci aee: " msg)
#define aee_kernel_exception_api(file, line, db_opt, module, msg...)	\
	pr_err("ccci aee(%s): " msg)

static inline void aed_md_exception_api(const int *log, int log_size,
		const int *phy, int phy_size, const char *detail, const int db_opt)
{
	pr_err("ccci aee: modem exception: %s\n", detail ? detail : "");
}
#endif
