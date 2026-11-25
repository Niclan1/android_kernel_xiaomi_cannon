// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6853 MIPI DSI TX PHY
 */

#include <linux/math64.h>
#include <linux/moduleparam.h>

#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"
#include "../../gpu/drm/mediatek/mtk_diag.h"

/* DIAG only: keep the PLL setup the bootloader left, like the vendor driver */
static bool mtk_diag_phy_skip;
core_param(mtk_diag_phy_skip, mtk_diag_phy_skip, bool, 0644);

#define MIPITX_LANE_CON 0x000c
#define MIPITX_PRESERVED 0x0014
#define MIPITX_PLL_PWR 0x0028
#define MIPITX_PLL_CON0 0x002c
#define MIPITX_PLL_CON1 0x0030
#define MIPITX_PLL_CON4 0x003c
#define MIPITX_SW_CTRL_CON4 0x0060

#define AD_DSI_PLL_SDM_PWR_ON BIT(0)
#define AD_DSI_PLL_SDM_ISO_EN BIT(1)
#define RG_DSI_PLL_EN BIT(4)
#define RG_DSI_PLL_POSDIV GENMASK(10, 8)

static int mt6853_mipi_tx_pll_prepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;
	unsigned int txdiv, txdiv0;
	u64 pcw;

	pr_debug("DIAG-DISP phy-pre data_rate=%u\n", mipi_tx->data_rate);
	mtk_diag_miptx(base, "phy-pre");

	if (mtk_diag_phy_skip &&
	    (readl(base + MIPITX_PLL_CON1) & RG_DSI_PLL_EN)) {
		pr_debug("DIAG-DISP phy: PLL already on, keeping bootloader setup\n");
		return 0;
	}

	if (mipi_tx->data_rate >= 2000000000) {
		txdiv = 1;
		txdiv0 = 0;
	} else if (mipi_tx->data_rate >= 1000000000) {
		txdiv = 2;
		txdiv0 = 1;
	} else if (mipi_tx->data_rate >= 500000000) {
		txdiv = 4;
		txdiv0 = 2;
	} else if (mipi_tx->data_rate > 250000000) {
		txdiv = 8;
		txdiv0 = 3;
	} else if (mipi_tx->data_rate >= 125000000) {
		txdiv = 16;
		txdiv0 = 4;
	} else {
		return -EINVAL;
	}

	writel(0, base + MIPITX_PRESERVED);
	writel(0x00ff12e0, base + MIPITX_PLL_CON4);
	writel(0x3fff0180, base + MIPITX_LANE_CON);
	usleep_range(500, 600);
	writel(0x3fff0080, base + MIPITX_LANE_CON);

	mtk_phy_set_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
	usleep_range(30, 100);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);

	pcw = div_u64(((u64)mipi_tx->data_rate * txdiv) << 24, 26000000);
	writel((u32)pcw, base + MIPITX_PLL_CON0);
	mtk_phy_update_field(base + MIPITX_PLL_CON1, RG_DSI_PLL_POSDIV, txdiv0);
	mtk_phy_set_bits(base + MIPITX_PLL_CON1, RG_DSI_PLL_EN);
	usleep_range(50, 100);

	mtk_diag_miptx(base, "phy-post");

	return 0;
}

static void mt6853_mipi_tx_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;

	mtk_phy_clear_bits(base + MIPITX_PLL_CON1, RG_DSI_PLL_EN);
	mtk_phy_set_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
	writel(0x3fff0180, base + MIPITX_LANE_CON);
	writel(0x3fff0100, base + MIPITX_LANE_CON);
}

static long mt6853_mipi_tx_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long *parent_rate)
{
	return clamp_val(rate, 125000000, 2000000000);
}

static const struct clk_ops mt6853_mipi_tx_pll_ops = {
	.prepare = mt6853_mipi_tx_pll_prepare,
	.unprepare = mt6853_mipi_tx_pll_unprepare,
	.round_rate = mt6853_mipi_tx_pll_round_rate,
	.set_rate = mtk_mipi_tx_pll_set_rate,
	.recalc_rate = mtk_mipi_tx_pll_recalc_rate,
};

static void mt6853_mipi_tx_power_on_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);

	mtk_phy_set_bits(mipi_tx->regs + MIPITX_SW_CTRL_CON4, BIT(0));
}

static void mt6853_mipi_tx_power_off_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);

	mtk_phy_clear_bits(mipi_tx->regs + MIPITX_SW_CTRL_CON4, BIT(0));
}

const struct mtk_mipitx_data mt6853_mipitx_data = {
	.mipi_tx_clk_ops = &mt6853_mipi_tx_pll_ops,
	.mipi_tx_enable_signal = mt6853_mipi_tx_power_on_signal,
	.mipi_tx_disable_signal = mt6853_mipi_tx_power_off_signal,
};
