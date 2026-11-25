/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/pm_runtime.h>

#include "seninf_clk.h"

#ifdef IMGSENSOR_DFS_CTRL_ENABLE
/*
 * There is no mmdvfs/PM_QOS_CAM_FREQ on this kernel. Report the cam_freq
 * steps of the mmdvfs_pmqos DT node (MHz, step0 first) and remember the
 * last request so userspace sees a consistent value; the CAM mux keeps the
 * rate it was given by the clock driver.
 */
static const unsigned int seninf_cam_freq_steps[] = { 624, 499, 392, 273 };
static unsigned int seninf_cam_freq_req;

int imgsensor_dfs_ctrl(enum DFS_OPTION option, void *pbuff)
{
	struct IMAGESENSOR_GET_SUPPORTED_ISP_CLK *pIspclks;
	unsigned int lv;

	switch (option) {
	case DFS_CTRL_ENABLE:
	case DFS_CTRL_DISABLE:
	case DFS_RELEASE:
		seninf_cam_freq_req = 0;
		break;
	case DFS_UPDATE:
		seninf_cam_freq_req = *(unsigned int *)pbuff;
		break;
	case DFS_SUPPORTED_ISP_CLOCKS:
		pIspclks = pbuff;
		pIspclks->clklevelcnt = ARRAY_SIZE(seninf_cam_freq_steps);
		for (lv = 0; lv < ARRAY_SIZE(seninf_cam_freq_steps); lv++)
			pIspclks->clklevel[lv] = seninf_cam_freq_steps[lv];
		break;
	case DFS_CUR_ISP_CLOCK:
		*(unsigned int *)pbuff = seninf_cam_freq_req ?
			seninf_cam_freq_req : seninf_cam_freq_steps[0];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
#endif

static inline void seninf_clk_check(struct SENINF_CLK *pclk)
{
	int i;

	for (i = 0; i < SENINF_CLK_IDX_MAX_NUM; i++)
		WARN_ON(IS_ERR(pclk->mclk_sel[i]));
}

/**********************************************************
 *Common Clock Framework (CCF)
 **********************************************************/
enum SENINF_RETURN seninf_clk_init(struct SENINF_CLK *pclk)
{
	int i;

	if (pclk->pplatform_device == NULL) {
		PK_PR_ERR("[%s] pdev is null\n", __func__);
		return SENINF_RETURN_ERROR;
	}
	/* get all possible using clocks */
	for (i = 0; i < SENINF_CLK_IDX_MAX_NUM; i++) {
		/*
		 * SCP_SYS_CAM was a clock on 4.14; it is the CAM power domain
		 * of the seninf_top node here and is handled by runtime PM.
		 */
		if (i == SENINF_CLK_IDX_SYS_SCP_SYS_CAM)
			pclk->mclk_sel[i] = devm_clk_get_optional(
				&pclk->pplatform_device->dev,
				gseninf_mclk_name[i].pctrl);
		else
			pclk->mclk_sel[i] = devm_clk_get(
				&pclk->pplatform_device->dev,
				gseninf_mclk_name[i].pctrl);
		atomic_set(&pclk->enable_cnt[i], 0);

		if (IS_ERR(pclk->mclk_sel[i])) {
			PK_PR_ERR("cannot get %d clock %s\n", i,
				  gseninf_mclk_name[i].pctrl);
			return SENINF_RETURN_ERROR;
		}
	}
	pclk->seninf_wake_lock = wakeup_source_register(
		&pclk->pplatform_device->dev, "seninf_lock_wakelock");
	devm_pm_runtime_enable(&pclk->pplatform_device->dev);
	atomic_set(&pclk->wakelock_cnt, 0);

	return SENINF_RETURN_SUCCESS;
}

int seninf_clk_set(struct SENINF_CLK *pclk,
					struct ACDK_SENSOR_MCLK_STRUCT *pmclk)
{
	int i, ret = 0;
	unsigned int idx_tg, idx_freq = 0;

	if (pmclk->TG >= SENINF_CLK_TG_MAX_NUM ||
	    pmclk->freq > SENINF_CLK_MCLK_FREQ_MAX ||
		pmclk->freq < SENINF_CLK_MCLK_FREQ_MIN) {
		PK_PR_ERR(
	"[CAMERA SENSOR]kdSetSensorMclk out of range, tg = %d, freq = %d\n",
		  pmclk->TG, pmclk->freq);
		return -EFAULT;
	}

	PK_DBG("[CAMERA SENSOR] CCF kdSetSensorMclk on= %d, freq= %d, TG= %d\n",
	       pmclk->on, pmclk->freq, pmclk->TG);

	seninf_clk_check(pclk);

	for (i = 0; i < SENINF_CLK_IDX_FREQ_IDX_NUM &&
		    pmclk->freq != gseninf_clk_freq[i]; i++)
		;
	if (i == SENINF_CLK_IDX_FREQ_IDX_NUM)
		return -EINVAL;

	idx_tg = pmclk->TG + SENINF_CLK_IDX_TG_MIN_NUM;
	idx_freq = i + SENINF_CLK_IDX_FREQ_MIN_NUM;

	if (pmclk->on) {
#ifdef TG1_ALWAYS_ON
		/* Workaround for timestamp: TG1 always ON */
		if (clk_prepare_enable(
			pclk->mclk_sel[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]))
			PK_PR_ERR("[CAMERA SENSOR] failed tg=%d\n",
				  SENINF_CLK_IDX_TG_TOP_MUX_CAMTG);
		else
			atomic_inc(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
#endif

		if (clk_prepare_enable(pclk->mclk_sel[idx_tg]))
			PK_PR_ERR("[CAMERA SENSOR] failed tg=%d\n", pmclk->TG);
		else
			atomic_inc(&pclk->enable_cnt[idx_tg]);

		if (clk_prepare_enable(pclk->mclk_sel[idx_freq]))
			PK_PR_ERR("[CAMERA SENSOR] failed freq idx= %d\n", i);
		else
			atomic_inc(&pclk->enable_cnt[idx_freq]);

		ret = clk_set_parent(
			pclk->mclk_sel[idx_tg], pclk->mclk_sel[idx_freq]);
	} else {
		if (atomic_read(&pclk->enable_cnt[idx_freq]) > 0) {
			clk_disable_unprepare(pclk->mclk_sel[idx_freq]);
			atomic_dec(&pclk->enable_cnt[idx_freq]);
		}

		if (atomic_read(&pclk->enable_cnt[idx_tg]) > 0) {
			clk_disable_unprepare(pclk->mclk_sel[idx_tg]);
			atomic_dec(&pclk->enable_cnt[idx_tg]);
		}
#ifdef TG1_ALWAYS_ON
		/* Workaround for timestamp: TG1 always ON */
		if (atomic_read(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG])
			> 0) {
			clk_disable_unprepare(
			pclk->mclk_sel[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
			atomic_dec(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
		}
#endif
	}

	return ret;
}

void seninf_clk_open(struct SENINF_CLK *pclk)
{
	MINT32 i;

	PK_DBG("open\n");

	if (atomic_inc_return(&pclk->wakelock_cnt) == 1) {
		__pm_stay_awake(pclk->seninf_wake_lock);
		if (pm_runtime_resume_and_get(&pclk->pplatform_device->dev))
			PK_PR_ERR("[CAMERA SENSOR] CAM power domain on failed\n");
	}

	for (i = SENINF_CLK_IDX_SYS_MIN_NUM;
		i < SENINF_CLK_IDX_SYS_MAX_NUM;
		i++) {
		if (clk_prepare_enable(pclk->mclk_sel[i]))
			PK_PR_ERR("[CAMERA SENSOR] failed sys idx= %d\n", i);
		else
			atomic_inc(&pclk->enable_cnt[i]);
	}
}

void seninf_clk_release(struct SENINF_CLK *pclk)
{
	MINT32 i = SENINF_CLK_IDX_MAX_NUM;

	PK_DBG("release\n");

	do {
		i--;
		for (; atomic_read(&pclk->enable_cnt[i]) > 0;) {
			clk_disable_unprepare(pclk->mclk_sel[i]);
			atomic_dec(&pclk->enable_cnt[i]);
		}
	} while (i);

	if (atomic_dec_and_test(&pclk->wakelock_cnt)) {
		pm_runtime_put_sync(&pclk->pplatform_device->dev);
		__pm_relax(pclk->seninf_wake_lock);
	}
}

unsigned int seninf_clk_get_meter(struct SENINF_CLK *pclk, unsigned int clk)
{
	unsigned int i = 0;

	if (clk == 0xff) {
		for (i = SENINF_CLK_IDX_SYS_MIN_NUM; i < SENINF_CLK_IDX_SYS_MAX_NUM; ++i) {
			PK_DBG("[sensor_dump][mclk]index=%u freq=%lu HW enable=%d enable_cnt=%u\n",
				i,
				clk_get_rate(pclk->mclk_sel[i]),
				__clk_is_enabled(pclk->mclk_sel[i]),
				__clk_get_enable_count(pclk->mclk_sel[i])
			);
		}
	}

	/* The 4.14 ckgen frequency meter (mt_get_ckgen_freq) is not ported. */
	return 0;
}

