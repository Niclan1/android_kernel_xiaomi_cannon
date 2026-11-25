// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <soc/mediatek/smi.h>
#include "mtk_vcodec_enc_pm.h"
#include "mtk_vcu.h"

#define VENC_GCON_BASE		0x17000000
#define VENC_CG_CON		0x0
#define VENC_CG_SET		0x4
#define VENC_CG_ALL		(BIT(0) | BIT(4) | BIT(8) | BIT(28))
#define SPM_BASE		0x10006000
#define SPM_PWR_STATUS		0x16c
#define SPM_PWR_STATUS_2ND	0x170
#define VENC_PWR_STA		BIT(17)

int mtk_vcodec_init_enc_pm(struct mtk_vcodec_dev *dev)
{
	struct device *enc = &dev->plat_dev->dev;
	struct device_node *node;
	struct platform_device *larb;
	int ret;

	dev->pm.dev = enc;
	mutex_init(&dev->pm.lock);
	/* shared with the clock and scpsys drivers, so no request_mem_region */
	dev->pm.cg_base = devm_ioremap(enc, VENC_GCON_BASE, 0x10);
	dev->pm.spm_base = devm_ioremap(enc, SPM_BASE, 0x1000);
	if (!dev->pm.cg_base || !dev->pm.spm_base)
		return -ENOMEM;
	dev->pm.clk_MT_CG_VENC = devm_clk_get(enc, "MT_CG_VENC");
	if (IS_ERR(dev->pm.clk_MT_CG_VENC))
		return dev_err_probe(enc, PTR_ERR(dev->pm.clk_MT_CG_VENC),
				     "cannot get encoder clock\n");
	node = of_parse_phandle(enc->of_node, "mediatek,larb", 0);
	if (!node)
		return -EINVAL;
	larb = of_find_device_by_node(node);
	of_node_put(node);
	if (!larb)
		return -EPROBE_DEFER;
	if (!device_is_bound(&larb->dev)) {
		put_device(&larb->dev);
		return -EPROBE_DEFER;
	}
	dev->pm.larbvenc = &larb->dev;
	ret = devm_pm_runtime_enable(enc);
	if (ret) {
		put_device(dev->pm.larbvenc);
		dev->pm.larbvenc = NULL;
		return ret;
	}
	ret = dma_set_mask_and_coherent(enc, DMA_BIT_MASK(34));
	if (!ret)
		ret = vb2_dma_contig_set_max_seg_size(enc, DMA_BIT_MASK(32));
	if (ret) {
		put_device(dev->pm.larbvenc);
		dev->pm.larbvenc = NULL;
	}
	return ret;
}

void mtk_vcodec_release_enc_pm(struct mtk_vcodec_dev *dev)
{
	vb2_dma_contig_clear_max_seg_size(dev->pm.dev);
	put_device(dev->pm.larbvenc);
	dev->pm.larbvenc = NULL;
}

static void venc_trace_state(struct mtk_vcodec_dev *dev, const char *what)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	u32 sta, sta2, cg = 0;
	bool on;

	if (!vcu_trace_on())
		return;
	sta = readl(pm->spm_base + SPM_PWR_STATUS);
	sta2 = readl(pm->spm_base + SPM_PWR_STATUS_2ND);
	on = (sta & sta2 & VENC_PWR_STA) != 0;
	/* venc_gcon sits inside the domain, only read it while powered */
	if (on)
		cg = readl(pm->cg_base + VENC_CG_CON);
	vcu_trace_log("[VENC] %s ref %d pwr 0x%x/0x%x domain %s cg_con %s0x%x\n",
		 what, pm->refcnt, sta, sta2, on ? "on" : "off",
		 on ? "" : "unread ", cg);
}

/*
 * The venc gates are inverted: a set bit in CG_CON means the clock runs,
 * so they are opened through CG_SET (0x4), not CG_CLR.
 */
static int venc_check_cg(struct mtk_vcodec_dev *dev)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	u32 sta, sta2, cg;

	sta = readl(pm->spm_base + SPM_PWR_STATUS);
	sta2 = readl(pm->spm_base + SPM_PWR_STATUS_2ND);
	if (!(sta & sta2 & VENC_PWR_STA)) {
		vcu_trace_log("[VENC] domain off after power on (0x%x/0x%x)\n",
			 sta, sta2);
		return -EIO;
	}
	cg = readl(pm->cg_base + VENC_CG_CON);
	if ((cg & VENC_CG_ALL) != VENC_CG_ALL) {
		writel(VENC_CG_ALL, pm->cg_base + VENC_CG_SET);
		cg = readl(pm->cg_base + VENC_CG_CON);
		if ((cg & VENC_CG_ALL) != VENC_CG_ALL)
			return -EIO;
	}
	return 0;
}

static int venc_power_on(struct mtk_vcodec_dev *dev)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	unsigned long flags;
	int ret;

	venc_trace_state(dev, "power on begin");
	/* venc power domain first, then larb7 and the venc clock gate */
	ret = pm_runtime_resume_and_get(pm->dev);
	if (ret < 0)
		return ret;
	ret = pm_runtime_resume_and_get(pm->larbvenc);
	if (ret < 0)
		goto err_put_dev;
	ret = clk_prepare_enable(pm->clk_MT_CG_VENC);
	if (ret)
		goto err_put_larb;
	ret = venc_check_cg(dev);
	if (ret)
		goto err_disable_clk;
	pm->ports_ready = false;
	spin_lock_irqsave(&dev->irqlock, flags);
	pm->powered = true;
	pm->reg_traced = false;
	spin_unlock_irqrestore(&dev->irqlock, flags);
	venc_trace_state(dev, "power on done");
	return 0;

err_disable_clk:
	clk_disable_unprepare(pm->clk_MT_CG_VENC);
err_put_larb:
	pm_runtime_put_sync(pm->larbvenc);
err_put_dev:
	pm_runtime_put_sync(pm->dev);
	return ret;
}

static void venc_power_off(struct mtk_vcodec_dev *dev)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	unsigned long flags;

	venc_trace_state(dev, "power off begin");
	spin_lock_irqsave(&dev->irqlock, flags);
	pm->powered = false;
	spin_unlock_irqrestore(&dev->irqlock, flags);
	clk_disable_unprepare(pm->clk_MT_CG_VENC);
	pm_runtime_put_sync(pm->larbvenc);
	pm_runtime_put_sync(pm->dev);
	venc_trace_state(dev, "power off done");
}

static int venc_get(struct mtk_vcodec_dev *dev, bool encode)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	int ret = 0;

	mutex_lock(&pm->lock);
	if (pm->system_suspended) {
		ret = -EBUSY;
		goto out;
	}
	if (!pm->powered) {
		ret = venc_power_on(dev);
		if (ret) {
			dev_err_ratelimited(pm->dev, "encoder power on failed: %d\n", ret);
			goto out;
		}
	}
	/* vpud can keep registers mapped long before the first DMA after wake. */
	if (encode && !pm->ports_ready) {
		ret = venc_check_cg(dev);
		if (!ret)
			ret = mtk_smi_larb_mt6853_restore_venc(pm->larbvenc);
		if (ret) {
			dev_err_ratelimited(pm->dev, "encoder DMA path not ready: %d\n", ret);
			if (!pm->refcnt)
				venc_power_off(dev);
			goto out;
		}
		pm->ports_ready = true;
	}
	pm->refcnt++;
out:
	mutex_unlock(&pm->lock);
	return ret;
}

int mtk_venc_power_get(void *priv)
{
	return venc_get(priv, false);
}

void mtk_venc_power_put(void *priv)
{
	struct mtk_vcodec_dev *dev = priv;
	struct mtk_vcodec_pm *pm = &dev->pm;

	mutex_lock(&pm->lock);
	if (!WARN_ON(!pm->refcnt) && !--pm->refcnt && pm->powered)
		venc_power_off(dev);
	mutex_unlock(&pm->lock);
}

void mtk_venc_system_suspend(struct mtk_vcodec_dev *dev)
{
	struct mtk_vcodec_pm *pm = &dev->pm;

	mutex_lock(&pm->lock);
	pm->system_suspended = true;
	if (pm->powered)
		venc_power_off(dev);
	mutex_unlock(&pm->lock);
}

int mtk_venc_system_resume(struct mtk_vcodec_dev *dev)
{
	struct mtk_vcodec_pm *pm = &dev->pm;
	int ret = 0;

	mutex_lock(&pm->lock);
	if (pm->system_suspended && pm->refcnt) {
		ret = venc_power_on(dev);
		if (ret)
			dev_err(pm->dev, "encoder resume failed: %d\n", ret);
	}
	pm->system_suspended = false;
	mutex_unlock(&pm->lock);
	return ret;
}

int mtk_vcodec_enc_clock_on(struct mtk_vcodec_ctx *ctx, int core_id)
{
	if (core_id)
		return -EINVAL;
	if (vcu_trace_on())
		vcu_trace_log("[VENC] clock_on ctx %d\n", ctx->id);
	return venc_get(ctx->dev, true);
}

void mtk_vcodec_enc_clock_off(struct mtk_vcodec_ctx *ctx, int core_id)
{
	if (core_id)
		return;
	if (vcu_trace_on())
		vcu_trace_log("[VENC] clock_off ctx %d\n", ctx->id);
	mtk_venc_power_put(ctx->dev);
}
