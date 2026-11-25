/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <memory/mediatek/emi.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_EMI_H
#define __MTK_CCCI_EMI_H
#include <linux/types.h>
struct reg_info_t {
	unsigned int offset;
	unsigned int value;
	unsigned int leng;
};
/* No EMI MPU driver here; mdee_dumper_v5.c has a weak fallback. */
int mtk_emimpu_md_handling_register(void (*md_handling_func)
		(unsigned int emi_id, struct reg_info_t *dump, unsigned int leng));
#endif
