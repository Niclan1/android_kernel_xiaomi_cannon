/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MT6853_VCODEC_PM_H
#define MT6853_VCODEC_PM_H
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/mutex.h>
#define MTK_PLATFORM_STR "platform:mt6853"
struct mtk_vcodec_pm {
	struct device *dev;
	struct device *larbvenc;
	struct clk *clk_MT_CG_VENC;
	bool powered;
	bool reg_traced;
	bool system_suspended;
	bool ports_ready;
	int refcnt;
	struct mutex lock;
	void __iomem *cg_base;
	void __iomem *spm_base;
};
enum mtk_dec_dtsi_reg_idx {
	VDEC_SYS, VDEC_UFO, VDEC_VLD, VDEC_MC, VDEC_MV, VDEC_MISC,
	NUM_MAX_VDEC_REG_BASE,
};
enum mtk_enc_dtsi_reg_idx { VENC_SYS, NUM_MAX_VENC_REG_BASE };
#endif
