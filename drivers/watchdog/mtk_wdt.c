// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek Watchdog Driver
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 *
 * Based on sunxi_wdt.c
 */

#include <dt-bindings/reset/mt2712-resets.h>
#include <dt-bindings/reset/mediatek,mt6735-wdt.h>
#include <dt-bindings/reset/mediatek,mt6795-resets.h>
#include <dt-bindings/reset/mt7986-resets.h>
#include <dt-bindings/reset/mt8183-resets.h>
#include <dt-bindings/reset/mt8186-resets.h>
#include <dt-bindings/reset/mt8188-resets.h>
#include <dt-bindings/reset/mt8192-resets.h>
#include <dt-bindings/reset/mt8195-resets.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/interrupt.h>

#define WDT_MAX_TIMEOUT		31
#define WDT_MIN_TIMEOUT		2
#define WDT_LENGTH_TIMEOUT(n)	((n) << 5)

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8

#define WDT_RST			0x08
#define WDT_RST_RELOAD		0x1971

#define WDT_MODE		0x00
#define WDT_MODE_EN		(1 << 0)
#define WDT_MODE_EXT_POL_LOW	(0 << 1)
#define WDT_MODE_EXT_POL_HIGH	(1 << 1)
#define WDT_MODE_EXRST_EN	(1 << 2)
#define WDT_MODE_IRQ_EN		(1 << 3)
#define WDT_MODE_AUTO_START	(1 << 4)
#define WDT_MODE_DUAL_EN	(1 << 6)
#define WDT_MODE_CNT_SEL	(1 << 8)
#define WDT_MODE_KEY		0x22000000

#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

#define WDT_SWSYSRST		0x18U
#define WDT_SWSYS_RST_KEY	0x88000000

#define WDT_SWSYSRST_EN		0xfc

/* MediaTek RGU <-> preloader/LK reset-reason handshake (stock wdt_v2) */
#define WDT_MODE_DDR_RESERVE	(1 << 7)
#define WDT_STATUS		0x0c
#define WDT_NONRST_REG		0x20
#define WDT_NONRST_REG2		0x24
#define WDT_NONRST2_BYPASS_PWR_KEY	(1 << 13)
#define WDT_NONRST2_STAGE_SHIFT	29
#define WDT_NONRST2_STAGE_MASK	(0x7 << WDT_NONRST2_STAGE_SHIFT)
#define RGU_STAGE_KERNEL	0x3
#define DBGTOP_MODE		0x00
#define DBGTOP_MODE_KEY		0x22000000
#define DBGTOP_MODE_DDR_RESERVE	(1 << 0)
#define PMIF_SPIWRAP_EN		0x14

#define DRV_NAME		"mtk-wdt"
#define DRV_VERSION		"1.0"

#define MT7988_TOPRGU_SW_RST_NUM	24

static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;

struct mtk_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	spinlock_t lock; /* protects WDT_SWSYSRST reg */
	struct reset_controller_dev rcdev;
	bool disable_wdt_extrst;
	bool reset_by_toprgu;
	bool has_swsysrst_en;
	bool rgu_handshake;
	void __iomem *dbgtop_base;
	void __iomem *spi_mst_base;
};

static bool mtk_handshake = true;
module_param(mtk_handshake, bool, 0444);
MODULE_PARM_DESC(mtk_handshake,
		 "MT6853: stock-compatible RGU stage/reset-reason handshake and restart (default 1)");

static bool ddr_reserve = true;
module_param(ddr_reserve, bool, 0644);
MODULE_PARM_DESC(ddr_reserve,
		 "MT6853: keep DRAM in self-refresh across a watchdog reset, like stock mrdump (default 1)");

struct mtk_wdt_data {
	int toprgu_sw_rst_num;
	bool has_swsysrst_en;
};

static const struct mtk_wdt_data mt2712_data = {
	.toprgu_sw_rst_num = MT2712_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt6735_data = {
	.toprgu_sw_rst_num = MT6735_TOPRGU_RST_NUM,
};

static const struct mtk_wdt_data mt6795_data = {
	.toprgu_sw_rst_num = MT6795_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt7986_data = {
	.toprgu_sw_rst_num = MT7986_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt7988_data = {
	.toprgu_sw_rst_num = MT7988_TOPRGU_SW_RST_NUM,
	.has_swsysrst_en = true,
};

static const struct mtk_wdt_data mt8183_data = {
	.toprgu_sw_rst_num = MT8183_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8186_data = {
	.toprgu_sw_rst_num = MT8186_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8188_data = {
	.toprgu_sw_rst_num = MT8188_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8192_data = {
	.toprgu_sw_rst_num = MT8192_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8195_data = {
	.toprgu_sw_rst_num = MT8195_TOPRGU_SW_RST_NUM,
};

/**
 * toprgu_reset_sw_en_unlocked() - enable/disable software control for reset bit
 * @data: Pointer to instance of driver data.
 * @id: Bit number identifying the reset to be enabled or disabled.
 * @enable: If true, enable software control for that bit, disable otherwise.
 *
 * Context: The caller must hold lock of struct mtk_wdt_dev.
 */
static void toprgu_reset_sw_en_unlocked(struct mtk_wdt_dev *data,
					unsigned long id, bool enable)
{
	u32 tmp;

	tmp = readl(data->wdt_base + WDT_SWSYSRST_EN);
	if (enable)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);

	writel(tmp, data->wdt_base + WDT_SWSYSRST_EN);
}

static int toprgu_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	unsigned int tmp;
	unsigned long flags;
	struct mtk_wdt_dev *data =
		 container_of(rcdev, struct mtk_wdt_dev, rcdev);

	spin_lock_irqsave(&data->lock, flags);

	if (assert && data->has_swsysrst_en)
		toprgu_reset_sw_en_unlocked(data, id, true);

	tmp = readl(data->wdt_base + WDT_SWSYSRST);
	if (assert)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);
	tmp |= WDT_SWSYS_RST_KEY;
	writel(tmp, data->wdt_base + WDT_SWSYSRST);

	if (!assert && data->has_swsysrst_en)
		toprgu_reset_sw_en_unlocked(data, id, false);

	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int toprgu_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return toprgu_reset_update(rcdev, id, true);
}

static int toprgu_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return toprgu_reset_update(rcdev, id, false);
}

static int toprgu_reset(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	int ret;

	ret = toprgu_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return toprgu_reset_deassert(rcdev, id);
}

static const struct reset_control_ops toprgu_reset_ops = {
	.assert = toprgu_reset_assert,
	.deassert = toprgu_reset_deassert,
	.reset = toprgu_reset,
};

static int toprgu_register_reset_controller(struct platform_device *pdev,
					    int rst_num)
{
	int ret;
	struct mtk_wdt_dev *mtk_wdt = platform_get_drvdata(pdev);

	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->rcdev.owner = THIS_MODULE;
	mtk_wdt->rcdev.nr_resets = rst_num;
	mtk_wdt->rcdev.ops = &toprgu_reset_ops;
	mtk_wdt->rcdev.of_node = pdev->dev.of_node;
	ret = devm_reset_controller_register(&pdev->dev, &mtk_wdt->rcdev);
	if (ret != 0)
		dev_err(&pdev->dev,
			"couldn't register wdt reset controller: %d\n", ret);
	return ret;
}

static void mtk_wdt_rgu_mark_stage(void __iomem *wdt_base)
{
	u32 reg = readl(wdt_base + WDT_NONRST_REG2);

	reg &= ~WDT_NONRST2_STAGE_MASK;
	reg |= RGU_STAGE_KERNEL << WDT_NONRST2_STAGE_SHIFT;
	writel(reg, wdt_base + WDT_NONRST_REG2);
}

/*
 * Mirror the stock wdt_arch_reset() sequence so preloader/LK see a clean
 * "SW reset from kernel": stage = KERNEL, NONRST_REG = live WDT_STATUS
 * (preloader treats NONRST_REG as the saved reset reason), no DDR reserve,
 * power-key bypass for plain reboots, PMIC wrapper idle before the reset.
 */
static void mtk_wdt_rgu_prepare_reset(struct mtk_wdt_dev *mtk_wdt,
				      const char *cmd)
{
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	bool bypass_pwrkey = true;
	u32 mode, nonrst2, reg;

	if (cmd && (!strcmp(cmd, "recovery") || !strcmp(cmd, "bootloader") ||
		    !strcmp(cmd, "kpoc") || !strcmp(cmd, "charger")))
		bypass_pwrkey = false;

	writel(WDT_RST_RELOAD, wdt_base + WDT_RST);

	nonrst2 = readl(wdt_base + WDT_NONRST_REG2);
	nonrst2 &= ~(WDT_NONRST2_BYPASS_PWR_KEY | WDT_NONRST2_STAGE_MASK);
	nonrst2 |= RGU_STAGE_KERNEL << WDT_NONRST2_STAGE_SHIFT;
	if (bypass_pwrkey)
		nonrst2 |= WDT_NONRST2_BYPASS_PWR_KEY;
	writel(nonrst2, wdt_base + WDT_NONRST_REG2);

	mode = readl(wdt_base + WDT_MODE);
	mode &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN | WDT_MODE_AUTO_START |
		  WDT_MODE_DDR_RESERVE);
	mode |= WDT_MODE_KEY | WDT_MODE_EXRST_EN;
	writel(mode, wdt_base + WDT_MODE);

	writel(readl(wdt_base + WDT_STATUS), wdt_base + WDT_NONRST_REG);

	if (mtk_wdt->dbgtop_base) {
		reg = readl(mtk_wdt->dbgtop_base + DBGTOP_MODE);
		reg &= ~DBGTOP_MODE_DDR_RESERVE;
		writel(reg | DBGTOP_MODE_KEY, mtk_wdt->dbgtop_base + DBGTOP_MODE);
	}

	pr_emerg("mtk-wdt: rgu reset cmd=%s mode=%08x nonrst=%08x nonrst2=%08x\n",
		 cmd ? cmd : "none", readl(wdt_base + WDT_MODE),
		 readl(wdt_base + WDT_NONRST_REG),
		 readl(wdt_base + WDT_NONRST_REG2));

	udelay(100);

	if (mtk_wdt->spi_mst_base) {
		writel(0, mtk_wdt->spi_mst_base + PMIF_SPIWRAP_EN);
		udelay(10);
	}
}

static int mtk_wdt_restart(struct watchdog_device *wdt_dev,
			   unsigned long action, void *data)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;
	u32 reg;

	wdt_base = mtk_wdt->wdt_base;

	if (mtk_wdt->rgu_handshake)
		mtk_wdt_rgu_prepare_reset(mtk_wdt, data);

	/* Enable reset in order to issue a system reset instead of an IRQ */
	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_IRQ_EN;
	writel(reg | WDT_MODE_KEY, wdt_base + WDT_MODE);

	while (1) {
		writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
		mdelay(5);
	}

	return 0;
}

static int mtk_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;

	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);
	pr_debug("mtk-wdt: ping mode=%08x len=%08x\n",
			    readl(wdt_base + WDT_MODE),
			    readl(wdt_base + WDT_LENGTH));

	return 0;
}

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	wdt_dev->timeout = timeout;
	/*
	 * In dual mode, irq will be triggered at timeout / 2
	 * the real timeout occurs at timeout
	 */
	if (wdt_dev->pretimeout)
		wdt_dev->pretimeout = timeout / 2;

	/*
	 * One bit is the value of 512 ticks
	 * The clock has 32 KHz
	 */
	reg = WDT_LENGTH_TIMEOUT((timeout - wdt_dev->pretimeout) << 6)
			| WDT_LENGTH_KEY;
	iowrite32(reg, wdt_base + WDT_LENGTH);

	mtk_wdt_ping(wdt_dev);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev);
static int mtk_wdt_stop(struct watchdog_device *wdt_dev);
static bool diag_stop;
module_param(diag_stop, bool, 0444);

static void mtk_wdt_init(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	/*
	 * The bootloader may leave the watchdog running in a mode that does
	 * not match our timeout/pretimeout split; reprogram it fully so the
	 * core's keepalive interval matches the real hardware timeout.
	 */
	pr_info("mtk-wdt: boot mode=%08x len=%08x sta=%08x\n",
		readl(wdt_base + WDT_MODE), readl(wdt_base + WDT_LENGTH),
		readl(wdt_base + 0xc));
	if (diag_stop) {
		mtk_wdt_stop(wdt_dev);
		pr_info("mtk-wdt: diag_stop mode=%08x\n", readl(wdt_base + WDT_MODE));
		return;
	}
	if (readl(wdt_base + WDT_MODE) & WDT_MODE_EN) {
		set_bit(WDOG_HW_RUNNING, &wdt_dev->status);
		mtk_wdt_start(wdt_dev);
	}
}

static int mtk_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_EN;
	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	int ret;

	ret = mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if (ret < 0)
		return ret;

	reg = ioread32(wdt_base + WDT_MODE);
	if (wdt_dev->pretimeout)
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	else
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	if (mtk_wdt->disable_wdt_extrst)
		reg &= ~WDT_MODE_EXRST_EN;
	if (mtk_wdt->reset_by_toprgu)
		reg |= WDT_MODE_CNT_SEL;
	reg |= (WDT_MODE_EN | WDT_MODE_KEY);
	iowrite32(reg, wdt_base + WDT_MODE);

	return 0;
}

static int mtk_wdt_set_pretimeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdd);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg = ioread32(wdt_base + WDT_MODE);

	if (timeout && !wdd->pretimeout) {
		wdd->pretimeout = wdd->timeout / 2;
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else if (!timeout && wdd->pretimeout) {
		wdd->pretimeout = 0;
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else {
		return 0;
	}

	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return mtk_wdt_set_timeout(wdd, wdd->timeout);
}

static irqreturn_t mtk_wdt_isr(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;

	watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static const struct watchdog_info mtk_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_info mtk_wdt_pt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_PRETIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops mtk_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= mtk_wdt_start,
	.stop		= mtk_wdt_stop,
	.ping		= mtk_wdt_ping,
	.set_timeout	= mtk_wdt_set_timeout,
	.set_pretimeout	= mtk_wdt_set_pretimeout,
	.restart	= mtk_wdt_restart,
};

static int mtk_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wdt_dev *mtk_wdt;
	const struct mtk_wdt_data *wdt_data;
	int err, irq;

	mtk_wdt = devm_kzalloc(dev, sizeof(*mtk_wdt), GFP_KERNEL);
	if (!mtk_wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, mtk_wdt);

	mtk_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mtk_wdt->wdt_base))
		return PTR_ERR(mtk_wdt->wdt_base);

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		err = devm_request_irq(&pdev->dev, irq, mtk_wdt_isr, 0, "wdt_bark",
				       &mtk_wdt->wdt_dev);
		if (err)
			return err;

		mtk_wdt->wdt_dev.info = &mtk_wdt_pt_info;
		mtk_wdt->wdt_dev.pretimeout = WDT_MAX_TIMEOUT / 2;
	} else {
		if (irq == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		mtk_wdt->wdt_dev.info = &mtk_wdt_info;
	}

	mtk_wdt->wdt_dev.ops = &mtk_wdt_ops;
	mtk_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT * 1000;
	mtk_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	mtk_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&mtk_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&mtk_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 128);

	if (mtk_handshake &&
	    of_device_is_compatible(dev->of_node, "mediatek,mt6853-wdt")) {
		struct device_node *np;

		mtk_wdt->rgu_handshake = true;
		/* stock resets through the RGU; run before PSCI (129) */
		watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 192);
		mtk_wdt_rgu_mark_stage(mtk_wdt->wdt_base);

		np = of_find_compatible_node(NULL, NULL, "mediatek,dbgtop");
		if (np) {
			mtk_wdt->dbgtop_base = of_iomap(np, 0);
			of_node_put(np);
		}
		np = of_find_compatible_node(NULL, NULL, "mediatek,mt6853-pwrap");
		if (np) {
			int idx = of_property_match_string(np, "reg-names",
							   "spi_mst");

			if (idx >= 0)
				mtk_wdt->spi_mst_base = of_iomap(np, idx);
			of_node_put(np);
		}
		dev_info(dev, "rgu handshake: nonrst=%08x nonrst2=%08x sta=%08x dbgtop=%d spi_mst=%d\n",
			 readl(mtk_wdt->wdt_base + WDT_NONRST_REG),
			 readl(mtk_wdt->wdt_base + WDT_NONRST_REG2),
			 readl(mtk_wdt->wdt_base + WDT_STATUS),
			 !!mtk_wdt->dbgtop_base, !!mtk_wdt->spi_mst_base);

		/*
		 * Without DDR reserve mode a watchdog reset drops DRAM out of
		 * self-refresh and pstore loses the hung boot's console.
		 * A clean restart clears these bits again.
		 */
		if (ddr_reserve) {
			u32 m = readl(mtk_wdt->wdt_base + WDT_MODE);

			writel(m | WDT_MODE_DDR_RESERVE | WDT_MODE_KEY,
			       mtk_wdt->wdt_base + WDT_MODE);
			if (mtk_wdt->dbgtop_base) {
				m = readl(mtk_wdt->dbgtop_base + DBGTOP_MODE);
				writel(m | DBGTOP_MODE_DDR_RESERVE | DBGTOP_MODE_KEY,
				       mtk_wdt->dbgtop_base + DBGTOP_MODE);
			}
			dev_info(dev, "ddr reserve: mode=%08x dbgtop=%08x\n",
				 readl(mtk_wdt->wdt_base + WDT_MODE),
				 mtk_wdt->dbgtop_base ?
				 readl(mtk_wdt->dbgtop_base + DBGTOP_MODE) : 0);
		}
	}

	watchdog_set_drvdata(&mtk_wdt->wdt_dev, mtk_wdt);

	mtk_wdt_init(&mtk_wdt->wdt_dev);

	watchdog_stop_on_reboot(&mtk_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &mtk_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)\n",
		 mtk_wdt->wdt_dev.timeout, nowayout);

	wdt_data = of_device_get_match_data(dev);
	if (wdt_data) {
		err = toprgu_register_reset_controller(pdev,
						       wdt_data->toprgu_sw_rst_num);
		if (err)
			return err;

		mtk_wdt->has_swsysrst_en = wdt_data->has_swsysrst_en;
	}

	mtk_wdt->disable_wdt_extrst =
		of_property_read_bool(dev->of_node, "mediatek,disable-extrst");

	mtk_wdt->reset_by_toprgu =
		of_property_read_bool(dev->of_node, "mediatek,reset-by-toprgu");

	return 0;
}

static int mtk_wdt_suspend(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	/*
	 * The core keeps pinging a bootloader-armed watchdog from a kthread
	 * even when nobody has /dev/watchdog open. That worker is frozen
	 * across system sleep, so the TOPRGU must be stopped here too or it
	 * resets the SoC about 31 seconds into suspend.
	 */
	if (watchdog_active(&mtk_wdt->wdt_dev) ||
	    watchdog_hw_running(&mtk_wdt->wdt_dev))
		mtk_wdt_stop(&mtk_wdt->wdt_dev);

	return 0;
}

static int mtk_wdt_resume(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev) ||
	    watchdog_hw_running(&mtk_wdt->wdt_dev)) {
		mtk_wdt_start(&mtk_wdt->wdt_dev);
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
	}

	return 0;
}

static const struct of_device_id mtk_wdt_dt_ids[] = {
	{ .compatible = "mediatek,mt2712-wdt", .data = &mt2712_data },
	{ .compatible = "mediatek,mt6589-wdt" },
	{ .compatible = "mediatek,mt6735-wdt", .data = &mt6735_data },
	{ .compatible = "mediatek,mt6795-wdt", .data = &mt6795_data },
	{ .compatible = "mediatek,mt7986-wdt", .data = &mt7986_data },
	{ .compatible = "mediatek,mt7988-wdt", .data = &mt7988_data },
	{ .compatible = "mediatek,mt8183-wdt", .data = &mt8183_data },
	{ .compatible = "mediatek,mt8186-wdt", .data = &mt8186_data },
	{ .compatible = "mediatek,mt8188-wdt", .data = &mt8188_data },
	{ .compatible = "mediatek,mt8192-wdt", .data = &mt8192_data },
	{ .compatible = "mediatek,mt8195-wdt", .data = &mt8195_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wdt_dt_ids);

static DEFINE_SIMPLE_DEV_PM_OPS(mtk_wdt_pm_ops,
				mtk_wdt_suspend, mtk_wdt_resume);

static struct platform_driver mtk_wdt_driver = {
	.probe		= mtk_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
		.pm		= pm_sleep_ptr(&mtk_wdt_pm_ops),
		.of_match_table	= mtk_wdt_dt_ids,
	},
};

module_platform_driver(mtk_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Brugger <matthias.bgg@gmail.com>");
MODULE_DESCRIPTION("Mediatek WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
