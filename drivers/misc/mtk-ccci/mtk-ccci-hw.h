/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_CCCI_HW_H__
#define __MTK_CCCI_HW_H__

#include <linux/types.h>

struct device;

extern bool mtk_ccci_hw;
extern bool mtk_ccci_power;

int mtk_ccci_md_pwr_init(struct device *mddev);
int mtk_ccci_md_pwr_on(void);
int mtk_ccci_md_pwr_off(void);
bool mtk_ccci_md_pwr_is_on(void);
void mtk_ccci_md_pwr_dump(const char *tag);
void mtk_ccci_md_rf_dump(const char *tag);
void mtk_ccci_dpmaif_cg(bool on);

#endif
