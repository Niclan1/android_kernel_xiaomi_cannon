// SPDX-License-Identifier: GPL-2.0
/*
 * Boot gate and platform glue for the MediaTek CCCI (MD1) stack on MT6853.
 *
 * mtk_ccci.hw=0    (default) nothing is registered, no register is touched.
 * mtk_ccci.hw=1 mtk_ccci.power=0
 *                  util/core/ports/ccmni and the modem platform driver are
 *                  registered (device nodes appear) but MD1 is never powered
 *                  and the MD start request from ccci_mdinit is refused.
 * mtk_ccci.hw=1    full stack, ccci_mdinit can boot MD1.
 */

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "mtk_ccci."

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/power_supply.h>
#include <linux/ratelimit.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>

#include "mtk-ccci-hw.h"
#include "ccci_hif_cldma.h"
#include "modem_sys.h"

bool mtk_ccci_hw;
module_param_named(hw, mtk_ccci_hw, bool, 0444);
MODULE_PARM_DESC(hw, "Register the CCCI modem stack (default 0)");

bool mtk_ccci_power = true;
module_param_named(power, mtk_ccci_power, bool, 0444);
MODULE_PARM_DESC(power, "Allow MD1 power-on and boot when hw=1 (default 1)");

int __init ccci_util_init(void);
int __init ccci_init(void);
int __init modem_cd_init(void);

/* ---- MD1 MTCMOS ---- */

DEFINE_RATELIMIT_STATE(mtk_ccci_log_rs, 10 * HZ, 40);

/*
 * The genpd "md" domain is attached for bookkeeping only: one permanent
 * runtime reference keeps genpd (sync_state, unused-domain sweep) away
 * from MD1 and the virtual device is a syscore device so that system
 * suspend does not power-cycle a running modem. genpd keeps a domain on
 * until sync_state, so a runtime put never switches MD1 off; the stock
 * md_start_platform() BROM check needs a real off/on cycle, which is done
 * here directly with the 4.14 MT6853 MTCMOS sequence (spm_mtcmos_ctrl_md1).
 */
#define SPM_POWERON_CONFIG_EN	0x000
#define SPM_PROJECT_CODE_KEY	0x0b160001
#define SPM_SRC_REQ_STA_0	0x114
#define SPM_PWR_STATUS		0x16c
#define SPM_PWR_STATUS_2ND	0x170
#define SPM_MD1_PWR_CON		0x300
#define SPM_MD_EXT_BUCK_ISO_CON	0x398
#define MD1_PWR_STA		BIT(0)
#define MD1_PWR_RST_B		BIT(0)
#define MD1_PWR_ON		BIT(2)
#define MD1_EXT_BUCK_ISO	(BIT(0) | BIT(1))

#define INFRA_PROT_EN_STA1	0x228
#define INFRA_PROT_EN_SET	0x2a0
#define INFRA_PROT_EN_CLR	0x2a4
#define INFRA_PROT_MD		BIT(7)
#define INFRA_PROT_VDNR_SET	0xb84
#define INFRA_PROT_VDNR_CLR	0xb88
#define INFRA_PROT_VDNR_STA1	0xb90
#define INFRA_PROT_VDNR_MD	(BIT(2) | BIT(12) | BIT(20))

#define MD1_POLL_US		10
#define MD1_TIMEOUT_US		20000

static struct platform_device *md1_pwr_pdev;
static struct device *md1_pwr_dev;
static DEFINE_MUTEX(md1_pwr_lock);

static void __iomem *mtk_ccci_spm_base(void)
{
	static void __iomem *spm;
	struct device_node *np;

	if (!spm) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,sleep");
		if (np) {
			spm = of_iomap(np, 0);
			of_node_put(np);
		}
	}
	return spm;
}

static struct regmap *mtk_ccci_infra(void)
{
	static struct regmap *infra;
	struct regmap *map;

	if (!infra) {
		map = syscon_regmap_lookup_by_compatible(
			"mediatek,mt6853-infracfg_ao");
		if (IS_ERR(map)) {
			mtk_ccci_log_err("infracfg_ao regmap: %ld\n",
				PTR_ERR(map));
			return NULL;
		}
		infra = map;
	}
	return infra;
}

/*
 * Stock spm_mtcmos_ctrl_md1 only polls PWR_STATUS: LK sets PWR_ON_2ND
 * (pwr_con=0xd), which nobody clears, so PWR_STATUS_2ND stays 1 after off.
 */
static u32 md1_sta(void __iomem *spm)
{
	return readl(spm + SPM_PWR_STATUS) & MD1_PWR_STA;
}

static void md1_dump_regs(const char *tag)
{
	void __iomem *spm = mtk_ccci_spm_base();
	struct regmap *infra = mtk_ccci_infra();
	u32 prot = 0, vdnr = 0;

	if (!spm)
		return;
	if (infra) {
		regmap_read(infra, INFRA_PROT_EN_STA1, &prot);
		regmap_read(infra, INFRA_PROT_VDNR_STA1, &vdnr);
	}
	pr_info("mtk-ccci: md1 %s: sta=%x/%x pwr_con=%08x buck_iso=%08x prot_sta1=%08x vdnr_sta1=%08x src_req=%08x\n",
		tag, readl(spm + SPM_PWR_STATUS),
		readl(spm + SPM_PWR_STATUS_2ND),
		readl(spm + SPM_MD1_PWR_CON),
		readl(spm + SPM_MD_EXT_BUCK_ISO_CON), prot, vdnr,
		readl(spm + SPM_SRC_REQ_STA_0));
}

void mtk_ccci_md_pwr_dump(const char *tag)
{
	md1_dump_regs(tag);
}

/* MD1 powered, out of reset, not isolated: its registers may be read. */
bool mtk_ccci_md_pwr_is_on(void)
{
	void __iomem *spm = mtk_ccci_spm_base();
	u32 con;

	if (!spm)
		return false;
	con = readl(spm + SPM_MD1_PWR_CON);
	return md1_sta(spm) &&
		(con & (MD1_PWR_ON | MD1_PWR_RST_B)) ==
			(MD1_PWR_ON | MD1_PWR_RST_B) &&
		!(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) & MD1_EXT_BUCK_ISO);
}

int mtk_ccci_md_pwr_init(struct device *mddev)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct device *vdev;
	int ret;

	if (md1_pwr_pdev)
		return 0;
	if (!mtk_ccci_spm_base()) {
		dev_err(mddev, "mtk-ccci: SPM (mediatek,sleep) not mapped\n");
		return -ENODEV;
	}
	np = of_get_child_by_name(mddev->of_node, "md1-power");
	if (!np) {
		dev_err(mddev, "mtk-ccci: no md1-power child node\n");
		return -ENODEV;
	}
	pdev = platform_device_alloc("mtk-ccci-md1-power", PLATFORM_DEVID_NONE);
	if (!pdev) {
		of_node_put(np);
		return -ENOMEM;
	}
	pdev->dev.parent = mddev;
	device_set_node(&pdev->dev, of_fwnode_handle(np));
	ret = platform_device_add(pdev);
	if (ret) {
		of_node_put(np);
		platform_device_put(pdev);
		return ret;
	}
	/* attach_by_id does not power the domain on (plain attach does) */
	vdev = dev_pm_domain_attach_by_id(&pdev->dev, 0);
	if (IS_ERR_OR_NULL(vdev)) {
		ret = vdev ? PTR_ERR(vdev) : -ENODEV;
		dev_err(mddev, "mtk-ccci: md power domain attach failed %d\n",
			ret);
		platform_device_unregister(pdev);
		return ret;
	}
	dev_pm_syscore_device(vdev, true);
	md1_pwr_pdev = pdev;
	md1_pwr_dev = vdev;
	md1_dump_regs("at init");
	/*
	 * LK leaves MD1 on and running its BROM; the domain therefore probed
	 * as on and this reference does not touch the hardware.
	 */
	if (mtk_ccci_power && mtk_ccci_md_pwr_is_on()) {
		ret = pm_runtime_resume_and_get(vdev);
		if (ret)
			pr_err("mtk-ccci: md1 genpd reference failed %d\n", ret);
	}
	pr_info("mtk-ccci: md1 power domain attached (MD1 %s, power=%d)\n",
		mtk_ccci_md_pwr_is_on() ? "on" : "off", mtk_ccci_power);
	return 0;
}

static int md1_bus_protect(struct regmap *infra, bool set)
{
	u32 val;
	int ret;

	if (!set) {
		regmap_write(infra, INFRA_PROT_VDNR_CLR, INFRA_PROT_VDNR_MD);
		regmap_write(infra, INFRA_PROT_EN_CLR, INFRA_PROT_MD);
		return 0;
	}
	regmap_write(infra, INFRA_PROT_EN_SET, INFRA_PROT_MD);
	ret = regmap_read_poll_timeout(infra, INFRA_PROT_EN_STA1, val,
		(val & INFRA_PROT_MD) == INFRA_PROT_MD,
		MD1_POLL_US, MD1_TIMEOUT_US);
	if (ret)
		goto fail;
	regmap_write(infra, INFRA_PROT_VDNR_SET, INFRA_PROT_VDNR_MD);
	ret = regmap_read_poll_timeout(infra, INFRA_PROT_VDNR_STA1, val,
		(val & INFRA_PROT_VDNR_MD) == INFRA_PROT_VDNR_MD,
		MD1_POLL_US, MD1_TIMEOUT_US);
	if (!ret)
		return 0;
fail:
	md1_dump_regs("bus protect timeout");
	md1_bus_protect(infra, false);
	return ret;
}

int mtk_ccci_md_pwr_on(void)
{
	void __iomem *spm = mtk_ccci_spm_base();
	struct regmap *infra = mtk_ccci_infra();
	u32 val;
	int ret;

	if (!mtk_ccci_power) {
		mtk_ccci_log_err("MD1 power-on refused (mtk_ccci.power=0)\n");
		return -EPERM;
	}
	if (!md1_pwr_dev || !spm || !infra)
		return -ENODEV;

	mutex_lock(&md1_pwr_lock);
	if (mtk_ccci_md_pwr_is_on()) {
		/* keep a running BROM untouched, release a stale protect */
		md1_bus_protect(infra, false);
		mtk_ccci_log("MD1 already on\n");
		ret = 0;
		goto out;
	}
	writel(SPM_PROJECT_CODE_KEY, spm + SPM_POWERON_CONFIG_EN);
	writel(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) & ~BIT(0),
		spm + SPM_MD_EXT_BUCK_ISO_CON);
	writel(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) & ~BIT(1),
		spm + SPM_MD_EXT_BUCK_ISO_CON);
	writel(readl(spm + SPM_MD1_PWR_CON) | MD1_PWR_RST_B,
		spm + SPM_MD1_PWR_CON);
	writel(readl(spm + SPM_MD1_PWR_CON) | MD1_PWR_ON,
		spm + SPM_MD1_PWR_CON);
	ret = readx_poll_timeout(md1_sta, spm, val, val,
		MD1_POLL_US, MD1_TIMEOUT_US);
	if (ret) {
		md1_dump_regs("power-on ack timeout");
		writel(readl(spm + SPM_MD1_PWR_CON) & ~MD1_PWR_ON,
			spm + SPM_MD1_PWR_CON);
		writel(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) | MD1_EXT_BUCK_ISO,
			spm + SPM_MD_EXT_BUCK_ISO_CON);
		writel(readl(spm + SPM_MD1_PWR_CON) & ~MD1_PWR_RST_B,
			spm + SPM_MD1_PWR_CON);
		goto out;
	}
	md1_bus_protect(infra, false);
	mtk_ccci_log("MD1 MTCMOS on\n");
out:
	mutex_unlock(&md1_pwr_lock);
	return ret;
}

int mtk_ccci_md_pwr_off(void)
{
	void __iomem *spm = mtk_ccci_spm_base();
	struct regmap *infra = mtk_ccci_infra();
	u32 val;
	int ret;

	if (!mtk_ccci_power || !md1_pwr_dev || !spm || !infra)
		return -ENODEV;

	mutex_lock(&md1_pwr_lock);
	if (!(readl(spm + SPM_MD1_PWR_CON) & MD1_PWR_ON)) {
		ret = 0;
		goto out;
	}
	/* never cut power under an unacknowledged bus: MD1 stays on */
	ret = md1_bus_protect(infra, true);
	if (ret) {
		mtk_ccci_log_err("MD1 bus protect not acked (%d), left on\n",
			ret);
		goto out;
	}
	writel(SPM_PROJECT_CODE_KEY, spm + SPM_POWERON_CONFIG_EN);
	writel(readl(spm + SPM_MD1_PWR_CON) & ~MD1_PWR_ON,
		spm + SPM_MD1_PWR_CON);
	ret = readx_poll_timeout(md1_sta, spm, val, val == 0,
		MD1_POLL_US, MD1_TIMEOUT_US);
	if (ret)
		md1_dump_regs("power-off ack timeout");
	writel(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) | BIT(0),
		spm + SPM_MD_EXT_BUCK_ISO_CON);
	writel(readl(spm + SPM_MD_EXT_BUCK_ISO_CON) | BIT(1),
		spm + SPM_MD_EXT_BUCK_ISO_CON);
	writel(readl(spm + SPM_MD1_PWR_CON) & ~MD1_PWR_RST_B,
		spm + SPM_MD1_PWR_CON);
	mtk_ccci_log("MD1 MTCMOS off%s\n", ret ? " (ack timeout)" : "");
out:
	mutex_unlock(&md1_pwr_lock);
	return ret;
}

/*
 * The 6.18 infracfg_ao provider has no DPMAIF_MAIN gate (IFRAO bank at
 * 0xc0/0xc4/0xc8, bit 26). The DT points "infra-dpmaif-clk" at the
 * topckgen dpmaif_main mux, which keeps the parent running; the leaf CG is
 * handled here until the clock driver grows the gate.
 */
#define IFRAO_CG3_SET		0xc0
#define IFRAO_CG3_CLR		0xc4
#define IFRAO_DPMAIF_MAIN	BIT(26)

void mtk_ccci_dpmaif_cg(bool on)
{
	struct regmap *infra = mtk_ccci_infra();

	if (!infra)
		return;
	regmap_write(infra, on ? IFRAO_CG3_CLR : IFRAO_CG3_SET,
		IFRAO_DPMAIF_MAIN);
	mtk_ccci_log("DPMAIF_MAIN cg %s\n", on ? "ungated" : "gated");
}

/* ---- symbols that other vendor drivers provided on 4.14 ---- */

bool spm_is_md1_sleep(void)
{
	void __iomem *spm = mtk_ccci_spm_base();

	if (!spm)
		return true;
	return !(readl(spm + SPM_SRC_REQ_STA_0) & 0x3f);
}

u32 mt_irq_get_pending(unsigned int irq)
{
	bool pending = false;

	if (irq_get_irqchip_state(irq, IRQCHIP_STATE_PENDING, &pending))
		return 0;
	return pending;
}

signed int battery_get_bat_voltage(void)
{
	union power_supply_propval val = { };
	struct power_supply *psy;
	int ret;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return 0;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
		&val);
	power_supply_put(psy);
	if (ret)
		return 0;
	return val.intval / 1000;
}

int switch_sim_mode(int id, char *buf, unsigned int len)
{
	return 0;
}

unsigned int get_sim_switch_type(void)
{
	return 0;
}

int mbim_start_xmit(struct sk_buff *skb, int ifid)
{
	dev_kfree_skb_any(skb);
	return 0;
}

/* MT6853 uses CCIF + DPMAIF; the CLDMA HIF is not built. */
int ccci_cldma_hif_init(unsigned char hif_id, unsigned char md_id)
{
	pr_err("mtk-ccci: CLDMA HIF requested but not supported\n");
	return -ENODEV;
}

int md_cd_late_init(unsigned char hif_id)
{
	return 0;
}

void cldma_start(unsigned char hif_id) { }
void cldma_stop(unsigned char hif_id) { }
void cldma_stop_for_ee(unsigned char hif_id) { }
void cldma_reset(unsigned char hif_id) { }
void md_cldma_clear(unsigned char hif_id) { }
void md_cd_ccif_allQreset_work(unsigned char hif_id) { }
void md_cd_clear_all_queue(unsigned char hif_id, enum DIRECTION dir) { }

/* ---- boot gate ---- */

static int __init mtk_ccci_gate_init(void)
{
	int ret;

	if (!mtk_ccci_hw) {
		pr_info("mtk-ccci: disabled (mtk_ccci.hw=0)\n");
		return 0;
	}
	pr_info("mtk-ccci: enabled, power=%d\n", mtk_ccci_power);

	ret = ccci_util_init();
	if (ret) {
		pr_err("mtk-ccci: util init failed %d\n", ret);
		return 0;
	}
	ret = ccci_init();
	if (ret) {
		pr_err("mtk-ccci: core init failed %d\n", ret);
		return 0;
	}
	ret = modem_cd_init();
	if (ret)
		pr_err("mtk-ccci: modem driver init failed %d\n", ret);
	return 0;
}
device_initcall(mtk_ccci_gate_init);
