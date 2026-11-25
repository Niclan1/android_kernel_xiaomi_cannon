// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/linear_range.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mtk-tphy.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/workqueue.h>

#define MT6360_PMU_CHG_CTRL1	0x311
#define MT6360_PMU_CHG_CTRL2	0x312
#define MT6360_PMU_CHG_CTRL3	0x313
#define MT6360_PMU_CHG_CTRL4	0x314
#define MT6360_PMU_CHG_CTRL5	0x315
#define MT6360_PMU_CHG_CTRL6	0x316
#define MT6360_PMU_CHG_CTRL7	0x317
#define MT6360_PMU_CHG_CTRL8	0x318
#define MT6360_PMU_CHG_CTRL9	0x319
#define MT6360_PMU_CHG_CTRL10	0x31A
#define MT6360_PMU_CHG_CTRL12	0x31C
#define MT6360_PMU_CHG_CTRL13	0x31D
#define MT6360_PMU_DEVICE_TYPE	0x322
#define MT6360_PMU_USB_STATUS1	0x327
#define MT6360_PMU_CHG_CTRL18	0x32C
#define MT6360_PMU_CHG_STAT	0x34A
#define MT6360_PMU_CHG_CTRL19	0x361
#define MT6360_PMU_FOD_STAT	0x3E7

/* MT6360_PMU_CHG_CTRL1 */
#define MT6360_OPA_MODE_SHFT	(0)
#define MT6360_OPA_MODE_MASK	BIT(MT6360_OPA_MODE_SHFT)
/* MT6360_PMU_CHG_CTRL2 */
#define MT6360_CHG_EN_MASK	BIT(0)
#define MT6360_IINLMTSEL_SHFT	(2)
#define MT6360_IINLMTSEL_MASK	GENMASK(3, 2)
#define MT6360_TE_EN_MASK	BIT(4)
/* MT6360_PMU_CHG_CTRL3 */
#define MT6360_IAICR_SHFT	(2)
#define MT6360_IAICR_MASK	GENMASK(7, 2)
#define MT6360_ILIM_EN_MASK	BIT(0)
/* MT6360_PMU_CHG_CTRL4 */
#define MT6360_VOREG_SHFT	(1)
#define MT6360_VOREG_MASK	GENMASK(7, 1)
/* MT6360_PMU_CHG_CTRL5 */
#define MT6360_VOBST_MASK	GENMASK(7, 2)
/* MT6360_PMU_CHG_CTRL6 */
#define MT6360_VMIVR_SHFT      (1)
#define MT6360_VMIVR_MASK      GENMASK(7, 1)
/* MT6360_PMU_CHG_CTRL7 */
#define MT6360_ICHG_SHFT	(2)
#define MT6360_ICHG_MASK	GENMASK(7, 2)
/* MT6360_PMU_CHG_CTRL8 */
#define MT6360_IPREC_SHFT	(0)
#define MT6360_IPREC_MASK	GENMASK(3, 0)
/* MT6360_PMU_CHG_CTRL9 */
#define MT6360_IEOC_SHFT	(4)
#define MT6360_IEOC_MASK	GENMASK(7, 4)
/* MT6360_PMU_CHG_CTRL10 */
#define MT6360_OTG_OC_MASK	GENMASK(2, 0)
/* MT6360_PMU_CHG_CTRL12 */
#define MT6360_TMR_EN_MASK	BIT(1)
#define MT6360_CHG_TMR_SHFT	(5)
#define MT6360_CHG_TMR_MASK	GENMASK(7, 5)
/* MT6360_PMU_CHG_CTRL13 */
#define MT6360_CHG_WDT_EN_MASK	BIT(7)
/* MT6360_PMU_DEVICE_TYPE */
#define MT6360_USBCHGEN_MASK	BIT(7)
#define MT6360_DCDTOUTEN_MASK	BIT(6)
/* MT6360_PMU_USB_STATUS1 */
#define MT6360_USB_STATUS_SHFT	(4)
#define MT6360_USB_STATUS_MASK	GENMASK(6, 4)
/* MT6360_PMU_CHG_CTRL18 */
#define MT6360_IRCMP_V_SHFT	(0)
#define MT6360_IRCMP_V_MASK	GENMASK(2, 0)
#define MT6360_IRCMP_R_SHFT	(3)
#define MT6360_IRCMP_R_MASK	GENMASK(5, 3)
/* MT6360_PMU_CHG_STAT */
#define MT6360_CHG_STAT_SHFT	(6)
#define MT6360_CHG_STAT_MASK	GENMASK(7, 6)
#define MT6360_VBAT_LVL_MASK	BIT(5)
/* MT6360_PMU_CHG_CTRL19 */
#define MT6360_VINOVP_SHFT	(5)
#define MT6360_VINOVP_MASK	GENMASK(6, 5)
/* MT6360_PMU_FOD_STAT */
#define MT6360_CHRDET_EXT_MASK	BIT(4)

/* uV */
#define MT6360_VMIVR_MIN	3900000
#define MT6360_VMIVR_MAX	13400000
#define MT6360_VMIVR_STEP	100000
/* uA */
#define MT6360_ICHG_MIN		100000
#define MT6360_ICHG_MAX		5000000
#define MT6360_ICHG_STEP	100000
/* uV */
#define MT6360_VOREG_MIN	3900000
#define MT6360_VOREG_MAX	4710000
#define MT6360_VOREG_STEP	10000
/* uA */
#define MT6360_AICR_MIN		100000
#define MT6360_AICR_MAX		3250000
#define MT6360_AICR_STEP	50000
/* uA */
#define MT6360_IPREC_MIN	100000
#define MT6360_IPREC_MAX	850000
#define MT6360_IPREC_STEP	50000
/* uA */
#define MT6360_IEOC_MIN		100000
#define MT6360_IEOC_MAX		850000
#define MT6360_IEOC_STEP	50000

/*
 * Ceilings enforced on every write, whatever the caller asks for: the stock
 * firmware never exceeds 2 A / 4.45 V on this cell, and IR compensation adds
 * up to 32 mV on top of the programmed voltage.
 */
#define MT6360_ICHG_LIMIT	2000000
#define MT6360_AICR_LIMIT	2000000
#define MT6360_VOREG_LIMIT	4400000
#define MT6360_IPREC_LIMIT	500000

/* Conservative values programmed at probe; the battery driver raises them. */
#define MT6360_ICHG_INIT	500000
#define MT6360_AICR_INIT	500000
#define MT6360_VOREG_INIT	4350000
#define MT6360_VMIVR_INIT	4400000
#define MT6360_IEOC_INIT	200000
#define MT6360_CHG_TMR_12H	4
#define MT6360_IRCMP_R_SEL	1	/* 25 mohm */
#define MT6360_IRCMP_V_SEL	1	/* 32 mV */

enum {
	MT6360_RANGE_VMIVR,
	MT6360_RANGE_ICHG,
	MT6360_RANGE_VOREG,
	MT6360_RANGE_AICR,
	MT6360_RANGE_IPREC,
	MT6360_RANGE_IEOC,
	MT6360_RANGE_MAX,
};

static const struct linear_range mt6360_chg_range[MT6360_RANGE_MAX] = {
	LINEAR_RANGE_IDX(MT6360_RANGE_VMIVR, 3900000, 0, 0x5F, 100000),
	LINEAR_RANGE_IDX(MT6360_RANGE_ICHG, 100000, 0, 0x31, 100000),
	LINEAR_RANGE_IDX(MT6360_RANGE_VOREG, 3900000, 0, 0x51, 10000),
	LINEAR_RANGE_IDX(MT6360_RANGE_AICR, 100000, 0, 0x3F, 50000),
	LINEAR_RANGE_IDX(MT6360_RANGE_IPREC, 100000, 0, 0x0F, 50000),
	LINEAR_RANGE_IDX(MT6360_RANGE_IEOC, 100000, 0, 0x0F, 50000),
};

static bool mt6360_bc12 = true;
module_param_named(bc12, mt6360_bc12, bool, 0444);
MODULE_PARM_DESC(bc12, "Run BC1.2 charger detection when the USB PHY DP/DM handoff is described (default on, otherwise the input stays at 500 mA)");

struct mt6360_chg_info {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *ac_psy;
	struct regulator_dev *otg_rdev;
	struct phy *usb_phy;
	struct mutex chgdet_lock;
	u32 vinovp;
	bool pwr_rdy;
	bool bc12_en;
	bool bc11_sw;
	int psy_usb_type;
	int bc12_type;
	struct work_struct chrdet_work;
	struct delayed_work bc12_wait_work;
	unsigned int bc12_wait_cnt;
	/* DP/DM give-back that failed and is retried for this connection */
	struct delayed_work bc12_release_work;
	unsigned int bc12_gen;
	unsigned int bc12_release_gen;
	unsigned int bc12_release_cnt;
	int bc12_pending_type;
	bool bc12_restart;
	/* set once teardown started, no work or irq may act after it */
	bool stopping;
};

/* Stock waited up to 20 s for the USB controller before touching DP/DM */
#define MT6360_BC12_WAIT_MS	100
#define MT6360_BC12_WAIT_MAX	200
#define MT6360_BC12_RELEASE_MS	200
#define MT6360_BC12_RELEASE_MAX	10

enum mt6360_iinlmtsel {
	MT6360_IINLMTSEL_AICR_3250 = 0,
	MT6360_IINLMTSEL_CHG_TYPE,
	MT6360_IINLMTSEL_AICR,
	MT6360_IINLMTSEL_LOWER_LEVEL,
};

enum mt6360_pmu_chg_type {
	MT6360_CHG_TYPE_NOVBUS = 0,
	MT6360_CHG_TYPE_UNDER_GOING,
	MT6360_CHG_TYPE_SDP,
	MT6360_CHG_TYPE_SDPNSTD,
	MT6360_CHG_TYPE_DCP,
	MT6360_CHG_TYPE_CDP,
	MT6360_CHG_TYPE_DISABLE_BC12,
	MT6360_CHG_TYPE_MAX,
};

static int mt6360_get_chrdet_ext_stat(struct mt6360_chg_info *mci,
					     bool *pwr_rdy)
{
	int ret;
	unsigned int regval;

	ret = regmap_read(mci->regmap, MT6360_PMU_FOD_STAT, &regval);
	if (ret < 0)
		return ret;
	*pwr_rdy = (regval & MT6360_CHRDET_EXT_MASK) ? true : false;
	return 0;
}

static bool mt6360_chg_is_ac_type(int bc12_type)
{
	return bc12_type == MT6360_CHG_TYPE_DCP ||
	       bc12_type == MT6360_CHG_TYPE_SDPNSTD;
}

/* "usb" is online for SDP/CDP and while the type is still unknown */
static int mt6360_charger_get_online(struct mt6360_chg_info *mci,
				     union power_supply_propval *val)
{
	int ret;
	bool pwr_rdy;

	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret < 0)
		return ret;
	val->intval = pwr_rdy && !mt6360_chg_is_ac_type(READ_ONCE(mci->bc12_type));
	return 0;
}

static int mt6360_ac_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct mt6360_chg_info *mci = power_supply_get_drvdata(psy);
	bool pwr_rdy;
	int ret;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret < 0)
		return ret;
	val->intval = pwr_rdy && mt6360_chg_is_ac_type(READ_ONCE(mci->bc12_type));
	return 0;
}

static int mt6360_charger_get_status(struct mt6360_chg_info *mci,
				     union power_supply_propval *val)
{
	int status, ret;
	unsigned int regval;
	bool pwr_rdy;

	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret < 0)
		return ret;
	if (!pwr_rdy) {
		status = POWER_SUPPLY_STATUS_DISCHARGING;
		goto out;
	}

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_STAT, &regval);
	if (ret < 0)
		return ret;
	regval &= MT6360_CHG_STAT_MASK;
	regval >>= MT6360_CHG_STAT_SHFT;
	switch (regval) {
	case 0x1:
		status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x2:
		status = POWER_SUPPLY_STATUS_FULL;
		break;
	default: /* not charging or charge fault */
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	}
out:
	val->intval = status;
	return 0;
}

static int mt6360_charger_get_charge_type(struct mt6360_chg_info *mci,
					  union power_supply_propval *val)
{
	int type, ret;
	unsigned int regval;
	u8 chg_stat;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_STAT, &regval);
	if (ret < 0)
		return ret;

	chg_stat = (regval & MT6360_CHG_STAT_MASK) >> MT6360_CHG_STAT_SHFT;
	switch (chg_stat) {
	case 0x01: /* Charge in Progress */
		if (regval & MT6360_VBAT_LVL_MASK)
			type = POWER_SUPPLY_CHARGE_TYPE_FAST;
		else
			type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case 0x00: /* Not Charging */
	case 0x02: /* Charge Done */
	case 0x03: /* Charge Fault */
	default:
		type = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	}

	val->intval = type;
	return 0;
}

static int mt6360_charger_get_ichg(struct mt6360_chg_info *mci,
				   union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL7, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_ICHG_MASK) >> MT6360_ICHG_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_ICHG], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_max_ichg(struct mt6360_chg_info *mci,
				       union power_supply_propval *val)
{
	val->intval = MT6360_ICHG_LIMIT;
	return 0;
}

static int mt6360_charger_get_cv(struct mt6360_chg_info *mci,
				 union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL4, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_VOREG_MASK) >> MT6360_VOREG_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_VOREG], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_max_cv(struct mt6360_chg_info *mci,
				     union power_supply_propval *val)
{
	val->intval = MT6360_VOREG_LIMIT;
	return 0;
}

static int mt6360_charger_get_aicr(struct mt6360_chg_info *mci,
				   union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL3, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_IAICR_MASK) >> MT6360_IAICR_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_AICR], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_mivr(struct mt6360_chg_info *mci,
				   union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL6, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_VMIVR_MASK) >> MT6360_VMIVR_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_VMIVR], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_iprechg(struct mt6360_chg_info *mci,
				      union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL8, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_IPREC_MASK) >> MT6360_IPREC_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_IPREC], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_ieoc(struct mt6360_chg_info *mci,
				   union power_supply_propval *val)
{
	int ret;
	u32 sel, value;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL9, &sel);
	if (ret < 0)
		return ret;
	sel = (sel & MT6360_IEOC_MASK) >> MT6360_IEOC_SHFT;
	ret = linear_range_get_value(&mt6360_chg_range[MT6360_RANGE_IEOC], sel, &value);
	if (!ret)
		val->intval = value;
	return ret;
}

static int mt6360_charger_get_behaviour(struct mt6360_chg_info *mci,
					union power_supply_propval *val)
{
	int ret;
	u32 regval;

	ret = regmap_read(mci->regmap, MT6360_PMU_CHG_CTRL2, &regval);
	if (ret < 0)
		return ret;
	val->intval = (regval & MT6360_CHG_EN_MASK) ?
		      POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO :
		      POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
	return 0;
}

static int mt6360_charger_set_behaviour(struct mt6360_chg_info *mci,
					const union power_supply_propval *val)
{
	switch (val->intval) {
	case POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO:
		return regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL2,
					  MT6360_CHG_EN_MASK, MT6360_CHG_EN_MASK);
	case POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE:
		return regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL2,
					  MT6360_CHG_EN_MASK, 0);
	default:
		return -EINVAL;
	}
}

/* A negative value would wrap to the top of the range, so refuse it */
static int mt6360_chg_get_sel(int range, int val, int limit, u32 *sel)
{
	if (val < 0)
		return -EINVAL;

	linear_range_get_selector_within(&mt6360_chg_range[range],
					 min(val, limit), sel);
	return 0;
}

static int mt6360_charger_set_ichg(struct mt6360_chg_info *mci,
				   const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_ICHG, val->intval,
				 MT6360_ICHG_LIMIT, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL7,
				  MT6360_ICHG_MASK,
				  sel << MT6360_ICHG_SHFT);
}

static int mt6360_charger_set_cv(struct mt6360_chg_info *mci,
				 const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_VOREG, val->intval,
				 MT6360_VOREG_LIMIT, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL4,
				  MT6360_VOREG_MASK,
				  sel << MT6360_VOREG_SHFT);
}

static int mt6360_charger_set_aicr(struct mt6360_chg_info *mci,
				   const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_AICR, val->intval,
				 MT6360_AICR_LIMIT, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL3,
				  MT6360_IAICR_MASK,
				  sel << MT6360_IAICR_SHFT);
}

static int mt6360_charger_set_mivr(struct mt6360_chg_info *mci,
				   const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_VMIVR, val->intval,
				 MT6360_VMIVR_MAX, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL6,
				  MT6360_VMIVR_MASK,
				  sel << MT6360_VMIVR_SHFT);
}

static int mt6360_charger_set_iprechg(struct mt6360_chg_info *mci,
				      const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_IPREC, val->intval,
				 MT6360_IPREC_LIMIT, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL8,
				  MT6360_IPREC_MASK,
				  sel << MT6360_IPREC_SHFT);
}

static int mt6360_charger_set_ieoc(struct mt6360_chg_info *mci,
				   const union power_supply_propval *val)
{
	u32 sel;
	int ret;

	ret = mt6360_chg_get_sel(MT6360_RANGE_IEOC, val->intval,
				 MT6360_IEOC_MAX, &sel);
	if (ret)
		return ret;
	return regmap_update_bits(mci->regmap,
				  MT6360_PMU_CHG_CTRL9,
				  MT6360_IEOC_MASK,
				  sel << MT6360_IEOC_SHFT);
}

static int mt6360_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6360_chg_info *mci = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = mt6360_charger_get_online(mci, val);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = mt6360_charger_get_status(mci, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = mt6360_charger_get_charge_type(mci, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		ret = mt6360_charger_get_behaviour(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = mt6360_charger_get_ichg(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = mt6360_charger_get_max_ichg(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = mt6360_charger_get_cv(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = mt6360_charger_get_max_cv(mci, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = mt6360_charger_get_aicr(mci, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		ret = mt6360_charger_get_mivr(mci, val);
		break;
	case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
		ret = mt6360_charger_get_iprechg(mci, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = mt6360_charger_get_ieoc(mci, val);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = mci->psy_usb_type;
		break;
	default:
		ret = -ENODATA;
	}
	return ret;
}

static int mt6360_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct mt6360_chg_info *mci = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		ret = mt6360_charger_set_behaviour(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = mt6360_charger_set_ichg(mci, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = mt6360_charger_set_cv(mci, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = mt6360_charger_set_aicr(mci, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		ret = mt6360_charger_set_mivr(mci, val);
		break;
	case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
		ret = mt6360_charger_set_iprechg(mci, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = mt6360_charger_set_ieoc(mci, val);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int mt6360_charger_property_is_writeable(struct power_supply *psy,
					       enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
	case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property mt6360_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,
	POWER_SUPPLY_PROP_PRECHARGE_CURRENT,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

/* Android and the stock init scripts look the supplies up by these names */
static const struct power_supply_desc mt6360_charger_desc = {
	.name			= "usb",
	.type			= POWER_SUPPLY_TYPE_USB,
	.properties		= mt6360_charger_properties,
	.num_properties		= ARRAY_SIZE(mt6360_charger_properties),
	.get_property		= mt6360_charger_get_property,
	.set_property		= mt6360_charger_set_property,
	.property_is_writeable	= mt6360_charger_property_is_writeable,
	.charge_behaviours	= BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
				  BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
	.usb_types		= BIT(POWER_SUPPLY_USB_TYPE_SDP) |
				  BIT(POWER_SUPPLY_USB_TYPE_CDP) |
				  BIT(POWER_SUPPLY_USB_TYPE_DCP) |
				  BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN),
};

static enum power_supply_property mt6360_ac_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc mt6360_ac_desc = {
	.name			= "ac",
	.type			= POWER_SUPPLY_TYPE_MAINS,
	.properties		= mt6360_ac_properties,
	.num_properties		= ARRAY_SIZE(mt6360_ac_properties),
	.get_property		= mt6360_ac_get_property,
};

static char *mt6360_chg_supplicants[] = { "battery" };

static const struct regulator_ops mt6360_chg_otg_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
};

static const struct regulator_desc mt6360_otg_rdesc = {
	.of_match = "usb-otg-vbus-regulator",
	.name = "usb-otg-vbus",
	.ops = &mt6360_chg_otg_ops,
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.min_uV = 4425000,
	.uV_step = 25000,
	.n_voltages = 57,
	.vsel_reg = MT6360_PMU_CHG_CTRL5,
	.vsel_mask = MT6360_VOBST_MASK,
	.enable_reg = MT6360_PMU_CHG_CTRL1,
	.enable_mask = MT6360_OPA_MODE_MASK,
};

static void mt6360_chg_notify(struct mt6360_chg_info *mci)
{
	power_supply_changed(mci->psy);
	power_supply_changed(mci->ac_psy);
}

/*
 * Input current limits by detected port type, as the stock charger DT used:
 * non_std_ac 1 A, charging_host 1.5 A and ac_charger_input 1.5 A. SDP stays
 * at the 500 mA USB2 budget.
 */
static int mt6360_chg_aicr_for_type(int bc12_type)
{
	switch (bc12_type) {
	case MT6360_CHG_TYPE_SDPNSTD:
		return 1000000;
	case MT6360_CHG_TYPE_CDP:
		return 1500000;
	case MT6360_CHG_TYPE_DCP:
		return MT6360_AICR_LIMIT;
	default:
		return MT6360_AICR_INIT;
	}
}

static void mt6360_chg_apply_aicr(struct mt6360_chg_info *mci, int bc12_type)
{
	union power_supply_propval val = {
		.intval = mt6360_chg_aicr_for_type(bc12_type),
	};
	int ret;

	ret = mt6360_charger_set_aicr(mci, &val);
	if (ret)
		dev_warn(mci->dev, "Failed to set input current limit: %d\n", ret);
}

/*
 * Hand DP/DM to the MT6360 detector, or back to the USB controller. The
 * handoff keeps whatever role the controller set; the PHY refuses a stale one.
 */
static int mt6360_chg_set_bc11_sw(struct mt6360_chg_info *mci, bool on)
{
	int ret;

	if (!mci->usb_phy || mci->bc11_sw == on)
		return 0;
	ret = phy_set_mode_ext(mci->usb_phy, phy_get_mode(mci->usb_phy),
			       on ? MTK_TPHY_USB_SUBMODE_BC11_ON :
				    MTK_TPHY_USB_SUBMODE_BC11_OFF);
	if (ret) {
		dev_err(mci->dev, "Failed to %s DP/DM for BC1.2: %d\n",
			on ? "take" : "release", ret);
		return ret;
	}
	mci->bc11_sw = on;
	return 0;
}

/* Detector off and DP/DM back with the USB controller */
static int mt6360_chg_bc12_stop(struct mt6360_chg_info *mci)
{
	int ret, ret2;

	ret = regmap_update_bits(mci->regmap, MT6360_PMU_DEVICE_TYPE,
				 MT6360_USBCHGEN_MASK, 0);
	if (!ret)
		mci->bc12_en = false;
	ret2 = mt6360_chg_set_bc11_sw(mci, false);
	return ret2 ?: ret;
}

static int mt6360_chg_bc12_start(struct mt6360_chg_info *mci)
{
	unsigned int regval;
	bool pwr_rdy;
	int ret;

	/* Without the DCD timeout the detector needs the contact settle time */
	ret = regmap_read(mci->regmap, MT6360_PMU_DEVICE_TYPE, &regval);
	if (ret)
		return ret;
	if (!(regval & MT6360_DCDTOUTEN_MASK))
		msleep(180);

	/* the cable may have gone while waiting */
	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret)
		return ret;
	if (!pwr_rdy)
		return 0;

	ret = mt6360_chg_set_bc11_sw(mci, true);
	if (ret)
		return ret;

	ret = regmap_update_bits(mci->regmap, MT6360_PMU_DEVICE_TYPE,
				 MT6360_USBCHGEN_MASK, MT6360_USBCHGEN_MASK);
	if (ret) {
		mt6360_chg_set_bc11_sw(mci, false);
		return ret;
	}

	mci->bc12_en = true;
	return 0;
}

/* errors the PHY returns for a request it can never honour */
static bool mt6360_chg_phy_err_permanent(int ret)
{
	return ret == -EINVAL || ret == -EOPNOTSUPP || ret == -ENODEV;
}

/*
 * A failed give-back leaves the input at the default limit and is retried a
 * bounded number of times for the current connection only.
 */
static void mt6360_chg_bc12_retry(struct mt6360_chg_info *mci, int ret)
{
	if (mci->stopping)
		return;
	if (mt6360_chg_phy_err_permanent(ret) ||
	    mci->bc12_release_cnt >= MT6360_BC12_RELEASE_MAX) {
		dev_err(mci->dev, "DP/DM not returned to USB: %d, BC1.2 result dropped, input stays at %d uA\n",
			ret, MT6360_AICR_INIT);
		mci->bc12_pending_type = MT6360_CHG_TYPE_NOVBUS;
		mci->bc12_restart = false;
		return;
	}
	mci->bc12_release_cnt++;
	mci->bc12_release_gen = mci->bc12_gen;
	mod_delayed_work(system_wq, &mci->bc12_release_work,
			 msecs_to_jiffies(MT6360_BC12_RELEASE_MS));
}

/* A result only counts for a live VBUS with the port not in the host role */
static bool mt6360_chg_bc12_live(struct mt6360_chg_info *mci)
{
	bool pwr_rdy;
	int ret;

	if (mci->stopping || !mci->pwr_rdy)
		return false;
	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret) {
		dev_err(mci->dev, "BC1.2 VBUS re-check failed: %d\n", ret);
		return false;
	}
	if (!pwr_rdy)
		return false;
	/* only a controller that set the device role owns the result */
	return phy_get_mode(mci->usb_phy) == PHY_MODE_USB_DEVICE;
}

static void mt6360_chg_bc12_publish(struct mt6360_chg_info *mci, int bc12_type)
{
	int last_usb_type = mci->psy_usb_type;
	int last_bc12_type = mci->bc12_type;

	switch (bc12_type) {
	case MT6360_CHG_TYPE_CDP:
		mci->psy_usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		break;
	case MT6360_CHG_TYPE_DCP:
		mci->psy_usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		break;
	default:
		mci->psy_usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		break;
	}

	dev_info(mci->dev, "BC1.2 port type %d, usb_type %d\n", bc12_type,
		 mci->psy_usb_type);
	WRITE_ONCE(mci->bc12_type, bc12_type);
	mt6360_chg_apply_aicr(mci, bc12_type);
	if (last_usb_type != mci->psy_usb_type || last_bc12_type != bc12_type)
		mt6360_chg_notify(mci);
}

/*
 * Detector off and DP/DM back with the USB controller, then publish what is
 * pending for this connection, or restart a detection the give-back blocked.
 */
static void mt6360_chg_bc12_giveback(struct mt6360_chg_info *mci)
{
	int type = mci->bc12_pending_type;
	int ret;

	ret = mt6360_chg_bc12_stop(mci);
	if (ret) {
		dev_warn(mci->dev, "BC1.2 stop failed, input stays at %d uA: %d\n",
			 MT6360_AICR_INIT, ret);
		mt6360_chg_bc12_retry(mci, ret);
		return;
	}

	mci->bc12_pending_type = MT6360_CHG_TYPE_NOVBUS;
	if (type != MT6360_CHG_TYPE_NOVBUS) {
		if (mt6360_chg_bc12_live(mci))
			mt6360_chg_bc12_publish(mci, type);
		else
			dev_info(mci->dev, "BC1.2 port type %d dropped, no VBUS or not in device role\n",
				 type);
	} else if (mci->bc12_restart) {
		mci->bc12_restart = false;
		if (mci->pwr_rdy && !mci->stopping) {
			mci->bc12_wait_cnt = 0;
			mod_delayed_work(system_wq, &mci->bc12_wait_work, 0);
		}
	}
}

static irqreturn_t mt6360_pmu_attach_i_handler(int irq, void *data)
{
	struct mt6360_chg_info *mci = data;
	unsigned int usb_status;
	bool publish = false;
	int ret;

	mutex_lock(&mci->chgdet_lock);
	if (mci->stopping || !mci->bc12_en) {
		dev_dbg(mci->dev, "Received attach interrupt, bc12 disabled, ignore irq\n");
		goto out;
	}

	ret = regmap_read(mci->regmap, MT6360_PMU_USB_STATUS1, &usb_status);
	if (ret < 0) {
		dev_err(mci->dev, "Failed to read BC1.2 result: %d, input stays at %d uA\n",
			ret, MT6360_AICR_INIT);
		usb_status = MT6360_CHG_TYPE_NOVBUS;
	} else {
		usb_status &= MT6360_USB_STATUS_MASK;
		usb_status >>= MT6360_USB_STATUS_SHFT;
	}

	/*
	 * The gadget pull-up re-triggers the detector after a DCP was found;
	 * a dedicated charger stays one until VBUS goes away.
	 */
	if (READ_ONCE(mci->bc12_type) == MT6360_CHG_TYPE_DCP &&
	    usb_status != MT6360_CHG_TYPE_DCP &&
	    usb_status != MT6360_CHG_TYPE_UNDER_GOING) {
		dev_info(mci->dev, "BC1.2 port type %u ignored, DCP already found\n",
			 usb_status);
		mci->bc12_pending_type = MT6360_CHG_TYPE_NOVBUS;
		mci->bc12_restart = false;
		mci->bc12_release_cnt = 0;
		mt6360_chg_bc12_giveback(mci);
		goto out;
	}

	switch (usb_status) {
	case MT6360_CHG_TYPE_UNDER_GOING:
		dev_dbg(mci->dev, "Received attach interrupt, under going...\n");
		goto out;
	case MT6360_CHG_TYPE_SDP:
	case MT6360_CHG_TYPE_SDPNSTD:
	case MT6360_CHG_TYPE_CDP:
	case MT6360_CHG_TYPE_DCP:
		publish = mt6360_chg_bc12_live(mci);
		if (!publish)
			dev_info(mci->dev, "BC1.2 port type %u dropped, no device VBUS\n",
				 usb_status);
		break;
	default:
		dev_dbg(mci->dev, "Received attach interrupt, status %u\n", usb_status);
		break;
	}

	/* a DCP shorts DP/DM, everything else needs the USB controller back first */
	if (publish && usb_status == MT6360_CHG_TYPE_DCP) {
		mt6360_chg_bc12_publish(mci, usb_status);
		goto out;
	}

	mci->bc12_pending_type = publish ? usb_status : MT6360_CHG_TYPE_NOVBUS;
	mci->bc12_restart = false;
	mci->bc12_release_cnt = 0;
	mt6360_chg_bc12_giveback(mci);
out:
	mutex_unlock(&mci->chgdet_lock);
	return IRQ_HANDLED;
}

static void mt6360_handle_chrdet_ext_evt(struct mt6360_chg_info *mci)
{
	int ret;
	bool pwr_rdy;

	mutex_lock(&mci->chgdet_lock);
	if (mci->stopping)
		goto out;

	ret = mt6360_get_chrdet_ext_stat(mci, &pwr_rdy);
	if (ret < 0)
		goto out;

	if (mci->pwr_rdy == pwr_rdy) {
		dev_dbg(mci->dev, "Received vbus interrupt, pwr_rdy is same(%d)\n", pwr_rdy);
		goto out;
	}
	mci->pwr_rdy = pwr_rdy;
	dev_dbg(mci->dev, "Received vbus interrupt, pwr_rdy = %d\n", pwr_rdy);
	/* anything still pending belongs to the previous connection */
	mci->bc12_gen++;
	mci->bc12_pending_type = MT6360_CHG_TYPE_NOVBUS;
	mci->bc12_release_cnt = 0;
	mci->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	WRITE_ONCE(mci->bc12_type, MT6360_CHG_TYPE_NOVBUS);
	/* every new connection starts at the lowest input current */
	mt6360_chg_apply_aicr(mci, MT6360_CHG_TYPE_NOVBUS);
	mt6360_chg_notify(mci);

	if (!mci->usb_phy)
		goto out;

	/* restart from a disabled detector so attach_i fires again */
	mci->bc12_restart = pwr_rdy;
	mt6360_chg_bc12_giveback(mci);
out:
	mutex_unlock(&mci->chgdet_lock);
}

static void mt6360_bc12_release_work(struct work_struct *work)
{
	struct mt6360_chg_info *mci = container_of(to_delayed_work(work),
						   struct mt6360_chg_info,
						   bc12_release_work);

	mutex_lock(&mci->chgdet_lock);
	if (!mci->stopping && mci->bc12_release_gen == mci->bc12_gen)
		mt6360_chg_bc12_giveback(mci);
	mutex_unlock(&mci->chgdet_lock);
}

/* The controller owns DP/DM once it initialised its PHY */
static bool mt6360_chg_usb_ready(struct mt6360_chg_info *mci)
{
	/* the controller sets the PHY role after phy_init() */
	return READ_ONCE(mci->usb_phy->init_count) > 0 &&
	       phy_get_mode(mci->usb_phy) != PHY_MODE_INVALID;
}

static void mt6360_bc12_wait_work(struct work_struct *work)
{
	struct mt6360_chg_info *mci = container_of(to_delayed_work(work),
						   struct mt6360_chg_info,
						   bc12_wait_work);
	int ret;

	mutex_lock(&mci->chgdet_lock);
	if (mci->stopping || !mci->pwr_rdy || mci->bc12_en || mci->bc11_sw)
		goto out;

	if (!mt6360_chg_usb_ready(mci)) {
		if (++mci->bc12_wait_cnt < MT6360_BC12_WAIT_MAX) {
			schedule_delayed_work(&mci->bc12_wait_work,
					      msecs_to_jiffies(MT6360_BC12_WAIT_MS));
			goto out;
		}
		dev_warn(mci->dev, "USB not ready, BC1.2 skipped, input stays at %d uA\n",
			 MT6360_AICR_INIT);
		goto out;
	}

	/* a host drives DP/DM itself, only a device port is detected */
	if (phy_get_mode(mci->usb_phy) != PHY_MODE_USB_DEVICE) {
		dev_info(mci->dev, "USB not in device role, BC1.2 skipped\n");
		goto out;
	}

	ret = mt6360_chg_bc12_start(mci);
	if (ret) {
		dev_warn(mci->dev, "BC1.2 start failed, input stays at %d uA: %d\n",
			 MT6360_AICR_INIT, ret);
		mci->bc12_pending_type = MT6360_CHG_TYPE_NOVBUS;
		mci->bc12_restart = false;
		mci->bc12_release_cnt = 0;
		mt6360_chg_bc12_giveback(mci);
	}
out:
	mutex_unlock(&mci->chgdet_lock);
}

static void mt6360_chrdet_work(struct work_struct *work)
{
	struct mt6360_chg_info *mci = (struct mt6360_chg_info *)container_of(
				     work, struct mt6360_chg_info, chrdet_work);

	mt6360_handle_chrdet_ext_evt(mci);
}

static irqreturn_t mt6360_pmu_chrdet_ext_evt_handler(int irq, void *data)
{
	struct mt6360_chg_info *mci = data;

	mt6360_handle_chrdet_ext_evt(mci);
	return IRQ_HANDLED;
}

static int mt6360_chg_irq_register(struct platform_device *pdev)
{
	const struct {
		const char *name;
		irq_handler_t handler;
	} irq_descs[] = {
		{ "attach_i", mt6360_pmu_attach_i_handler },
		{ "chrdet_ext_evt", mt6360_pmu_chrdet_ext_evt_handler }
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(irq_descs); i++) {
		ret = platform_get_irq_byname(pdev, irq_descs[i].name);
		if (ret < 0)
			return ret;

		ret = devm_request_threaded_irq(&pdev->dev, ret, NULL,
						irq_descs[i].handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						irq_descs[i].name,
						platform_get_drvdata(pdev));
		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret, "Failed to request %s irq\n",
					     irq_descs[i].name);
	}

	return 0;
}

static u32 mt6360_vinovp_trans_to_sel(u32 val)
{
	u32 vinovp_tbl[] = { 5500000, 6500000, 11000000, 14500000 };
	int i;

	/* Select the smaller and equal supported value */
	for (i = 0; i < ARRAY_SIZE(vinovp_tbl)-1; i++) {
		if (val < vinovp_tbl[i+1])
			break;
	}
	return i;
}

static int mt6360_chg_init_limits(struct mt6360_chg_info *mci)
{
	static const struct {
		int (*set)(struct mt6360_chg_info *mci,
			   const union power_supply_propval *val);
		int uval;
		const char *what;
	} limits[] = {
		{ mt6360_charger_set_aicr, MT6360_AICR_INIT, "input current" },
		{ mt6360_charger_set_ichg, MT6360_ICHG_INIT, "charge current" },
		{ mt6360_charger_set_cv, MT6360_VOREG_INIT, "charge voltage" },
		{ mt6360_charger_set_mivr, MT6360_VMIVR_INIT, "input voltage limit" },
		{ mt6360_charger_set_ieoc, MT6360_IEOC_INIT, "termination current" },
	};
	union power_supply_propval val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(limits); i++) {
		val.intval = limits[i].uval;
		ret = limits[i].set(mci, &val);
		if (ret)
			return dev_err_probe(mci->dev, ret, "Failed to set %s\n",
					     limits[i].what);
	}

	return 0;
}

static int mt6360_chg_init_setting(struct mt6360_chg_info *mci)
{
	int ret;
	u32 sel;

	sel = mt6360_vinovp_trans_to_sel(mci->vinovp);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL19,
				  MT6360_VINOVP_MASK, sel << MT6360_VINOVP_SHFT);
	if (ret)
		return dev_err_probe(mci->dev, ret, "%s: Failed to apply vinovp\n", __func__);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_DEVICE_TYPE,
				 MT6360_USBCHGEN_MASK, 0);
	if (ret)
		return dev_err_probe(mci->dev, ret, "%s: Failed to disable bc12\n", __func__);
	/* nothing kicks the watchdog, and its expiry would reset the limits below */
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL13,
				 MT6360_CHG_WDT_EN_MASK, 0);
	if (ret)
		return dev_err_probe(mci->dev, ret, "%s: Failed to disable wdt\n", __func__);
	ret = mt6360_chg_init_limits(mci);
	if (ret)
		return ret;
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL2,
				 MT6360_IINLMTSEL_MASK,
				 MT6360_IINLMTSEL_AICR <<
					MT6360_IINLMTSEL_SHFT);
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to switch iinlmtsel to aicr\n", __func__);
	usleep_range(5000, 6000);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL3,
				 MT6360_ILIM_EN_MASK, 0);
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to disable ilim\n", __func__);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL2,
				 MT6360_TE_EN_MASK, MT6360_TE_EN_MASK);
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to enable termination\n", __func__);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL12,
				 MT6360_CHG_TMR_MASK | MT6360_TMR_EN_MASK,
				 (MT6360_CHG_TMR_12H << MT6360_CHG_TMR_SHFT) |
				 MT6360_TMR_EN_MASK);
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to enable safety timer\n", __func__);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL18,
				 MT6360_IRCMP_R_MASK | MT6360_IRCMP_V_MASK,
				 (MT6360_IRCMP_R_SEL << MT6360_IRCMP_R_SHFT) |
				 (MT6360_IRCMP_V_SEL << MT6360_IRCMP_V_SHFT));
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to set ir compensation\n", __func__);
	ret = regmap_update_bits(mci->regmap, MT6360_PMU_CHG_CTRL10,
				 MT6360_OTG_OC_MASK, MT6360_OTG_OC_MASK);
	if (ret)
		return dev_err_probe(mci->dev, ret,
				     "%s: Failed to config otg oc to 3A\n", __func__);
	return 0;
}

/* Last devres step: hand DP/DM back once nothing can run any more */
static void mt6360_chg_bc12_release(void *data)
{
	struct mt6360_chg_info *mci = data;
	int ret;

	mutex_lock(&mci->chgdet_lock);
	mci->stopping = true;
	ret = mt6360_chg_bc12_stop(mci);
	if (ret)
		dev_err(mci->dev, "BC1.2 stop on removal failed: %d\n", ret);
	mutex_unlock(&mci->chgdet_lock);
}

/*
 * Runs after the irqs are freed and before the power supplies go: refuse new
 * work at every entry point, then wait for what is queued or running. A work
 * that took the lock before the flag was set may queue another one, which
 * the cancels below still catch; anything after the flag queues nothing.
 */
static void mt6360_chg_quiesce(void *data)
{
	struct mt6360_chg_info *mci = data;

	mutex_lock(&mci->chgdet_lock);
	mci->stopping = true;
	mutex_unlock(&mci->chgdet_lock);

	cancel_work_sync(&mci->chrdet_work);
	cancel_delayed_work_sync(&mci->bc12_wait_work);
	cancel_delayed_work_sync(&mci->bc12_release_work);
}

static int mt6360_chg_bc12_init(struct mt6360_chg_info *mci)
{
	struct phy *phy;

	if (!mt6360_bc12) {
		dev_info(mci->dev, "BC1.2 disabled by parameter\n");
		return 0;
	}

	phy = devm_phy_optional_get(mci->dev, "usb");
	if (IS_ERR(phy))
		return dev_err_probe(mci->dev, PTR_ERR(phy), "Failed to get usb phy\n");
	if (!phy) {
		dev_info(mci->dev, "No DP/DM handoff described, BC1.2 disabled\n");
		return 0;
	}
	mci->usb_phy = phy;

	return devm_add_action_or_reset(mci->dev, mt6360_chg_bc12_release, mci);
}

static int mt6360_charger_probe(struct platform_device *pdev)
{
	struct mt6360_chg_info *mci;
	struct power_supply_config charger_cfg = {};
	struct power_supply_config ac_cfg = {};
	struct regulator_config config = { };
	int ret;

	mci = devm_kzalloc(&pdev->dev, sizeof(*mci), GFP_KERNEL);
	if (!mci)
		return -ENOMEM;

	mci->dev = &pdev->dev;
	mci->vinovp = 6500000;
	mutex_init(&mci->chgdet_lock);
	platform_set_drvdata(pdev, mci);
	ret = device_property_read_u32(&pdev->dev, "richtek,vinovp-microvolt", &mci->vinovp);
	if (ret)
		dev_warn(&pdev->dev, "Failed to parse vinovp in DT, keep default 6.5v\n");

	mci->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!mci->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV, "Failed to get parent regmap\n");

	ret = mt6360_chg_init_setting(mci);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to initial setting\n");

	INIT_WORK(&mci->chrdet_work, mt6360_chrdet_work);
	INIT_DELAYED_WORK(&mci->bc12_wait_work, mt6360_bc12_wait_work);
	INIT_DELAYED_WORK(&mci->bc12_release_work, mt6360_bc12_release_work);

	ret = mt6360_chg_bc12_init(mci);
	if (ret)
		return ret;

	charger_cfg.drv_data = mci;
	charger_cfg.fwnode = dev_fwnode(&pdev->dev);
	charger_cfg.supplied_to = mt6360_chg_supplicants;
	charger_cfg.num_supplicants = ARRAY_SIZE(mt6360_chg_supplicants);
	mci->psy = devm_power_supply_register(&pdev->dev,
					      &mt6360_charger_desc, &charger_cfg);
	if (IS_ERR(mci->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(mci->psy),
				     "Failed to register power supply dev\n");

	ac_cfg.drv_data = mci;
	ac_cfg.supplied_to = mt6360_chg_supplicants;
	ac_cfg.num_supplicants = ARRAY_SIZE(mt6360_chg_supplicants);
	mci->ac_psy = devm_power_supply_register(&pdev->dev,
						 &mt6360_ac_desc, &ac_cfg);
	if (IS_ERR(mci->ac_psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(mci->ac_psy),
				     "Failed to register ac power supply dev\n");

	/* only the irqs below queue work */
	ret = devm_add_action_or_reset(&pdev->dev, mt6360_chg_quiesce, mci);
	if (ret)
		return ret;

	ret = mt6360_chg_irq_register(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to register irqs\n");

	config.dev = &pdev->dev;
	config.regmap = mci->regmap;
	mci->otg_rdev = devm_regulator_register(&pdev->dev, &mt6360_otg_rdesc,
						&config);
	if (IS_ERR(mci->otg_rdev))
		return PTR_ERR(mci->otg_rdev);

	schedule_work(&mci->chrdet_work);

	return 0;
}

static const struct of_device_id __maybe_unused mt6360_charger_of_id[] = {
	{ .compatible = "mediatek,mt6360-chg", },
	{},
};
MODULE_DEVICE_TABLE(of, mt6360_charger_of_id);

static const struct platform_device_id mt6360_charger_id[] = {
	{ "mt6360-chg", 0 },
	{},
};
MODULE_DEVICE_TABLE(platform, mt6360_charger_id);

static struct platform_driver mt6360_charger_driver = {
	.driver = {
		.name = "mt6360-chg",
		.of_match_table = of_match_ptr(mt6360_charger_of_id),
	},
	.probe = mt6360_charger_probe,
	.id_table = mt6360_charger_id,
};
module_platform_driver(mt6360_charger_driver);

MODULE_AUTHOR("Gene Chen <gene_chen@richtek.com>");
MODULE_DESCRIPTION("MT6360 Charger Driver");
MODULE_LICENSE("GPL");
