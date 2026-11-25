/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mach/mtk_pbm.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_PBM_H
#define __MTK_CCCI_PBM_H
#include <linux/types.h>
/* No power budget manager here; md_sys1_platform.c has a weak fallback. */
#define DISABLE_PBM_FEATURE
enum pbm_kicker {
	KR_DLPT,
	KR_MD1,
	KR_MD3,
	KR_CPU,
	KR_GPU,
	KR_FLASH
};
void kicker_pbm_by_md(enum pbm_kicker kicker, bool status);
#endif
