/*
* Copyright (c) 2016 MediaTek Inc.
* Author: PC Chen <pc.chen@mediatek.com>
*       Tiffany Lin <tiffany.lin@mediatek.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include <linux/module.h>
#include <linux/module.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk_vcodec_drv.h"
#include "mtk_vcodec_util.h"
#include "mtk_vcu.h"
#include "venc_drv_if.h"

#define LOG_INFO_SIZE 64
#define MAX_SUPPORTED_LOG_PARAMS_COUNT 12

/* For encoder, this will enable logs in venc/*/
bool mtk_vcodec_dbg;
EXPORT_SYMBOL(mtk_vcodec_dbg);

/* For vcodec performance measure */
bool mtk_vcodec_perf;
EXPORT_SYMBOL(mtk_vcodec_perf);

/* The log level of v4l2 encoder or decoder driver.
 * That is, files under mtk-vcodec/.
 */
int mtk_v4l2_dbg_level;
EXPORT_SYMBOL(mtk_v4l2_dbg_level);

void __iomem *mtk_vcodec_get_dec_reg_addr(struct mtk_vcodec_ctx *data,
	unsigned int reg_idx)
{
	struct mtk_vcodec_ctx *ctx = (struct mtk_vcodec_ctx *)data;

	if (!data || reg_idx >= NUM_MAX_VDEC_REG_BASE) {
		mtk_v4l2_err("Invalid arguments, reg_idx=%d", reg_idx);
		return NULL;
	}
	return ctx->dev->dec_reg_base[reg_idx];
}
EXPORT_SYMBOL(mtk_vcodec_get_dec_reg_addr);

void __iomem *mtk_vcodec_get_enc_reg_addr(struct mtk_vcodec_ctx *data,
	unsigned int reg_idx)
{
	struct mtk_vcodec_ctx *ctx = (struct mtk_vcodec_ctx *)data;

	if (!data || reg_idx >= NUM_MAX_VENC_REG_BASE) {
		mtk_v4l2_err("Invalid arguments, reg_idx=%d", reg_idx);
		return NULL;
	}
	return ctx->dev->enc_reg_base[reg_idx];
}
EXPORT_SYMBOL(mtk_vcodec_get_enc_reg_addr);


int mtk_vcodec_mem_alloc(struct mtk_vcodec_ctx *data,
						 struct mtk_vcodec_mem *mem)
{
	unsigned long size = mem->size;
	struct mtk_vcodec_ctx *ctx = (struct mtk_vcodec_ctx *)data;
	struct device *dev = &ctx->dev->plat_dev->dev;

	mem->va = dma_alloc_coherent(dev, size, &mem->dma_addr, GFP_KERNEL);

	if (!mem->va) {
		mtk_v4l2_err("%s dma_alloc size=%ld failed!", dev_name(dev),
					 size);
		return -ENOMEM;
	}

	memset(mem->va, 0, size);

	mtk_v4l2_debug(4, "[%d]  - va      = %p", ctx->id, mem->va);
	mtk_v4l2_debug(4, "[%d]  - dma     = 0x%lx", ctx->id,
				   (unsigned long)mem->dma_addr);
	mtk_v4l2_debug(4, "[%d]    size = 0x%lx", ctx->id, size);

	return 0;
}
EXPORT_SYMBOL(mtk_vcodec_mem_alloc);

void mtk_vcodec_mem_free(struct mtk_vcodec_ctx *data,
						 struct mtk_vcodec_mem *mem)
{
	unsigned long size = mem->size;
	struct mtk_vcodec_ctx *ctx = (struct mtk_vcodec_ctx *)data;
	struct device *dev = &ctx->dev->plat_dev->dev;

	if (!mem->va) {
		mtk_v4l2_err("%s dma_free size=%ld failed!", dev_name(dev),
					 size);
		return;
	}

	mtk_v4l2_debug(4, "[%d]  - va      = %p", ctx->id, mem->va);
	mtk_v4l2_debug(4, "[%d]  - dma     = 0x%lx", ctx->id,
				   (unsigned long)mem->dma_addr);
	mtk_v4l2_debug(4, "[%d]    size = 0x%lx", ctx->id, size);

	dma_free_coherent(dev, size, mem->va, mem->dma_addr);
	mem->va = NULL;
	mem->dma_addr = 0;
	mem->size = 0;
}
EXPORT_SYMBOL(mtk_vcodec_mem_free);

void mtk_vcodec_set_curr_ctx(struct mtk_vcodec_dev *dev,
	struct mtk_vcodec_ctx *ctx, unsigned int hw_id)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);
	dev->curr_dec_ctx[hw_id] = ctx;
	spin_unlock_irqrestore(&dev->irqlock, flags);
}
EXPORT_SYMBOL(mtk_vcodec_set_curr_ctx);

struct mtk_vcodec_ctx *mtk_vcodec_get_curr_ctx(struct mtk_vcodec_dev *dev,
	unsigned int hw_id)
{
	unsigned long flags;
	struct mtk_vcodec_ctx *ctx;

	spin_lock_irqsave(&dev->irqlock, flags);
	ctx = dev->curr_dec_ctx[hw_id];
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return ctx;
}
EXPORT_SYMBOL(mtk_vcodec_get_curr_ctx);


void v4l2_m2m_buf_queue_check(struct v4l2_m2m_ctx *m2m_ctx,
		void *vbuf)
{
	struct v4l2_m2m_buffer *b = container_of(vbuf,
				struct v4l2_m2m_buffer, vb);
	mtk_v4l2_debug(8, "[Debug] b %p b->list.next %p prev %p %p %p\n",
		b, b->list.next, b->list.prev,
		LIST_POISON1, LIST_POISON2);

	if (WARN_ON(IS_ERR_OR_NULL(m2m_ctx) ||
		(b->list.next != LIST_POISON1 && b->list.next) ||
		(b->list.prev != LIST_POISON2 && b->list.prev))) {
		v4l2_aee_print("b %p next %p prev %p already in rdyq %p %p\n",
			b, b->list.next, b->list.prev,
			LIST_POISON1, LIST_POISON2);
		return;
	}
	v4l2_m2m_buf_queue(m2m_ctx, vbuf);
}
EXPORT_SYMBOL(v4l2_m2m_buf_queue_check);

void mtk_vcodec_set_log(struct mtk_vcodec_ctx *ctx, char *val)
{
	int i, argc = 0, ret = 0, argcMex = 0;
	//char argv[MAX_SUPPORTED_LOG_PARAMS_COUNT * 2][LOG_INFO_SIZE] = {0};
	char *argv[MAX_SUPPORTED_LOG_PARAMS_COUNT * 2];
	char *temp = NULL;
	char *token = NULL;
	int max_cpy_len = 0;
	long temp_val = 0;
	char vcu_log[LOG_INFO_SIZE] = {0};

	pr_info("%s: %s", __func__, val);

	if (val == NULL) {
		mtk_v4l2_err("cannot set log due to input is null");
		return;
	}

	for (i = 0; i < MAX_SUPPORTED_LOG_PARAMS_COUNT * 2; i++)
		argv[i] = kzalloc(LOG_INFO_SIZE, GFP_KERNEL);

	temp = val;
	for (token = strsep(&temp, " "); token != NULL; token = strsep(&temp, " ")) {
		max_cpy_len = strnlen(token, LOG_INFO_SIZE - 1);
		if (argc >= 0 && argv[argc]) {
			argcMex = (LOG_INFO_SIZE-1 > max_cpy_len) ?
				max_cpy_len : (LOG_INFO_SIZE-2);
			strncpy(argv[argc], token, argcMex);
			argv[argc][argcMex+1] = '\0';
		}
		argc++;
		if (argc >= MAX_SUPPORTED_LOG_PARAMS_COUNT * 2)
			break;
	}

	argcMex = (argc >= MAX_SUPPORTED_LOG_PARAMS_COUNT * 2
			? MAX_SUPPORTED_LOG_PARAMS_COUNT * 2 : argc);
	for (i = 0; i < argcMex; i++) {
		if (argv[i] != NULL)
			argv[i][LOG_INFO_SIZE-1] = '\0';
		else
			continue;
		if ((i < argcMex-1) && strcmp("-mtk_vcodec_dbg", argv[i]) == 0) {
			if (kstrtol(argv[++i], 0, &temp_val) == 0)
				mtk_vcodec_dbg = temp_val;
		} else if ((i < argcMex-1) && strcmp("-mtk_vcodec_perf", argv[i]) == 0) {
			if (kstrtol(argv[++i], 0, &temp_val) == 0)
				mtk_vcodec_perf = temp_val;
		} else if ((i < argcMex-1) && strcmp("-mtk_v4l2_dbg_level", argv[i]) == 0) {
			if (kstrtol(argv[++i], 0, &temp_val) == 0)
				mtk_v4l2_dbg_level = temp_val;
		} else if (i < argcMex-1) {
			memset(vcu_log, 0x00, LOG_INFO_SIZE);
			if (argv[i+1] != NULL) {
				argv[i+1][LOG_INFO_SIZE-1] = '\0';
				ret = snprintf(vcu_log, LOG_INFO_SIZE, "%s %s", argv[i], argv[i+1]);
			} else {
				pr_info("[MTK_V4L2] vcu_log input arg[%d] error: Null value", i+1);
				break;
			}
			if (ret < 0) {
				pr_info("[MTK_V4L2] vcu_log snprintf error: %d", ret);
				break;
			}
			vcu_set_log(vcu_log);
			i++;
		}
	}

	for (i = 0; i < MAX_SUPPORTED_LOG_PARAMS_COUNT * 2; i++) {
		if (argv[i] != NULL)
			kfree(argv[i]);
	}

	pr_info("----------------Debug Config ----------------\n");
	pr_info("mtk_vcodec_dbg: %d\n", mtk_vcodec_dbg);
	pr_info("mtk_vcodec_perf: %d\n", mtk_vcodec_perf);
	pr_info("mtk_v4l2_dbg_level: %d\n", mtk_v4l2_dbg_level);
}
