/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MT6853_VCODEC_ENC_PM_H
#define MT6853_VCODEC_ENC_PM_H
#include "mtk_vcodec_drv.h"
int mtk_vcodec_init_enc_pm(struct mtk_vcodec_dev *dev);
void mtk_vcodec_release_enc_pm(struct mtk_vcodec_dev *dev);
int mtk_vcodec_enc_clock_on(struct mtk_vcodec_ctx *ctx, int core_id);
void mtk_vcodec_enc_clock_off(struct mtk_vcodec_ctx *ctx, int core_id);
int mtk_venc_power_get(void *priv);
void mtk_venc_power_put(void *priv);
void mtk_venc_system_suspend(struct mtk_vcodec_dev *dev);
int mtk_venc_system_resume(struct mtk_vcodec_dev *dev);
#endif
