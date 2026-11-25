/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mtk_clkbuf_ctl.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_CLKBUF_H
#define __MTK_CCCI_CLKBUF_H
#include <linux/types.h>
enum CLK_BUF_SWCTRL_STATUS_T {
	CLK_BUF_SW_DISABLE = 0,
	CLK_BUF_SW_ENABLE  = 1,
};
#define CLKBUF_NUM	7
void clk_buf_get_swctrl_status(enum CLK_BUF_SWCTRL_STATUS_T *status);
void clk_buf_get_rf_drv_curr(void *rf_drv_curr);
void clk_buf_save_afc_val(unsigned int afcdac);
#endif
