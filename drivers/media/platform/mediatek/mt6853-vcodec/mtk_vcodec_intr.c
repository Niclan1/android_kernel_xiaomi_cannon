// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2016 MediaTek Inc.
#include <linux/io.h>
#include "mtk_vcodec_intr.h"
#include "mtk_vcodec_util.h"
#include "mtk_vcu.h"

int mtk_vcodec_wait_for_done_ctx(struct mtk_vcodec_ctx *ctx,
	int core_id, int command, unsigned int timeout_ms)
{
	long ret;

	if (core_id)
		return -EINVAL;
	ret = wait_event_interruptible_timeout(ctx->queue[0],
		READ_ONCE(ctx->int_cond[0]), msecs_to_jiffies(timeout_ms));
	WRITE_ONCE(ctx->int_cond[0], 0);
	return ret > 0 ? 0 : -1;
}

irqreturn_t mtk_vcodec_enc_irq_handler(int irq, void *priv)
{
	struct mtk_vcodec_dev *dev = priv;
	struct mtk_vcodec_ctx *ctx;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&dev->irqlock, flags);
	ctx = dev->curr_enc_ctx[0];
	/* a register read with the venc block off stalls the bus */
	if (!dev->pm.powered) {
		spin_unlock_irqrestore(&dev->irqlock, flags);
		dev_warn_ratelimited(&dev->plat_dev->dev, "irq while powered off\n");
		return IRQ_HANDLED;
	}
	if (!dev->pm.reg_traced) {
		dev->pm.reg_traced = true;
		if (vcu_trace_on())
			vcu_trace_log("[VENC] first irq reg access ctx %d\n",
				 ctx ? ctx->id : -1);
	}
	status = readl(dev->enc_reg_base[VENC_SYS] + MTK_VENC_IRQ_STATUS_OFFSET);
	writel(status, dev->enc_reg_base[VENC_SYS] + MTK_VENC_IRQ_ACK_OFFSET);
	if (ctx) {
		ctx->irq_status = status;
		WRITE_ONCE(ctx->int_cond[0], 1);
		wake_up_interruptible(&ctx->queue[0]);
	}
	spin_unlock_irqrestore(&dev->irqlock, flags);
	return IRQ_HANDLED;
}

int mtk_vcodec_enc_irq_setup(struct platform_device *pdev,
	struct mtk_vcodec_dev *dev)
{
	int ret;

	dev->enc_irq = platform_get_irq(pdev, 0);
	if (dev->enc_irq < 0)
		return dev->enc_irq;
	ret = devm_request_irq(&pdev->dev, dev->enc_irq,
		mtk_vcodec_enc_irq_handler, IRQF_NO_AUTOEN, pdev->name, dev);
	return ret;
}
