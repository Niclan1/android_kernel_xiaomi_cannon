// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Author: Balsam CHIHI <bchihi@baylibre.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>
#include <dt-bindings/thermal/mediatek,lvts-thermal.h>

#include "../thermal_hwmon.h"

#define LVTS_MONCTL0(__base)	(__base + 0x0000)
#define LVTS_MONCTL1(__base)	(__base + 0x0004)
#define LVTS_MONCTL2(__base)	(__base + 0x0008)
#define LVTS_MONINT(__base)		(__base + 0x000C)
#define LVTS_MONINTSTS(__base)	(__base + 0x0010)
#define LVTS_MONIDET0(__base)	(__base + 0x0014)
#define LVTS_MONIDET1(__base)	(__base + 0x0018)
#define LVTS_MONIDET2(__base)	(__base + 0x001C)
#define LVTS_MONIDET3(__base)	(__base + 0x0020)
#define LVTS_H2NTHRE(__base)	(__base + 0x0024)
#define LVTS_HTHRE(__base)		(__base + 0x0028)
#define LVTS_OFFSETH(__base)	(__base + 0x0030)
#define LVTS_OFFSETL(__base)	(__base + 0x0034)
#define LVTS_MSRCTL0(__base)	(__base + 0x0038)
#define LVTS_MSRCTL1(__base)	(__base + 0x003C)
#define LVTS_TSSEL(__base)		(__base + 0x0040)
#define LVTS_CALSCALE(__base)	(__base + 0x0048)
#define LVTS_ID(__base)			(__base + 0x004C)
#define LVTS_CONFIG(__base)		(__base + 0x0050)
#define LVTS_RDATA0(__base)		(__base + 0x00B0)
#define LVTS_EDATA00(__base)	(__base + 0x0054)
#define LVTS_EDATA01(__base)	(__base + 0x0058)
#define LVTS_EDATA02(__base)	(__base + 0x005C)
#define LVTS_EDATA03(__base)	(__base + 0x0060)
#define LVTS_MSR0(__base)		(__base + 0x0090)
#define LVTS_MSR1(__base)		(__base + 0x0094)
#define LVTS_MSR2(__base)		(__base + 0x0098)
#define LVTS_MSR3(__base)		(__base + 0x009C)
#define LVTS_IMMD0(__base)		(__base + 0x00A0)
#define LVTS_IMMD1(__base)		(__base + 0x00A4)
#define LVTS_IMMD2(__base)		(__base + 0x00A8)
#define LVTS_IMMD3(__base)		(__base + 0x00AC)
#define LVTS_PROTCTL(__base)	(__base + 0x00C0)
#define LVTS_PROTTA(__base)		(__base + 0x00C4)
#define LVTS_PROTTB(__base)		(__base + 0x00C8)
#define LVTS_PROTTC(__base)		(__base + 0x00CC)
#define LVTS_CLKEN(__base)		(__base + 0x00E4)

#define LVTS_HW_FILTER				0x0
#define LVTS_TSSEL_CONF				0x13121110
#define LVTS_CALSCALE_CONF			0x300

/* LVTS_CONFIG status bits of the device (sensor chip) access */
#define LVTS_CONFIG_ACCESS_BUSY			BIT(24)
#define LVTS_CONFIG_SENSING_BUSY		BIT(25)

/* Device registers and values used by the manual RC calibration */
#define LVTS_DEV_REG_RCK_COUNT			0x00
#define LVTS_DEV_REG_COUNTING			0x03
#define LVTS_DEV_REG_SENSOR_SEL			0x0d
#define LVTS_DEV_COUNTING_RCK			0x02
#define LVTS_DEV_SENSOR_SEL_NO_RCK		0x10

/* The efuse normalization data field of the controller is 21 bits wide */
#define LVTS_EDATA_MASK				GENMASK(20, 0)

/* A measured RC count is only trusted within 6.1% of the efuse one */
#define LVTS_RC_MAX_RATIO_PERMILLE		1061

#define LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR0		BIT(3)
#define LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR1		BIT(8)
#define LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR2		BIT(13)
#define LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR3		BIT(25)
#define LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR0		BIT(2)
#define LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR1		BIT(7)
#define LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR2		BIT(12)
#define LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR3		BIT(24)

#define LVTS_INT_SENSOR0			0x0009001F
#define LVTS_INT_SENSOR1			0x001203E0
#define LVTS_INT_SENSOR2			0x00247C00
#define LVTS_INT_SENSOR3			0x1FC00000

#define LVTS_SENSOR_MAX				4
#define LVTS_GOLDEN_TEMP_MAX		62
#define LVTS_GOLDEN_TEMP_DEFAULT	50
#define LVTS_COEFF_A_MT8195			-250460
#define LVTS_COEFF_B_MT8195			250460
#define LVTS_COEFF_A_MT7988			-204650
#define LVTS_COEFF_B_MT7988			204650

/*
 * Limits used by the SoCs flagged fail_soft to reject calibration data and
 * temperature readings that cannot be real (golden temp unit: 0.5 degC).
 */
#define LVTS_GOLDEN_TEMP_MIN_PLAUSIBLE	40
#define LVTS_COUNT_R_MIN_PLAUSIBLE	20000
#define LVTS_COUNT_R_MAX_PLAUSIBLE	50000
/* An efuse RC count is within this factor of the nominal one (rck->def_count_rc) */
#define LVTS_RC_RANGE_FACTOR		2
#define LVTS_TEMP_MIN_PLAUSIBLE		(-20000)
#define LVTS_TEMP_MAX_PLAUSIBLE		150000

#define LVTS_MSR_IMMEDIATE_MODE		0
#define LVTS_MSR_FILTERED_MODE		1

#define LVTS_MSR_READ_TIMEOUT_US	400
#define LVTS_MSR_READ_WAIT_US		(LVTS_MSR_READ_TIMEOUT_US / 2)

#define LVTS_MINIMUM_THRESHOLD		20000

static int golden_temp = LVTS_GOLDEN_TEMP_DEFAULT;
static int golden_temp_offset;

struct lvts_sensor_data {
	int dt_id;
	u8 cal_offsets[3];
};

struct lvts_ctrl_data {
	struct lvts_sensor_data lvts_sensor[LVTS_SENSOR_MAX];
	u8 valid_sensor_mask;
	int offset;
	int mode;
	/* Where the efuse RC count of the controller is, if lvts_data->rck */
	u8 rc_offsets[3];
};

#define VALID_SENSOR_MAP(s0, s1, s2, s3) \
	.valid_sensor_mask = (((s0) ? BIT(0) : 0) | \
			      ((s1) ? BIT(1) : 0) | \
			      ((s2) ? BIT(2) : 0) | \
			      ((s3) ? BIT(3) : 0))

#define lvts_for_each_valid_sensor(i, lvts_ctrl) \
	for ((i) = 0; (i) < LVTS_SENSOR_MAX; (i)++) \
		if (!((lvts_ctrl)->valid_sensor_mask & BIT(i))) \
			continue; \
		else

/*
 * Manual RC calibration of the sensor devices, for the SoCs that do not use
 * the auto RCK command: every device counts its RC oscillator and the efuse
 * reference value of each sensor is scaled with that count.
 */
struct lvts_rck_data {
	u32 write_cmd;			/* write a device register */
	u32 read_cmd;			/* read a device register */
	const u32 *meas_cmd;		/* normal measurement mode, sent once calibrated */
	int num_meas_cmd;
	unsigned int def_count_rc;	/* RC count reference without efuse data */
};

struct lvts_data {
	const struct lvts_ctrl_data *lvts_ctrl;
	const u32 *conn_cmd;
	const u32 *init_cmd;
	int num_lvts_ctrl;
	int num_conn_cmd;
	int num_init_cmd;
	int temp_factor;
	int temp_offset;
	int gt_calib_bit_offset;
	unsigned int def_calibration;
	/*
	 * Sampling timing. The intervals are in period units, they are all
	 * zero (as fast as possible) unless the SoC data says otherwise.
	 */
	u16 period_unit;
	u16 group_interval;
	u16 filter_interval;
	u16 sensor_interval;
	const struct lvts_rck_data *rck;
	/*
	 * fail_soft: never let a missing or implausible calibration, a missing
	 * reset line, a controller that does not answer or an out of range
	 * reading take the whole thermal domain down (or trigger a shutdown):
	 * fall back to def_calibration, skip the broken controller and report
	 * the reading as not available instead.
	 */
	bool fail_soft;
	/*
	 * Optional: indexes of the 32-bit words of the bootloader provided
	 * 'atag,devinfo' blob that make up the calibration stream, used when
	 * the efuse is not described as nvmem cells.
	 */
	const u16 *devinfo_idx;
	int num_devinfo_idx;
	/*
	 * The controller and its devices may lose their configuration while
	 * the system sleeps: run the whole setup again on resume.
	 */
	bool context_lost_in_suspend;
};

struct lvts_sensor {
	struct thermal_zone_device *tz;
	void __iomem *msr;
	void __iomem *base;
	int id;
	int dt_id;
	int low_thresh;
	int high_thresh;
};

struct lvts_ctrl {
	struct lvts_sensor sensors[LVTS_SENSOR_MAX];
	const struct lvts_data *lvts_data;
	u32 calibration[LVTS_SENSOR_MAX];
	u32 count_rc;
	u8 valid_sensor_mask;
	int mode;
	void __iomem *base;
	int low_thresh;
	int high_thresh;
	u32 saved_offsetl;
	u32 saved_offseth;
	u32 saved_monint;
};

struct lvts_domain {
	struct lvts_ctrl *lvts_ctrl;
	struct reset_control *reset;
	struct clk *clk;
	int num_lvts_ctrl;
	void __iomem *base;
	size_t calib_len;
	u8 *calib;
#ifdef CONFIG_DEBUG_FS
	struct dentry *dom_dentry;
#endif
};

#ifdef CONFIG_MTK_LVTS_THERMAL_DEBUGFS

#define LVTS_DEBUG_FS_REGS(__reg)		\
{						\
	.name = __stringify(__reg),		\
	.offset = __reg(0),			\
}

static const struct debugfs_reg32 lvts_regs[] = {
	LVTS_DEBUG_FS_REGS(LVTS_MONCTL0),
	LVTS_DEBUG_FS_REGS(LVTS_MONCTL1),
	LVTS_DEBUG_FS_REGS(LVTS_MONCTL2),
	LVTS_DEBUG_FS_REGS(LVTS_MONINT),
	LVTS_DEBUG_FS_REGS(LVTS_MONINTSTS),
	LVTS_DEBUG_FS_REGS(LVTS_MONIDET0),
	LVTS_DEBUG_FS_REGS(LVTS_MONIDET1),
	LVTS_DEBUG_FS_REGS(LVTS_MONIDET2),
	LVTS_DEBUG_FS_REGS(LVTS_MONIDET3),
	LVTS_DEBUG_FS_REGS(LVTS_H2NTHRE),
	LVTS_DEBUG_FS_REGS(LVTS_HTHRE),
	LVTS_DEBUG_FS_REGS(LVTS_OFFSETH),
	LVTS_DEBUG_FS_REGS(LVTS_OFFSETL),
	LVTS_DEBUG_FS_REGS(LVTS_MSRCTL0),
	LVTS_DEBUG_FS_REGS(LVTS_MSRCTL1),
	LVTS_DEBUG_FS_REGS(LVTS_TSSEL),
	LVTS_DEBUG_FS_REGS(LVTS_CALSCALE),
	LVTS_DEBUG_FS_REGS(LVTS_ID),
	LVTS_DEBUG_FS_REGS(LVTS_CONFIG),
	LVTS_DEBUG_FS_REGS(LVTS_EDATA00),
	LVTS_DEBUG_FS_REGS(LVTS_EDATA01),
	LVTS_DEBUG_FS_REGS(LVTS_EDATA02),
	LVTS_DEBUG_FS_REGS(LVTS_EDATA03),
	LVTS_DEBUG_FS_REGS(LVTS_MSR0),
	LVTS_DEBUG_FS_REGS(LVTS_MSR1),
	LVTS_DEBUG_FS_REGS(LVTS_MSR2),
	LVTS_DEBUG_FS_REGS(LVTS_MSR3),
	LVTS_DEBUG_FS_REGS(LVTS_IMMD0),
	LVTS_DEBUG_FS_REGS(LVTS_IMMD1),
	LVTS_DEBUG_FS_REGS(LVTS_IMMD2),
	LVTS_DEBUG_FS_REGS(LVTS_IMMD3),
	LVTS_DEBUG_FS_REGS(LVTS_PROTCTL),
	LVTS_DEBUG_FS_REGS(LVTS_PROTTA),
	LVTS_DEBUG_FS_REGS(LVTS_PROTTB),
	LVTS_DEBUG_FS_REGS(LVTS_PROTTC),
	LVTS_DEBUG_FS_REGS(LVTS_CLKEN),
};

static void lvts_debugfs_exit(void *data)
{
	struct lvts_domain *lvts_td = data;

	debugfs_remove_recursive(lvts_td->dom_dentry);
}

static int lvts_debugfs_init(struct device *dev, struct lvts_domain *lvts_td)
{
	struct debugfs_regset32 *regset;
	struct lvts_ctrl *lvts_ctrl;
	struct dentry *dentry;
	char name[64];
	int i;

	lvts_td->dom_dentry = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(lvts_td->dom_dentry))
		return 0;

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++) {

		lvts_ctrl = &lvts_td->lvts_ctrl[i];

		sprintf(name, "controller%d", i);
		dentry = debugfs_create_dir(name, lvts_td->dom_dentry);
		if (IS_ERR(dentry))
			continue;

		regset = devm_kzalloc(dev, sizeof(*regset), GFP_KERNEL);
		if (!regset)
			continue;

		regset->base = lvts_ctrl->base;
		regset->regs = lvts_regs;
		regset->nregs = ARRAY_SIZE(lvts_regs);

		debugfs_create_regset32("registers", 0400, dentry, regset);
	}

	return devm_add_action_or_reset(dev, lvts_debugfs_exit, lvts_td);
}

#else

static inline int lvts_debugfs_init(struct device *dev,
				    struct lvts_domain *lvts_td)
{
	return 0;
}

#endif

static int lvts_raw_to_temp(u32 raw_temp, int temp_factor)
{
	int temperature;

	temperature = ((s64)(raw_temp & 0xFFFF) * temp_factor) >> 14;
	temperature += golden_temp_offset;

	return temperature;
}

static u32 lvts_temp_to_raw(int temperature, int temp_factor)
{
	u32 raw_temp = ((s64)(golden_temp_offset - temperature)) << 14;

	raw_temp = div_s64(raw_temp, -temp_factor);

	return raw_temp;
}

static int lvts_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct lvts_sensor *lvts_sensor = thermal_zone_device_priv(tz);
	struct lvts_ctrl *lvts_ctrl = container_of(lvts_sensor, struct lvts_ctrl,
						   sensors[lvts_sensor->id]);
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;
	void __iomem *msr = lvts_sensor->msr;
	u32 value;
	int rc, t;

	/*
	 * Measurement registers:
	 *
	 * LVTS_MSR[0-3] / LVTS_IMMD[0-3]
	 *
	 * Bits:
	 *
	 * 32-17: Unused
	 * 16	: Valid temperature
	 * 15-0	: Raw temperature
	 */
	rc = readl_poll_timeout(msr, value, value & BIT(16),
				LVTS_MSR_READ_WAIT_US, LVTS_MSR_READ_TIMEOUT_US);

	/*
	 * As the thermal zone temperature will read before the
	 * hardware sensor is fully initialized, we have to check the
	 * validity of the temperature returned when reading the
	 * measurement register. The thermal controller will set the
	 * valid bit temperature only when it is totally initialized.
	 *
	 * Otherwise, we may end up with garbage values out of the
	 * functionning temperature and directly jump to a system
	 * shutdown.
	 */
	if (rc)
		return -EAGAIN;

	t = lvts_raw_to_temp(value & 0xFFFF, lvts_data->temp_factor);

	/*
	 * A reading that cannot be real (bad calibration, sensor not
	 * wired, controller in a bad state) is reported as "no data": the
	 * thermal core silently retries and never disables the zone, and
	 * the value can never reach a critical trip.
	 */
	if (lvts_data->fail_soft &&
	    (t < LVTS_TEMP_MIN_PLAUSIBLE || t > LVTS_TEMP_MAX_PLAUSIBLE))
		return -EAGAIN;

	*temp = t;

	return 0;
}

static void lvts_update_irq_mask(struct lvts_ctrl *lvts_ctrl)
{
	static const u32 high_offset_inten_masks[] = {
		LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR0,
		LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR1,
		LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR2,
		LVTS_MONINT_OFFSET_HIGH_INTEN_SENSOR3,
	};
	static const u32 low_offset_inten_masks[] = {
		LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR0,
		LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR1,
		LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR2,
		LVTS_MONINT_OFFSET_LOW_INTEN_SENSOR3,
	};
	u32 value = 0;
	int i;

	value = readl(LVTS_MONINT(lvts_ctrl->base));

	lvts_for_each_valid_sensor(i, lvts_ctrl) {
		if (lvts_ctrl->sensors[i].high_thresh == lvts_ctrl->high_thresh
		    && lvts_ctrl->sensors[i].low_thresh == lvts_ctrl->low_thresh) {
			/*
			 * The minimum threshold needs to be configured in the
			 * OFFSETL register to get working interrupts, but we
			 * don't actually want to generate interrupts when
			 * crossing it.
			 */
			if (lvts_ctrl->low_thresh == -INT_MAX) {
				value &= ~low_offset_inten_masks[i];
				value |= high_offset_inten_masks[i];
			} else {
				value |= low_offset_inten_masks[i] | high_offset_inten_masks[i];
			}
		} else {
			value &= ~(low_offset_inten_masks[i] | high_offset_inten_masks[i]);
		}
	}

	writel(value, LVTS_MONINT(lvts_ctrl->base));
}

static bool lvts_should_update_thresh(struct lvts_ctrl *lvts_ctrl, int high)
{
	int i;

	if (high > lvts_ctrl->high_thresh)
		return true;

	lvts_for_each_valid_sensor(i, lvts_ctrl)
		if (lvts_ctrl->sensors[i].high_thresh == lvts_ctrl->high_thresh
		    && lvts_ctrl->sensors[i].low_thresh == lvts_ctrl->low_thresh)
			return false;

	return true;
}

static int lvts_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct lvts_sensor *lvts_sensor = thermal_zone_device_priv(tz);
	struct lvts_ctrl *lvts_ctrl = container_of(lvts_sensor, struct lvts_ctrl,
						   sensors[lvts_sensor->id]);
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;
	void __iomem *base = lvts_sensor->base;
	u32 raw_low = lvts_temp_to_raw(low != -INT_MAX ? low : LVTS_MINIMUM_THRESHOLD,
				       lvts_data->temp_factor);
	u32 raw_high = lvts_temp_to_raw(high, lvts_data->temp_factor);
	bool should_update_thresh;

	lvts_sensor->low_thresh = low;
	lvts_sensor->high_thresh = high;

	should_update_thresh = lvts_should_update_thresh(lvts_ctrl, high);
	if (should_update_thresh) {
		lvts_ctrl->high_thresh = high;
		lvts_ctrl->low_thresh = low;
	}
	lvts_update_irq_mask(lvts_ctrl);

	if (!should_update_thresh)
		return 0;

	/*
	 * Low offset temperature threshold
	 *
	 * LVTS_OFFSETL
	 *
	 * Bits:
	 *
	 * 14-0 : Raw temperature for threshold
	 */
	pr_debug("%s: Setting low limit temperature interrupt: %d\n",
		 thermal_zone_device_type(tz), low);
	writel(raw_low, LVTS_OFFSETL(base));

	/*
	 * High offset temperature threshold
	 *
	 * LVTS_OFFSETH
	 *
	 * Bits:
	 *
	 * 14-0 : Raw temperature for threshold
	 */
	pr_debug("%s: Setting high limit temperature interrupt: %d\n",
		 thermal_zone_device_type(tz), high);
	writel(raw_high, LVTS_OFFSETH(base));

	return 0;
}

static irqreturn_t lvts_ctrl_irq_handler(struct lvts_ctrl *lvts_ctrl)
{
	irqreturn_t iret = IRQ_NONE;
	u32 value;
	static const u32 masks[] = {
		LVTS_INT_SENSOR0,
		LVTS_INT_SENSOR1,
		LVTS_INT_SENSOR2,
		LVTS_INT_SENSOR3
	};
	int i;

	/*
	 * Interrupt monitoring status
	 *
	 * LVTS_MONINTST
	 *
	 * Bits:
	 *
	 * 31 : Interrupt for stage 3
	 * 30 : Interrupt for stage 2
	 * 29 : Interrupt for state 1
	 * 28 : Interrupt using filter on sensor 3
	 *
	 * 27 : Interrupt using immediate on sensor 3
	 * 26 : Interrupt normal to hot on sensor 3
	 * 25 : Interrupt high offset on sensor 3
	 * 24 : Interrupt low offset on sensor 3
	 *
	 * 23 : Interrupt hot threshold on sensor 3
	 * 22 : Interrupt cold threshold on sensor 3
	 * 21 : Interrupt using filter on sensor 2
	 * 20 : Interrupt using filter on sensor 1
	 *
	 * 19 : Interrupt using filter on sensor 0
	 * 18 : Interrupt using immediate on sensor 2
	 * 17 : Interrupt using immediate on sensor 1
	 * 16 : Interrupt using immediate on sensor 0
	 *
	 * 15 : Interrupt device access timeout interrupt
	 * 14 : Interrupt normal to hot on sensor 2
	 * 13 : Interrupt high offset interrupt on sensor 2
	 * 12 : Interrupt low offset interrupt on sensor 2
	 *
	 * 11 : Interrupt hot threshold on sensor 2
	 * 10 : Interrupt cold threshold on sensor 2
	 *  9 : Interrupt normal to hot on sensor 1
	 *  8 : Interrupt high offset interrupt on sensor 1
	 *
	 *  7 : Interrupt low offset interrupt on sensor 1
	 *  6 : Interrupt hot threshold on sensor 1
	 *  5 : Interrupt cold threshold on sensor 1
	 *  4 : Interrupt normal to hot on sensor 0
	 *
	 *  3 : Interrupt high offset interrupt on sensor 0
	 *  2 : Interrupt low offset interrupt on sensor 0
	 *  1 : Interrupt hot threshold on sensor 0
	 *  0 : Interrupt cold threshold on sensor 0
	 *
	 * We are interested in the sensor(s) responsible of the
	 * interrupt event. We update the thermal framework with the
	 * thermal zone associated with the sensor. The framework will
	 * take care of the rest whatever the kind of interrupt, we
	 * are only interested in which sensor raised the interrupt.
	 *
	 * sensor 3 interrupt: 0001 1111 1100 0000 0000 0000 0000 0000
	 *                  => 0x1FC00000
	 * sensor 2 interrupt: 0000 0000 0010 0100 0111 1100 0000 0000
	 *                  => 0x00247C00
	 * sensor 1 interrupt: 0000 0000 0001 0010 0000 0011 1110 0000
	 *                  => 0X001203E0
	 * sensor 0 interrupt: 0000 0000 0000 1001 0000 0000 0001 1111
	 *                  => 0x0009001F
	 */
	value = readl(LVTS_MONINTSTS(lvts_ctrl->base));

	/*
	 * Let's figure out which sensors raised the interrupt
	 *
	 * NOTE: the masks array must be ordered with the index
	 * corresponding to the sensor id eg. index=0, mask for
	 * sensor0.
	 */
	for (i = 0; i < ARRAY_SIZE(masks); i++) {

		if (!(value & masks[i]))
			continue;

		/* No zone registered for this sensor (no DT zone, or skipped) */
		if (!lvts_ctrl->sensors[i].tz)
			continue;

		thermal_zone_device_update(lvts_ctrl->sensors[i].tz,
					   THERMAL_TRIP_VIOLATED);
		iret = IRQ_HANDLED;
	}

	/*
	 * Write back to clear the interrupt status (W1C)
	 */
	writel(value, LVTS_MONINTSTS(lvts_ctrl->base));

	return iret;
}

/*
 * Temperature interrupt handler. Even if the driver supports more
 * interrupt modes, we use the interrupt when the temperature crosses
 * the hot threshold the way up and the way down (modulo the
 * hysteresis).
 *
 * Each thermal domain has a couple of interrupts, one for hardware
 * reset and another one for all the thermal events happening on the
 * different sensors.
 *
 * The interrupt is configured for thermal events when crossing the
 * hot temperature limit. At each interrupt, we check in every
 * controller if there is an interrupt pending.
 */
static irqreturn_t lvts_irq_handler(int irq, void *data)
{
	struct lvts_domain *lvts_td = data;
	irqreturn_t aux, iret = IRQ_NONE;
	int i;

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++) {

		aux = lvts_ctrl_irq_handler(&lvts_td->lvts_ctrl[i]);
		if (aux != IRQ_HANDLED)
			continue;

		iret = IRQ_HANDLED;
	}

	return iret;
}

static const struct thermal_zone_device_ops lvts_ops = {
	.get_temp = lvts_get_temp,
	.set_trips = lvts_set_trips,
};

static int lvts_sensor_init(struct device *dev, struct lvts_ctrl *lvts_ctrl,
					const struct lvts_ctrl_data *lvts_ctrl_data)
{
	struct lvts_sensor *lvts_sensor = lvts_ctrl->sensors;

	void __iomem *msr_regs[] = {
		LVTS_MSR0(lvts_ctrl->base),
		LVTS_MSR1(lvts_ctrl->base),
		LVTS_MSR2(lvts_ctrl->base),
		LVTS_MSR3(lvts_ctrl->base)
	};

	void __iomem *imm_regs[] = {
		LVTS_IMMD0(lvts_ctrl->base),
		LVTS_IMMD1(lvts_ctrl->base),
		LVTS_IMMD2(lvts_ctrl->base),
		LVTS_IMMD3(lvts_ctrl->base)
	};

	int i;

	lvts_for_each_valid_sensor(i, lvts_ctrl_data) {

		int dt_id = lvts_ctrl_data->lvts_sensor[i].dt_id;

		/*
		 * At this point, we don't know which id matches which
		 * sensor. Let's set arbitrally the id from the index.
		 */
		lvts_sensor[i].id = i;

		/*
		 * The thermal zone registration will set the trip
		 * point interrupt in the thermal controller
		 * register. But this one will be reset in the
		 * initialization after. So we need to post pone the
		 * thermal zone creation after the controller is
		 * setup. For this reason, we store the device tree
		 * node id from the data in the sensor structure
		 */
		lvts_sensor[i].dt_id = dt_id;

		/*
		 * We assign the base address of the thermal
		 * controller as a back pointer. So it will be
		 * accessible from the different thermal framework ops
		 * as we pass the lvts_sensor pointer as thermal zone
		 * private data.
		 */
		lvts_sensor[i].base = lvts_ctrl->base;

		/*
		 * Each sensor has its own register address to read from.
		 */
		lvts_sensor[i].msr = lvts_ctrl_data->mode == LVTS_MSR_IMMEDIATE_MODE ?
			imm_regs[i] : msr_regs[i];

		lvts_sensor[i].low_thresh = INT_MIN;
		lvts_sensor[i].high_thresh = INT_MIN;
	}

	lvts_ctrl->valid_sensor_mask = lvts_ctrl_data->valid_sensor_mask;

	return 0;
}

/*
 * The efuse blob values follows the sensor enumeration per thermal
 * controller. The decoding of the stream is as follow:
 *
 * MT8192 :
 * Stream index map for MCU Domain mt8192 :
 *
 * <-----mcu-tc#0-----> <-----sensor#0----->        <-----sensor#1----->
 *  0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 | 0x08 | 0x09 | 0x0A | 0x0B
 *
 * <-----sensor#2----->        <-----sensor#3----->
 *  0x0C | 0x0D | 0x0E | 0x0F | 0x10 | 0x11 | 0x12 | 0x13
 *
 * <-----sensor#4----->        <-----sensor#5----->        <-----sensor#6----->        <-----sensor#7----->
 *  0x14 | 0x15 | 0x16 | 0x17 | 0x18 | 0x19 | 0x1A | 0x1B | 0x1C | 0x1D | 0x1E | 0x1F | 0x20 | 0x21 | 0x22 | 0x23
 *
 * Stream index map for AP Domain mt8192 :
 *
 * <-----sensor#0----->        <-----sensor#1----->
 *  0x24 | 0x25 | 0x26 | 0x27 | 0x28 | 0x29 | 0x2A | 0x2B
 *
 * <-----sensor#2----->        <-----sensor#3----->
 *  0x2C | 0x2D | 0x2E | 0x2F | 0x30 | 0x31 | 0x32 | 0x33
 *
 * <-----sensor#4----->        <-----sensor#5----->
 *  0x34 | 0x35 | 0x36 | 0x37 | 0x38 | 0x39 | 0x3A | 0x3B
 *
 * <-----sensor#6----->        <-----sensor#7----->        <-----sensor#8----->
 *  0x3C | 0x3D | 0x3E | 0x3F | 0x40 | 0x41 | 0x42 | 0x43 | 0x44 | 0x45 | 0x46 | 0x47
 *
 * MT8195 :
 * Stream index map for MCU Domain mt8195 :
 *
 * <-----mcu-tc#0-----> <-----sensor#0-----> <-----sensor#1----->
 *  0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 | 0x08 | 0x09
 *
 * <-----mcu-tc#1-----> <-----sensor#2-----> <-----sensor#3----->
 *  0x0A | 0x0B | 0x0C | 0x0D | 0x0E | 0x0F | 0x10 | 0x11 | 0x12
 *
 * <-----mcu-tc#2-----> <-----sensor#4-----> <-----sensor#5-----> <-----sensor#6-----> <-----sensor#7----->
 *  0x13 | 0x14 | 0x15 | 0x16 | 0x17 | 0x18 | 0x19 | 0x1A | 0x1B | 0x1C | 0x1D | 0x1E | 0x1F | 0x20 | 0x21
 *
 * Stream index map for AP Domain mt8195 :
 *
 * <-----ap--tc#0-----> <-----sensor#0-----> <-----sensor#1----->
 *  0x22 | 0x23 | 0x24 | 0x25 | 0x26 | 0x27 | 0x28 | 0x29 | 0x2A
 *
 * <-----ap--tc#1-----> <-----sensor#2-----> <-----sensor#3----->
 *  0x2B | 0x2C | 0x2D | 0x2E | 0x2F | 0x30 | 0x31 | 0x32 | 0x33
 *
 * <-----ap--tc#2-----> <-----sensor#4-----> <-----sensor#5-----> <-----sensor#6----->
 *  0x34 | 0x35 | 0x36 | 0x37 | 0x38 | 0x39 | 0x3A | 0x3B | 0x3C | 0x3D | 0x3E | 0x3F
 *
 * <-----ap--tc#3-----> <-----sensor#7-----> <-----sensor#8----->
 *  0x40 | 0x41 | 0x42 | 0x43 | 0x44 | 0x45 | 0x46 | 0x47 | 0x48
 *
 * Note: In some cases, values don't strictly follow a little endian ordering.
 * The data description gives byte offsets constituting each calibration value
 * for each sensor.
 */
static int lvts_calibration_init(struct device *dev, struct lvts_ctrl *lvts_ctrl,
					const struct lvts_ctrl_data *lvts_ctrl_data,
					u8 *efuse_calibration,
					size_t calib_len)
{
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;
	int i;
	u32 gt = 0;

	/* A zero value for gt means that device has invalid efuse data */
	if (efuse_calibration && calib_len >= sizeof(u32))
		gt = (get_unaligned_le32(efuse_calibration) >>
		      lvts_data->gt_calib_bit_offset) & 0xff;

	if (lvts_data->rck) {
		const u8 *rc = lvts_ctrl_data->rc_offsets;

		lvts_ctrl->count_rc = lvts_data->rck->def_count_rc;

		if (gt) {
			if (rc[0] >= calib_len || rc[1] >= calib_len ||
			    rc[2] >= calib_len)
				return -EINVAL;

			lvts_ctrl->count_rc = (efuse_calibration[rc[0]] << 0) +
					      (efuse_calibration[rc[1]] << 8) +
					      (efuse_calibration[rc[2]] << 16);
		}
	}

	lvts_for_each_valid_sensor(i, lvts_ctrl_data) {
		const struct lvts_sensor_data *sensor =
					&lvts_ctrl_data->lvts_sensor[i];

		if (efuse_calibration &&
		    (sensor->cal_offsets[0] >= calib_len ||
		     sensor->cal_offsets[1] >= calib_len ||
		     sensor->cal_offsets[2] >= calib_len))
			return -EINVAL;

		if (gt) {
			lvts_ctrl->calibration[i] =
				(efuse_calibration[sensor->cal_offsets[0]] << 0) +
				(efuse_calibration[sensor->cal_offsets[1]] << 8) +
				(efuse_calibration[sensor->cal_offsets[2]] << 16);
		} else if (lvts_ctrl->lvts_data->def_calibration) {
			lvts_ctrl->calibration[i] = lvts_ctrl->lvts_data->def_calibration;
		} else {
			dev_err(dev, "efuse contains invalid calibration data and no default given.\n");
			return -ENODATA;
		}
	}

	return 0;
}

/*
 * The efuse bytes stream can be split into different chunk of
 * nvmems. This function reads and concatenate those into a single
 * buffer so it can be read sequentially when initializing the
 * calibration data.
 */
static int lvts_calibration_read(struct device *dev, struct lvts_domain *lvts_td,
					const struct lvts_data *lvts_data)
{
	struct device_node *np = dev_of_node(dev);
	struct nvmem_cell *cell;
	struct property *prop;
	const char *cell_name;

	of_property_for_each_string(np, "nvmem-cell-names", prop, cell_name) {
		size_t len;
		u8 *efuse;

		cell = of_nvmem_cell_get(np, cell_name);
		if (IS_ERR(cell)) {
			dev_err(dev, "Failed to get cell '%s'\n", cell_name);
			return PTR_ERR(cell);
		}

		efuse = nvmem_cell_read(cell, &len);

		nvmem_cell_put(cell);

		if (IS_ERR(efuse)) {
			dev_err(dev, "Failed to read cell '%s'\n", cell_name);
			return PTR_ERR(efuse);
		}

		lvts_td->calib = devm_krealloc(dev, lvts_td->calib,
					       lvts_td->calib_len + len, GFP_KERNEL);
		if (!lvts_td->calib) {
			kfree(efuse);
			return -ENOMEM;
		}

		memcpy(lvts_td->calib + lvts_td->calib_len, efuse, len);

		lvts_td->calib_len += len;

		kfree(efuse);
	}

	return 0;
}

static void lvts_calibration_drop(struct device *dev, struct lvts_domain *lvts_td)
{
	devm_kfree(dev, lvts_td->calib);
	lvts_td->calib = NULL;
	lvts_td->calib_len = 0;
}

/*
 * Build the calibration stream from the 'atag,devinfo' property the
 * bootloader adds to /chosen: a 32-bit little endian word count followed
 * by that many raw efuse words. The words listed in devinfo_idx become the
 * 32-bit words of the calibration stream, in that order.
 */
static int lvts_devinfo_read(struct device *dev, struct lvts_domain *lvts_td,
			     const struct lvts_data *lvts_data)
{
	size_t calib_len = lvts_data->num_devinfo_idx * sizeof(u32);
	const u8 *devinfo;
	u32 nr_words;
	u8 *calib;
	int i, len;

	if (!of_chosen)
		return -ENOENT;

	devinfo = of_get_property(of_chosen, "atag,devinfo", &len);
	if (!devinfo || len < sizeof(u32))
		return -ENOENT;

	nr_words = get_unaligned_le32(devinfo);
	if (nr_words > (len - sizeof(u32)) / sizeof(u32))
		return -EINVAL;

	for (i = 0; i < lvts_data->num_devinfo_idx; i++)
		if (lvts_data->devinfo_idx[i] >= nr_words)
			return -EINVAL;

	calib = devm_kzalloc(dev, calib_len, GFP_KERNEL);
	if (!calib)
		return -ENOMEM;

	for (i = 0; i < lvts_data->num_devinfo_idx; i++)
		put_unaligned_le32(get_unaligned_le32(devinfo + sizeof(u32) +
						      lvts_data->devinfo_idx[i] * sizeof(u32)),
				   calib + i * sizeof(u32));

	lvts_td->calib = calib;
	lvts_td->calib_len = calib_len;

	return 0;
}

static bool lvts_offsets_in_range(const u8 *off, size_t len)
{
	return off[0] < len && off[1] < len && off[2] < len;
}

static u32 lvts_calib_u24(const u8 *calib, const u8 *off)
{
	return calib[off[0]] | (calib[off[1]] << 8) | (calib[off[2]] << 16);
}

/*
 * Check that calibration data looks like it comes from a calibrated part,
 * and that the regular code can use it. A zero golden temperature is the
 * "not calibrated" marker handled by the regular code (default
 * calibration), so it is accepted.
 */
static bool lvts_calibration_sane(struct device *dev, const u8 *calib,
				  size_t calib_len,
				  const struct lvts_data *lvts_data)
{
	u32 gt;
	int i, j;

	if (!calib || calib_len < sizeof(u32)) {
		dev_warn(dev, "calibration data too short (%zu bytes)\n", calib_len);
		return false;
	}

	gt = (get_unaligned_le32(calib) >> lvts_data->gt_calib_bit_offset) & 0xff;
	if (gt && (gt < LVTS_GOLDEN_TEMP_MIN_PLAUSIBLE || gt >= LVTS_GOLDEN_TEMP_MAX)) {
		dev_warn(dev, "implausible golden temp %u\n", gt);
		return false;
	}

	for (i = 0; i < lvts_data->num_lvts_ctrl; i++) {
		const struct lvts_ctrl_data *ctrl_data = &lvts_data->lvts_ctrl[i];

		if (lvts_data->rck) {
			unsigned int def_rc = lvts_data->rck->def_count_rc;
			u32 count_rc;

			if (!lvts_offsets_in_range(ctrl_data->rc_offsets, calib_len)) {
				dev_warn(dev, "calibration data too short (%zu bytes)\n",
					 calib_len);
				return false;
			}

			count_rc = lvts_calib_u24(calib, ctrl_data->rc_offsets);
			if (gt && (count_rc < def_rc / LVTS_RC_RANGE_FACTOR ||
				   count_rc > def_rc * LVTS_RC_RANGE_FACTOR)) {
				dev_warn(dev, "implausible RC count %u (controller %d)\n",
					 count_rc, i);
				return false;
			}
		}

		lvts_for_each_valid_sensor(j, ctrl_data) {
			const u8 *off = ctrl_data->lvts_sensor[j].cal_offsets;
			u32 count_r;

			if (!lvts_offsets_in_range(off, calib_len)) {
				dev_warn(dev, "calibration data too short (%zu bytes)\n",
					 calib_len);
				return false;
			}

			if (!gt)
				continue;

			count_r = lvts_calib_u24(calib, off);
			if (count_r < LVTS_COUNT_R_MIN_PLAUSIBLE ||
			    count_r > LVTS_COUNT_R_MAX_PLAUSIBLE) {
				dev_warn(dev, "implausible reference count %u (controller %d, sensor %d)\n",
					 count_r, i, j);
				return false;
			}
		}
	}

	return true;
}

/*
 * Get the calibration stream. The nvmem cells are the regular source; the
 * fail_soft SoCs additionally accept the devinfo blob, and fall back to the
 * default calibration (no data in lvts_td->calib) instead of failing the
 * probe when no usable data is available.
 */
static int lvts_calibration_get(struct device *dev, struct lvts_domain *lvts_td,
				const struct lvts_data *lvts_data)
{
	const char *src = "nvmem";
	int ret;

	ret = lvts_calibration_read(dev, lvts_td, lvts_data);
	if (!lvts_data->fail_soft || ret == -EPROBE_DEFER)
		return ret;

	if (ret) {
		dev_warn(dev, "cannot read the calibration cells (%d)\n", ret);
		lvts_calibration_drop(dev, lvts_td);
	}

	if (!lvts_td->calib && lvts_data->devinfo_idx) {
		ret = lvts_devinfo_read(dev, lvts_td, lvts_data);
		if (!ret)
			src = "devinfo";
		else if (ret != -ENOENT)
			dev_warn(dev, "cannot use the devinfo calibration (%d)\n", ret);
	}

	if (lvts_td->calib &&
	    !lvts_calibration_sane(dev, lvts_td->calib, lvts_td->calib_len,
				   lvts_data)) {
		dev_warn(dev, "%s calibration data implausible, using defaults\n", src);
		lvts_calibration_drop(dev, lvts_td);
	}

	if (lvts_td->calib)
		dev_info(dev, "calibration source: %s\n", src);
	else
		dev_warn(dev, "calibration source: built-in defaults, readings may be off by tens of degrees C\n");

	return 0;
}

static int lvts_golden_temp_init(struct device *dev, u8 *calib, size_t calib_len,
				 const struct lvts_data *lvts_data)
{
	u32 gt = 0;

	/*
	 * The golden temp information is contained in the first 32-bit
	 * word  of efuse data at a specific bit offset.
	 */
	if (calib && calib_len >= sizeof(u32))
		gt = (get_unaligned_le32(calib) >> lvts_data->gt_calib_bit_offset) & 0xff;

	/* A zero value for gt means that device has invalid efuse data */
	if (gt && gt < LVTS_GOLDEN_TEMP_MAX)
		golden_temp = gt;

	golden_temp_offset = golden_temp * 500 + lvts_data->temp_offset;

	dev_info(dev, "%sgolden temp=%d\n", gt ? "" : "fake ", golden_temp);

	return 0;
}

static int lvts_ctrl_init(struct device *dev, struct lvts_domain *lvts_td,
					const struct lvts_data *lvts_data)
{
	size_t size = sizeof(*lvts_td->lvts_ctrl) * lvts_data->num_lvts_ctrl;
	struct lvts_ctrl *lvts_ctrl;
	int i, ret;

	/*
	 * Create the calibration bytes stream from efuse data
	 */
	ret = lvts_calibration_get(dev, lvts_td, lvts_data);
	if (ret)
		return ret;

	ret = lvts_golden_temp_init(dev, lvts_td->calib, lvts_td->calib_len,
				    lvts_data);
	if (ret)
		return ret;

	lvts_ctrl = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!lvts_ctrl)
		return -ENOMEM;

	for (i = 0; i < lvts_data->num_lvts_ctrl; i++) {

		lvts_ctrl[i].base = lvts_td->base + lvts_data->lvts_ctrl[i].offset;
		lvts_ctrl[i].lvts_data = lvts_data;

		ret = lvts_sensor_init(dev, &lvts_ctrl[i],
				       &lvts_data->lvts_ctrl[i]);
		if (ret)
			return ret;

		ret = lvts_calibration_init(dev, &lvts_ctrl[i],
					    &lvts_data->lvts_ctrl[i],
					    lvts_td->calib,
					    lvts_td->calib_len);
		if (ret)
			return ret;

		/*
		 * The mode the ctrl will use to read the temperature
		 * (filtered or immediate)
		 */
		lvts_ctrl[i].mode = lvts_data->lvts_ctrl[i].mode;

		lvts_ctrl[i].low_thresh = INT_MIN;
		lvts_ctrl[i].high_thresh = INT_MIN;
	}

	/*
	 * We no longer need the efuse bytes stream, let's free it
	 */
	devm_kfree(dev, lvts_td->calib);

	lvts_td->lvts_ctrl = lvts_ctrl;
	lvts_td->num_lvts_ctrl = lvts_data->num_lvts_ctrl;

	return 0;
}

static void lvts_ctrl_monitor_enable(struct device *dev, struct lvts_ctrl *lvts_ctrl, bool enable)
{
	/*
	 * Bitmaps to enable each sensor on filtered mode in the MONCTL0
	 * register.
	 */
	static const u8 sensor_filt_bitmap[] = { BIT(0), BIT(1), BIT(2), BIT(3) };
	u32 sensor_map = 0;
	int i;

	if (lvts_ctrl->mode != LVTS_MSR_FILTERED_MODE)
		return;

	if (enable) {
		lvts_for_each_valid_sensor(i, lvts_ctrl)
			sensor_map |= sensor_filt_bitmap[i];
	}

	/*
	 * Bits:
	 *      9: Single point access flow
	 *    0-3: Enable sensing point 0-3
	 */
	writel(sensor_map | BIT(9), LVTS_MONCTL0(lvts_ctrl->base));
}

/*
 * At this point the configuration register is the only place in the
 * driver where we write multiple values. Per hardware constraint,
 * each write in the configuration register must be separated by a
 * delay of 2 us.
 */
static void lvts_write_config(struct lvts_ctrl *lvts_ctrl, const u32 *cmds, int nr_cmds)
{
	int i;

	/*
	 * Configuration register
	 */
	for (i = 0; i < nr_cmds; i++) {
		writel(cmds[i], LVTS_CONFIG(lvts_ctrl->base));
		usleep_range(2, 4);
	}
}

static int lvts_irq_init(struct lvts_ctrl *lvts_ctrl)
{
	/*
	 * LVTS_PROTCTL : Thermal Protection Sensor Selection
	 *
	 * Bits:
	 *
	 * 19-18 : Sensor to base the protection on
	 * 17-16 : Strategy:
	 *         00 : Average of 4 sensors
	 *         01 : Max of 4 sensors
	 *         10 : Selected sensor with bits 19-18
	 *         11 : Reserved
	 */

	/*
	 * LVTS_PROTTA : Stage 1 temperature threshold
	 * LVTS_PROTTB : Stage 2 temperature threshold
	 * LVTS_PROTTC : Stage 3 temperature threshold
	 *
	 * Bits:
	 *
	 * 14-0: Raw temperature threshold
	 *
	 * writel(0x0, LVTS_PROTTA(lvts_ctrl->base));
	 * writel(0x0, LVTS_PROTTB(lvts_ctrl->base));
	 * writel(0x0, LVTS_PROTTC(lvts_ctrl->base));
	 */

	/*
	 * LVTS_MONINT : Interrupt configuration register
	 *
	 * The LVTS_MONINT register layout is the same as the LVTS_MONINTSTS
	 * register, except we set the bits to enable the interrupt.
	 */
	writel(0, LVTS_MONINT(lvts_ctrl->base));

	return 0;
}

static int lvts_domain_reset(struct device *dev, struct reset_control *reset)
{
	int ret;

	ret = reset_control_assert(reset);
	if (ret)
		return ret;

	return reset_control_deassert(reset);
}

/*
 * Enable or disable the clocks of a specified thermal controller
 */
static int lvts_ctrl_set_enable(struct lvts_ctrl *lvts_ctrl, int enable)
{
	/*
	 * LVTS_CLKEN : Internal LVTS clock
	 *
	 * Bits:
	 *
	 * 0 : enable / disable clock
	 */
	writel(enable, LVTS_CLKEN(lvts_ctrl->base));

	return 0;
}

static int lvts_ctrl_connect(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;
	u32 id;

	lvts_write_config(lvts_ctrl, lvts_data->conn_cmd, lvts_data->num_conn_cmd);

	/*
	 * LVTS_ID : Get ID and status of the thermal controller
	 *
	 * Bits:
	 *
	 * 0-5	: thermal controller id
	 *   7	: thermal controller connection is valid
	 */
	id = readl(LVTS_ID(lvts_ctrl->base));
	if (!(id & BIT(7)))
		return -EIO;

	return 0;
}

static int lvts_ctrl_initialize(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;

	lvts_write_config(lvts_ctrl, lvts_data->init_cmd, lvts_data->num_init_cmd);

	return 0;
}

/*
 * Have the device of a sensor count its RC oscillator. Returns 0 when the
 * measurement did not complete. Every wait is bounded.
 */
static u32 lvts_rck_read_count(struct lvts_ctrl *lvts_ctrl, int sensor)
{
	const struct lvts_rck_data *rck = lvts_ctrl->lvts_data->rck;
	void __iomem *config = LVTS_CONFIG(lvts_ctrl->base);
	u32 cmd, val, count = 0;

	/* Select the sensor with RCK, then start the counting */
	cmd = rck->write_cmd | (LVTS_DEV_REG_SENSOR_SEL << 8) | sensor;
	lvts_write_config(lvts_ctrl, &cmd, 1);
	cmd = rck->write_cmd | (LVTS_DEV_REG_COUNTING << 8) | LVTS_DEV_COUNTING_RCK;
	lvts_write_config(lvts_ctrl, &cmd, 1);
	usleep_range(40, 60);

	if (readl_poll_timeout(config, val, !(val & LVTS_CONFIG_SENSING_BUSY), 5, 500))
		goto out;

	/* The count is in the lower 24 bits of the device register 0 */
	cmd = rck->read_cmd | (LVTS_DEV_REG_RCK_COUNT << 8);
	lvts_write_config(lvts_ctrl, &cmd, 1);
	usleep_range(5, 10);

	if (readl_poll_timeout(config, val, !(val & LVTS_CONFIG_ACCESS_BUSY), 5, 500))
		goto out;

	count = readl(LVTS_RDATA0(lvts_ctrl->base)) & GENMASK(23, 0);

out:
	/* Back to the normal temperature measurement */
	cmd = rck->write_cmd | (LVTS_DEV_REG_SENSOR_SEL << 8) |
	      LVTS_DEV_SENSOR_SEL_NO_RCK | sensor;
	lvts_write_config(lvts_ctrl, &cmd, 1);

	return count;
}

/*
 * Manual RC calibration: the efuse reference value of a sensor is the
 * value measured at the factory, scaled to the RC count the device reaches
 * now (count_rc * count_r >> 14). A missing measure, or one too far from the
 * efuse RC count, is replaced by the efuse RC count.
 */
static int lvts_ctrl_rck(struct device *dev, struct lvts_ctrl *lvts_ctrl, int idx)
{
	const struct lvts_rck_data *rck = lvts_ctrl->lvts_data->rck;
	u32 ref = lvts_ctrl->count_rc;
	char buf[LVTS_SENSOR_MAX * 12] = "";
	int i, len = 0;

	if (!rck)
		return 0;

	lvts_for_each_valid_sensor(i, lvts_ctrl) {
		u32 count = lvts_rck_read_count(lvts_ctrl, i);
		bool rejected = !count;
		u64 edata;

		if (count && ref)
			rejected = (u64)max(count, ref) * 1000 >
				   (u64)min(count, ref) * LVTS_RC_MAX_RATIO_PERMILLE;

		len += scnprintf(buf + len, sizeof(buf) - len, " %u%s", count,
				 rejected ? "*" : "");

		edata = (u64)(rejected ? ref : count) * lvts_ctrl->calibration[i] >> 14;
		if (!edata || edata > LVTS_EDATA_MASK) {
			dev_dbg(dev, "controller %d: bad RC calibration for sensor %d\n",
				idx, i);
			return -ERANGE;
		}

		lvts_ctrl->calibration[i] = edata;
	}

	dev_info(dev, "controller %d: RC count reference %u, measured%s (* = not used)\n",
		 idx, ref, buf);

	return 0;
}

static int lvts_ctrl_calibrate(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	int i;
	void __iomem *lvts_edata[] = {
		LVTS_EDATA00(lvts_ctrl->base),
		LVTS_EDATA01(lvts_ctrl->base),
		LVTS_EDATA02(lvts_ctrl->base),
		LVTS_EDATA03(lvts_ctrl->base)
	};

	/*
	 * LVTS_EDATA0X : Efuse calibration reference value for sensor X
	 *
	 * Bits:
	 *
	 * 20-0 : Efuse value for normalization data
	 */
	for (i = 0; i < LVTS_SENSOR_MAX; i++)
		writel(lvts_ctrl->calibration[i], lvts_edata[i]);

	return 0;
}

static int lvts_ctrl_configure(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	const struct lvts_data *lvts_data = lvts_ctrl->lvts_data;
	u32 value;

	/*
	 * LVTS_TSSEL : Sensing point index numbering
	 *
	 * Bits:
	 *
	 * 31-24: ADC Sense 3
	 * 23-16: ADC Sense 2
	 * 15-8	: ADC Sense 1
	 * 7-0	: ADC Sense 0
	 */
	value = LVTS_TSSEL_CONF;
	writel(value, LVTS_TSSEL(lvts_ctrl->base));

	/*
	 * LVTS_CALSCALE : ADC voltage round
	 *
	 * Only written where the vendor driver is known to do it too, the
	 * other SoCs keep the reset value. The same goes for the device
	 * measurement mode command that follows it.
	 */
	value = LVTS_CALSCALE_CONF;
	if (lvts_data->rck) {
		writel(value, LVTS_CALSCALE(lvts_ctrl->base));
		lvts_write_config(lvts_ctrl, lvts_data->rck->meas_cmd,
				  lvts_data->rck->num_meas_cmd);
	}

	/*
	 * LVTS_MSRCTL0 : Sensor filtering strategy
	 *
	 * Filters:
	 *
	 * 000 : One sample
	 * 001 : Avg 2 samples
	 * 010 : 4 samples, drop min and max, avg 2 samples
	 * 011 : 6 samples, drop min and max, avg 4 samples
	 * 100 : 10 samples, drop min and max, avg 8 samples
	 * 101 : 18 samples, drop min and max, avg 16 samples
	 *
	 * Bits:
	 *
	 * 0-2  : Sensor0 filter
	 * 3-5  : Sensor1 filter
	 * 6-8  : Sensor2 filter
	 * 9-11 : Sensor3 filter
	 */
	value = LVTS_HW_FILTER << 9 |  LVTS_HW_FILTER << 6 |
			LVTS_HW_FILTER << 3 | LVTS_HW_FILTER;
	writel(value, LVTS_MSRCTL0(lvts_ctrl->base));

	/*
	 * LVTS_MONCTL1 : Period unit and group interval configuration
	 *
	 * The clock source of LVTS thermal controller is 26MHz.
	 *
	 * The period unit is a time base for all the interval delays
	 * specified in the registers. By default we use 12. The time
	 * conversion is done by multiplying by 256 and 1/26.10^6
	 *
	 * An interval delay multiplied by the period unit gives the
	 * duration in seconds.
	 *
	 * - Filter interval delay is a delay between two samples of
	 * the same sensor.
	 *
	 * - Sensor interval delay is a delay between two samples of
	 * different sensors.
	 *
	 * - Group interval delay is a delay between different rounds.
	 *
	 * For example:
	 *     If Period unit = C, filter delay = 1, sensor delay = 2, group delay = 1,
	 *     and two sensors, TS1 and TS2, are in a LVTS thermal controller
	 *     and then
	 *     Period unit time = C * 1/26M * 256 = 12 * 38.46ns * 256 = 118.149us
	 *     Filter interval delay = 1 * Period unit = 118.149us
	 *     Sensor interval delay = 2 * Period unit = 236.298us
	 *     Group interval delay = 1 * Period unit = 118.149us
	 *
	 *     TS1    TS1 ... TS1    TS2    TS2 ... TS2    TS1...
	 *        <--> Filter interval delay
	 *                       <--> Sensor interval delay
	 *                                             <--> Group interval delay
	 * Bits:
	 *      29 - 20 : Group interval
	 *      16 - 13 : Send a single interrupt when crossing the hot threshold (1)
	 *                or an interrupt everytime the hot threshold is crossed (0)
	 *       9 - 0  : Period unit
	 *
	 */
	value = lvts_data->group_interval << 20 | lvts_data->period_unit;
	writel(value, LVTS_MONCTL1(lvts_ctrl->base));

	/*
	 * LVTS_MONCTL2 : Filtering and sensor interval
	 *
	 * Bits:
	 *
	 *      25-16 : Interval unit in PERIOD_UNIT between sample on
	 *              the same sensor, filter interval
	 *       9-0  : Interval unit in PERIOD_UNIT between each sensor
	 *
	 */
	value = lvts_data->filter_interval << 16 | lvts_data->sensor_interval;
	writel(value, LVTS_MONCTL2(lvts_ctrl->base));

	return lvts_irq_init(lvts_ctrl);
}

static int lvts_ctrl_start(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	struct lvts_sensor *lvts_sensors = lvts_ctrl->sensors;
	struct thermal_zone_device *tz;
	u32 sensor_map = 0;
	int i;
	/*
	 * Bitmaps to enable each sensor on immediate and filtered modes, as
	 * described in MSRCTL1 and MONCTL0 registers below, respectively.
	 */
	u32 sensor_imm_bitmap[] = { BIT(4), BIT(5), BIT(6), BIT(9) };
	u32 sensor_filt_bitmap[] = { BIT(0), BIT(1), BIT(2), BIT(3) };

	u32 *sensor_bitmap = lvts_ctrl->mode == LVTS_MSR_IMMEDIATE_MODE ?
			     sensor_imm_bitmap : sensor_filt_bitmap;

	lvts_for_each_valid_sensor(i, lvts_ctrl) {

		int dt_id = lvts_sensors[i].dt_id;

		tz = devm_thermal_of_zone_register(dev, dt_id, &lvts_sensors[i],
						   &lvts_ops);
		if (IS_ERR(tz)) {
			/*
			 * This thermal zone is not described in the
			 * device tree. It is not an error from the
			 * thermal OF code POV, we just continue.
			 */
			if (PTR_ERR(tz) == -ENODEV)
				continue;

			return PTR_ERR(tz);
		}

		devm_thermal_add_hwmon_sysfs(dev, tz);

		/*
		 * The thermal zone pointer will be needed in the
		 * interrupt handler, we store it in the sensor
		 * structure. The thermal domain structure will be
		 * passed to the interrupt handler private data as the
		 * interrupt is shared for all the controller
		 * belonging to the thermal domain.
		 */
		lvts_sensors[i].tz = tz;

		/*
		 * This sensor was correctly associated with a thermal
		 * zone, let's set the corresponding bit in the sensor
		 * map, so we can enable the temperature monitoring in
		 * the hardware thermal controller.
		 */
		sensor_map |= sensor_bitmap[i];
	}

	/*
	 * The initialization of the thermal zones give us
	 * which sensor point to enable. If any thermal zone
	 * was not described in the device tree, it won't be
	 * enabled here in the sensor map.
	 */
	if (lvts_ctrl->mode == LVTS_MSR_IMMEDIATE_MODE) {
		/*
		 * LVTS_MSRCTL1 : Measurement control
		 *
		 * Bits:
		 *
		 * 9: Ignore MSRCTL0 config and do immediate measurement on sensor3
		 * 6: Ignore MSRCTL0 config and do immediate measurement on sensor2
		 * 5: Ignore MSRCTL0 config and do immediate measurement on sensor1
		 * 4: Ignore MSRCTL0 config and do immediate measurement on sensor0
		 *
		 * That configuration will ignore the filtering and the delays
		 * introduced in MONCTL1 and MONCTL2
		 */
		writel(sensor_map, LVTS_MSRCTL1(lvts_ctrl->base));
	} else {
		/*
		 * Bits:
		 *      9: Single point access flow
		 *    0-3: Enable sensing point 0-3
		 */
		writel(sensor_map | BIT(9), LVTS_MONCTL0(lvts_ctrl->base));
	}

	return 0;
}

static int lvts_ctrl_setup(struct device *dev, struct lvts_ctrl *lvts_ctrl, int idx)
{
	int ret;

	/*
	 * Initialization steps:
	 *
	 * - Enable the clock
	 * - Connect to the LVTS
	 * - Initialize the LVTS
	 * [ Calibrate the RC oscillators ]
	 * - Prepare the calibration data
	 * - Select monitored sensors
	 * [ Configure sampling ]
	 * [ Configure the interrupt ]
	 * - Start measurement
	 */
	ret = lvts_ctrl_set_enable(lvts_ctrl, true);
	if (ret) {
		dev_dbg(dev, "Failed to enable LVTS clock");
		return ret;
	}

	ret = lvts_ctrl_connect(dev, lvts_ctrl);
	if (ret) {
		dev_dbg(dev, "Failed to connect to LVTS controller");
		return ret;
	}

	ret = lvts_ctrl_initialize(dev, lvts_ctrl);
	if (ret) {
		dev_dbg(dev, "Failed to initialize controller");
		return ret;
	}

	ret = lvts_ctrl_rck(dev, lvts_ctrl, idx);
	if (ret) {
		dev_dbg(dev, "Failed to calibrate the RC count");
		return ret;
	}

	ret = lvts_ctrl_calibrate(dev, lvts_ctrl);
	if (ret) {
		dev_dbg(dev, "Failed to calibrate controller");
		return ret;
	}

	ret = lvts_ctrl_configure(dev, lvts_ctrl);
	if (ret) {
		dev_dbg(dev, "Failed to configure controller");
		return ret;
	}

	ret = lvts_ctrl_start(dev, lvts_ctrl);
	if (ret) {
		dev_dbg(dev, "Failed to start controller");
		return ret;
	}

	return 0;
}

static int lvts_domain_init(struct device *dev, struct lvts_domain *lvts_td,
					const struct lvts_data *lvts_data)
{
	struct lvts_ctrl *lvts_ctrl;
	int i, ret, nr_ok = 0;

	ret = lvts_ctrl_init(dev, lvts_td, lvts_data);
	if (ret)
		return ret;

	ret = lvts_domain_reset(dev, lvts_td->reset);
	if (ret) {
		dev_dbg(dev, "Failed to reset domain");
		return ret;
	}

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++) {

		lvts_ctrl = &lvts_td->lvts_ctrl[i];

		ret = lvts_ctrl_setup(dev, lvts_ctrl, i);
		if (!ret) {
			nr_ok++;
			continue;
		}

		if (!lvts_data->fail_soft)
			return ret;

		/*
		 * Do not lose the working controllers (e.g. the CPU and
		 * GPU sensors) because one of them does not answer.
		 */
		dev_warn(dev, "controller %d (offset 0x%x) does not work (%d), skipping it\n",
			 i, lvts_data->lvts_ctrl[i].offset, ret);
		lvts_ctrl_set_enable(lvts_ctrl, false);
		lvts_ctrl->valid_sensor_mask = 0;
	}

	if (lvts_data->fail_soft && !nr_ok)
		return -ENODEV;

	return lvts_debugfs_init(dev, lvts_td);
}

static int lvts_probe(struct platform_device *pdev)
{
	const struct lvts_data *lvts_data;
	struct lvts_domain *lvts_td;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int irq, ret;

	lvts_td = devm_kzalloc(dev, sizeof(*lvts_td), GFP_KERNEL);
	if (!lvts_td)
		return -ENOMEM;

	lvts_data = of_device_get_match_data(dev);
	if (!lvts_data)
		return -ENODEV;

	lvts_td->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(lvts_td->clk))
		return dev_err_probe(dev, PTR_ERR(lvts_td->clk), "Failed to retrieve clock\n");

	res = platform_get_mem_or_io(pdev, 0);
	if (!res)
		return dev_err_probe(dev, (-ENXIO), "No IO resource\n");

	lvts_td->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(lvts_td->base))
		return dev_err_probe(dev, PTR_ERR(lvts_td->base), "Failed to map io resource\n");

	/*
	 * The fail_soft SoCs may have no reset line described: a NULL reset
	 * is accepted by the reset core and the domain is simply not reset,
	 * which is fine on a cold boot.
	 */
	if (lvts_data->fail_soft)
		lvts_td->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	else
		lvts_td->reset = devm_reset_control_get_by_index(dev, 0);
	if (IS_ERR(lvts_td->reset))
		return dev_err_probe(dev, PTR_ERR(lvts_td->reset), "Failed to get reset control\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	golden_temp_offset = lvts_data->temp_offset;

	ret = lvts_domain_init(dev, lvts_td, lvts_data);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize the lvts domain\n");

	/*
	 * At this point the LVTS is initialized and enabled. We can
	 * safely enable the interrupt.
	 */
	ret = devm_request_threaded_irq(dev, irq, NULL, lvts_irq_handler,
					IRQF_ONESHOT, dev_name(dev), lvts_td);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request interrupt\n");

	platform_set_drvdata(pdev, lvts_td);

	return 0;
}

static void lvts_remove(struct platform_device *pdev)
{
	struct lvts_domain *lvts_td;
	int i;

	lvts_td = platform_get_drvdata(pdev);

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++)
		lvts_ctrl_set_enable(&lvts_td->lvts_ctrl[i], false);
}

static const struct lvts_ctrl_data mt7988_lvts_ap_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT7988_CPU_0,
			  .cal_offsets = { 0x00, 0x01, 0x02 } },
			{ .dt_id = MT7988_CPU_1,
			  .cal_offsets = { 0x04, 0x05, 0x06 } },
			{ .dt_id = MT7988_ETH2P5G_0,
			  .cal_offsets = { 0x08, 0x09, 0x0a } },
			{ .dt_id = MT7988_ETH2P5G_1,
			  .cal_offsets = { 0x0c, 0x0d, 0x0e } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT7988_TOPS_0,
			   .cal_offsets = { 0x14, 0x15, 0x16 } },
			{ .dt_id = MT7988_TOPS_1,
			   .cal_offsets = { 0x18, 0x19, 0x1a } },
			{ .dt_id = MT7988_ETHWARP_0,
			   .cal_offsets = { 0x1c, 0x1d, 0x1e } },
			{ .dt_id = MT7988_ETHWARP_1,
			   .cal_offsets = { 0x20, 0x21, 0x22 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x100,
	}
};

static int lvts_ctrl_restore_context(struct device *dev, struct lvts_ctrl *lvts_ctrl)
{
	int ret;

	ret = lvts_ctrl_connect(dev, lvts_ctrl);
	if (ret)
		return ret;

	ret = lvts_ctrl_initialize(dev, lvts_ctrl);
	if (ret)
		return ret;

	/* The RC scaled reference values cached at probe time are reused */
	ret = lvts_ctrl_calibrate(dev, lvts_ctrl);
	if (ret)
		return ret;

	ret = lvts_ctrl_configure(dev, lvts_ctrl);
	if (ret)
		return ret;

	writel(lvts_ctrl->saved_offsetl, LVTS_OFFSETL(lvts_ctrl->base));
	writel(lvts_ctrl->saved_offseth, LVTS_OFFSETH(lvts_ctrl->base));
	lvts_ctrl_monitor_enable(dev, lvts_ctrl, true);
	writel(lvts_ctrl->saved_monint, LVTS_MONINT(lvts_ctrl->base));

	return 0;
}

static int lvts_suspend(struct device *dev)
{
	struct lvts_domain *lvts_td;
	int i;

	lvts_td = dev_get_drvdata(dev);

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++) {
		struct lvts_ctrl *lvts_ctrl = &lvts_td->lvts_ctrl[i];

		if (!lvts_ctrl->valid_sensor_mask)
			continue;

		if (lvts_ctrl->lvts_data->context_lost_in_suspend) {
			lvts_ctrl->saved_offsetl = readl(LVTS_OFFSETL(lvts_ctrl->base));
			lvts_ctrl->saved_offseth = readl(LVTS_OFFSETH(lvts_ctrl->base));
			lvts_ctrl->saved_monint = readl(LVTS_MONINT(lvts_ctrl->base));
		}

		lvts_ctrl_monitor_enable(dev, &lvts_td->lvts_ctrl[i], false);
		usleep_range(100, 200);
		lvts_ctrl_set_enable(&lvts_td->lvts_ctrl[i], false);
	}

	clk_disable_unprepare(lvts_td->clk);

	return 0;
}

static int lvts_resume(struct device *dev)
{
	struct lvts_domain *lvts_td;
	int i, ret;

	lvts_td = dev_get_drvdata(dev);

	ret = clk_prepare_enable(lvts_td->clk);
	if (ret)
		return ret;

	for (i = 0; i < lvts_td->num_lvts_ctrl; i++) {
		if (!lvts_td->lvts_ctrl[i].valid_sensor_mask)
			continue;

		lvts_ctrl_set_enable(&lvts_td->lvts_ctrl[i], true);
		usleep_range(100, 200);

		if (lvts_td->lvts_ctrl[i].lvts_data->context_lost_in_suspend) {
			/*
			 * Never fail the system resume for a sensor: the
			 * controller is left off and its zone reads "no data".
			 */
			if (lvts_ctrl_restore_context(dev, &lvts_td->lvts_ctrl[i])) {
				dev_warn(dev, "controller %d: restoring after sleep failed, disabled\n", i);
				lvts_ctrl_set_enable(&lvts_td->lvts_ctrl[i], false);
				lvts_td->lvts_ctrl[i].valid_sensor_mask = 0;
			}
			continue;
		}

		lvts_ctrl_monitor_enable(dev, &lvts_td->lvts_ctrl[i], true);
	}

	return 0;
}

static const u32 default_conn_cmds[] = { 0xC103FFFF, 0xC502FF55 };
static const u32 mt7988_conn_cmds[] = { 0xC103FFFF, 0xC502FC55 };

/*
 * Write device mask: 0xC1030000
 */
static const u32 default_init_cmds[] = {
	0xC1030E01, 0xC1030CFC, 0xC1030A8C, 0xC103098D, 0xC10308F1,
	0xC10307A6, 0xC10306B8, 0xC1030500, 0xC1030420, 0xC1030300,
	0xC1030030, 0xC10300F6, 0xC1030050, 0xC1030060, 0xC10300AC,
	0xC10300FC, 0xC103009D, 0xC10300F1, 0xC10300E1
};

static const u32 mt7988_init_cmds[] = {
	0xC1030300, 0xC1030420, 0xC1030500, 0xC10307A6, 0xC1030CFC,
	0xC1030A8C, 0xC103098D, 0xC10308F1, 0xC1030B04, 0xC1030E01,
	0xC10306B8
};

/*
 * The MT6853 vendor driver uses 0x81... device commands (0xC1... elsewhere)
 * and no auto RCK (device register 0x0e is left alone): the RC calibration
 * is done by the host, and the devices are switched to their normal
 * measurement mode afterwards.
 */
static const u32 mt6853_conn_cmds[] = { 0x8103FFFF, 0x8502FC55 };

static const u32 mt6853_init_cmds[] = {
	0x81030300, 0x810306F8, 0x810307A6, 0x81030500, 0x81030420,
	0x81030A8C, 0x81030CFC, 0x8103098D, 0x810308F1
};

static const u32 mt6853_meas_cmds[] = { 0x810306B8 };

static const struct lvts_rck_data mt6853_rck_data = {
	.write_cmd	= 0x81030000,
	.read_cmd	= 0x81020000,
	.meas_cmd	= mt6853_meas_cmds,
	.num_meas_cmd	= ARRAY_SIZE(mt6853_meas_cmds),
	.def_count_rc	= 2750,
};

/*
 * Words of the bootloader devinfo array holding the MT6853 calibration data,
 * in the order the calibration stream is described by the controller data:
 * the golden temperature, the reference count of the 14 sensors, 3 unused
 * words and the RC count of the controllers.
 */
static const u16 mt6853_devinfo_idx[] = {
	116, 117, 118, 119, 190, 191, 192, 193, 194, 195, 196,
	197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207
};

/*
 * The MT8186 calibration data is stored as packed 3-byte little-endian
 * values using a weird layout that makes sense only when viewed as a 32-bit
 * hexadecimal word dump. Let's suppose SxBy where x = sensor number and
 * y = byte number where the LSB is y=0. We then have:
 *
 *   [S0B2-S0B1-S0B0-S1B2] [S1B1-S1B0-S2B2-S2B1] [S2B0-S3B2-S3B1-S3B0]
 *
 * However, when considering a byte stream, those appear as follows:
 *
 *   [S1B2] [S0B0[ [S0B1] [S0B2] [S2B1] [S2B2] [S1B0] [S1B1] [S3B0] [S3B1] [S3B2] [S2B0]
 *
 * Hence the rather confusing offsets provided below.
 */
static const struct lvts_ctrl_data mt8186_lvts_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8186_LITTLE_CPU0,
			  .cal_offsets = { 5, 6, 7 } },
			{ .dt_id = MT8186_LITTLE_CPU1,
			  .cal_offsets = { 10, 11, 4 } },
			{ .dt_id = MT8186_LITTLE_CPU2,
			  .cal_offsets = { 15, 8, 9 } },
			{ .dt_id = MT8186_CAM,
			  .cal_offsets = { 12, 13, 14 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8186_BIG_CPU0,
			  .cal_offsets = { 22, 23, 16 } },
			{ .dt_id = MT8186_BIG_CPU1,
			  .cal_offsets = { 27, 20, 21 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8186_NNA,
			  .cal_offsets = { 29, 30, 31 } },
			{ .dt_id = MT8186_ADSP,
			  .cal_offsets = { 34, 35, 28 } },
			{ .dt_id = MT8186_GPU,
			  .cal_offsets = { 39, 32, 33 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 0),
		.offset = 0x200,
	}
};

static const struct lvts_ctrl_data mt8188_lvts_mcu_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8188_MCU_LITTLE_CPU0,
			  .cal_offsets = { 22, 23, 24 } },
			{ .dt_id = MT8188_MCU_LITTLE_CPU1,
			  .cal_offsets = { 25, 26, 27 } },
			{ .dt_id = MT8188_MCU_LITTLE_CPU2,
			  .cal_offsets = { 28, 29, 30 } },
			{ .dt_id = MT8188_MCU_LITTLE_CPU3,
			  .cal_offsets = { 31, 32, 33 } },
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8188_MCU_BIG_CPU0,
			  .cal_offsets = { 34, 35, 36 } },
			{ .dt_id = MT8188_MCU_BIG_CPU1,
			  .cal_offsets = { 37, 38, 39 } },
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
	}
};

static const struct lvts_ctrl_data mt8188_lvts_ap_data_ctrl[] = {
	{
		.lvts_sensor = {

			{ /* unused */ },
			{ .dt_id = MT8188_AP_APU,
			  .cal_offsets = { 40, 41, 42 } },
		},
		VALID_SENSOR_MAP(0, 1, 0, 0),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8188_AP_GPU0,
			  .cal_offsets = { 43, 44, 45 } },
			{ .dt_id = MT8188_AP_GPU1,
			  .cal_offsets = { 46, 47, 48 } },
			{ .dt_id = MT8188_AP_ADSP,
			  .cal_offsets = { 49, 50, 51 } },
		},
		VALID_SENSOR_MAP(1, 1, 1, 0),
		.offset = 0x100,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8188_AP_VDO,
			  .cal_offsets = { 52, 53, 54 } },
			{ .dt_id = MT8188_AP_INFRA,
			  .cal_offsets = { 55, 56, 57 } },
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x200,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8188_AP_CAM1,
			  .cal_offsets = { 58, 59, 60 } },
			{ .dt_id = MT8188_AP_CAM2,
			  .cal_offsets = { 61, 62, 63 } },
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x300,
	}
};

static const struct lvts_ctrl_data mt8192_lvts_mcu_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_MCU_BIG_CPU0,
			  .cal_offsets = { 0x04, 0x05, 0x06 } },
			{ .dt_id = MT8192_MCU_BIG_CPU1,
			  .cal_offsets = { 0x08, 0x09, 0x0a } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x0,
		.mode = LVTS_MSR_FILTERED_MODE,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_MCU_BIG_CPU2,
			  .cal_offsets = { 0x0c, 0x0d, 0x0e } },
			{ .dt_id = MT8192_MCU_BIG_CPU3,
			  .cal_offsets = { 0x10, 0x11, 0x12 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
		.mode = LVTS_MSR_FILTERED_MODE,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_MCU_LITTLE_CPU0,
			  .cal_offsets = { 0x14, 0x15, 0x16 } },
			{ .dt_id = MT8192_MCU_LITTLE_CPU1,
			  .cal_offsets = { 0x18, 0x19, 0x1a } },
			{ .dt_id = MT8192_MCU_LITTLE_CPU2,
			  .cal_offsets = { 0x1c, 0x1d, 0x1e } },
			{ .dt_id = MT8192_MCU_LITTLE_CPU3,
			  .cal_offsets = { 0x20, 0x21, 0x22 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x200,
		.mode = LVTS_MSR_FILTERED_MODE,
	}
};

static const struct lvts_ctrl_data mt8192_lvts_ap_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_AP_VPU0,
			  .cal_offsets = { 0x24, 0x25, 0x26 } },
			{ .dt_id = MT8192_AP_VPU1,
			  .cal_offsets = { 0x28, 0x29, 0x2a } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_AP_GPU0,
			  .cal_offsets = { 0x2c, 0x2d, 0x2e } },
			{ .dt_id = MT8192_AP_GPU1,
			  .cal_offsets = { 0x30, 0x31, 0x32 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_AP_INFRA,
			  .cal_offsets = { 0x34, 0x35, 0x36 } },
			{ .dt_id = MT8192_AP_CAM,
			  .cal_offsets = { 0x38, 0x39, 0x3a } },
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x200,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8192_AP_MD0,
			  .cal_offsets = { 0x3c, 0x3d, 0x3e } },
			{ .dt_id = MT8192_AP_MD1,
			  .cal_offsets = { 0x40, 0x41, 0x42 } },
			{ .dt_id = MT8192_AP_MD2,
			  .cal_offsets = { 0x44, 0x45, 0x46 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 0),
		.offset = 0x300,
	}
};

static const struct lvts_ctrl_data mt8195_lvts_mcu_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_MCU_BIG_CPU0,
			  .cal_offsets = { 0x04, 0x05, 0x06 } },
			{ .dt_id = MT8195_MCU_BIG_CPU1,
			  .cal_offsets = { 0x07, 0x08, 0x09 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_MCU_BIG_CPU2,
			  .cal_offsets = { 0x0d, 0x0e, 0x0f } },
			{ .dt_id = MT8195_MCU_BIG_CPU3,
			  .cal_offsets = { 0x10, 0x11, 0x12 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_MCU_LITTLE_CPU0,
			  .cal_offsets = { 0x16, 0x17, 0x18 } },
			{ .dt_id = MT8195_MCU_LITTLE_CPU1,
			  .cal_offsets = { 0x19, 0x1a, 0x1b } },
			{ .dt_id = MT8195_MCU_LITTLE_CPU2,
			  .cal_offsets = { 0x1c, 0x1d, 0x1e } },
			{ .dt_id = MT8195_MCU_LITTLE_CPU3,
			  .cal_offsets = { 0x1f, 0x20, 0x21 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x200,
	}
};

static const struct lvts_ctrl_data mt8195_lvts_ap_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_AP_VPU0,
			  .cal_offsets = { 0x25, 0x26, 0x27 } },
			{ .dt_id = MT8195_AP_VPU1,
			  .cal_offsets = { 0x28, 0x29, 0x2a } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x0,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_AP_GPU0,
			  .cal_offsets = { 0x2e, 0x2f, 0x30 } },
			{ .dt_id = MT8195_AP_GPU1,
			  .cal_offsets = { 0x31, 0x32, 0x33 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_AP_VDEC,
			  .cal_offsets = { 0x37, 0x38, 0x39 } },
			{ .dt_id = MT8195_AP_IMG,
			  .cal_offsets = { 0x3a, 0x3b, 0x3c } },
			{ .dt_id = MT8195_AP_INFRA,
			  .cal_offsets = { 0x3d, 0x3e, 0x3f } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 0),
		.offset = 0x200,
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT8195_AP_CAM0,
			  .cal_offsets = { 0x43, 0x44, 0x45 } },
			{ .dt_id = MT8195_AP_CAM1,
			  .cal_offsets = { 0x46, 0x47, 0x48 } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x300,
	}
};

/*
 * The MT6853 calibration stream is 22 32-bit words (see mt6853_devinfo_idx):
 * word 0 has the golden temperature in its upper byte, words 1 to 14 the
 * reference count of the sensors, in the order they are listed below (the
 * upper bytes of words 1 to 3 are the RC count of the last controller),
 * words 15 to 17 are not used and words 18 to 21 are the RC counts of the
 * first four controllers.
 */
static const struct lvts_ctrl_data mt6853_lvts_mcu_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT6853_MCU_LL_CPU0,
			  .cal_offsets = { 0x04, 0x05, 0x06 } },
			{ .dt_id = MT6853_MCU_LL_CPU1,
			  .cal_offsets = { 0x08, 0x09, 0x0a } },
			{ .dt_id = MT6853_MCU_LL_CPU2,
			  .cal_offsets = { 0x0c, 0x0d, 0x0e } },
			{ .dt_id = MT6853_MCU_LL_CPU3,
			  .cal_offsets = { 0x10, 0x11, 0x12 } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x0,
		.mode = LVTS_MSR_FILTERED_MODE,
		.rc_offsets = { 72, 73, 74 },
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT6853_MCU_L_CPU0,
			  .cal_offsets = { 0x14, 0x15, 0x16 } },
			{ .dt_id = MT6853_MCU_L_CPU1,
			  .cal_offsets = { 0x18, 0x19, 0x1a } }
		},
		VALID_SENSOR_MAP(1, 1, 0, 0),
		.offset = 0x100,
		.mode = LVTS_MSR_FILTERED_MODE,
		.rc_offsets = { 76, 77, 78 },
	}
};

static const struct lvts_ctrl_data mt6853_lvts_ap_data_ctrl[] = {
	{
		.lvts_sensor = {
			{ .dt_id = MT6853_AP_GPU,
			  .cal_offsets = { 0x1c, 0x1d, 0x1e } },
			{ .dt_id = MT6853_AP_CAM,
			  .cal_offsets = { 0x20, 0x21, 0x22 } },
			{ .dt_id = MT6853_AP_SOC0,
			  .cal_offsets = { 0x24, 0x25, 0x26 } },
			{ .dt_id = MT6853_AP_SOC1,
			  .cal_offsets = { 0x28, 0x29, 0x2a } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 1),
		.offset = 0x0,
		.mode = LVTS_MSR_FILTERED_MODE,
		.rc_offsets = { 80, 81, 82 },
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT6853_AP_VPU,
			  .cal_offsets = { 0x2c, 0x2d, 0x2e } }
		},
		VALID_SENSOR_MAP(1, 0, 0, 0),
		.offset = 0x100,
		.mode = LVTS_MSR_FILTERED_MODE,
		.rc_offsets = { 84, 85, 86 },
	},
	{
		.lvts_sensor = {
			{ .dt_id = MT6853_AP_MD0,
			  .cal_offsets = { 0x30, 0x31, 0x32 } },
			{ .dt_id = MT6853_AP_MD1,
			  .cal_offsets = { 0x34, 0x35, 0x36 } },
			{ .dt_id = MT6853_AP_MD2,
			  .cal_offsets = { 0x38, 0x39, 0x3a } }
		},
		VALID_SENSOR_MAP(1, 1, 1, 0),
		.offset = 0x200,
		.mode = LVTS_MSR_FILTERED_MODE,
		.rc_offsets = { 15, 11, 7 },
	}
};

static const struct lvts_data mt7988_lvts_ap_data = {
	.lvts_ctrl	= mt7988_lvts_ap_data_ctrl,
	.conn_cmd	= mt7988_conn_cmds,
	.init_cmd	= mt7988_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt7988_lvts_ap_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(mt7988_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(mt7988_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT7988,
	.temp_offset	= LVTS_COEFF_B_MT7988,
	.gt_calib_bit_offset = 24,
};

static const struct lvts_data mt8186_lvts_data = {
	.lvts_ctrl	= mt8186_lvts_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8186_lvts_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT7988,
	.temp_offset	= LVTS_COEFF_B_MT7988,
	.gt_calib_bit_offset = 24,
	.def_calibration = 19000,
};

static const struct lvts_data mt8188_lvts_mcu_data = {
	.lvts_ctrl	= mt8188_lvts_mcu_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8188_lvts_mcu_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 20,
	.def_calibration = 35000,
};

static const struct lvts_data mt8188_lvts_ap_data = {
	.lvts_ctrl	= mt8188_lvts_ap_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8188_lvts_ap_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 20,
	.def_calibration = 35000,
};

static const struct lvts_data mt8192_lvts_mcu_data = {
	.lvts_ctrl	= mt8192_lvts_mcu_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8192_lvts_mcu_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
};

static const struct lvts_data mt8192_lvts_ap_data = {
	.lvts_ctrl	= mt8192_lvts_ap_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8192_lvts_ap_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
};

static const struct lvts_data mt8195_lvts_mcu_data = {
	.lvts_ctrl	= mt8195_lvts_mcu_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8195_lvts_mcu_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
};

static const struct lvts_data mt8195_lvts_ap_data = {
	.lvts_ctrl	= mt8195_lvts_ap_data_ctrl,
	.conn_cmd	= default_conn_cmds,
	.init_cmd	= default_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt8195_lvts_ap_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(default_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(default_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
};

static const struct lvts_data mt6853_lvts_mcu_data = {
	.lvts_ctrl	= mt6853_lvts_mcu_data_ctrl,
	.conn_cmd	= mt6853_conn_cmds,
	.init_cmd	= mt6853_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt6853_lvts_mcu_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(mt6853_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(mt6853_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
	.period_unit	= 12,
	.group_interval	= 1,
	.filter_interval = 1,
	.sensor_interval = 1,
	.rck		= &mt6853_rck_data,
	.fail_soft	= true,
	.devinfo_idx	= mt6853_devinfo_idx,
	.num_devinfo_idx = ARRAY_SIZE(mt6853_devinfo_idx),
	.context_lost_in_suspend = true,
};

static const struct lvts_data mt6853_lvts_ap_data = {
	.lvts_ctrl	= mt6853_lvts_ap_data_ctrl,
	.conn_cmd	= mt6853_conn_cmds,
	.init_cmd	= mt6853_init_cmds,
	.num_lvts_ctrl	= ARRAY_SIZE(mt6853_lvts_ap_data_ctrl),
	.num_conn_cmd	= ARRAY_SIZE(mt6853_conn_cmds),
	.num_init_cmd	= ARRAY_SIZE(mt6853_init_cmds),
	.temp_factor	= LVTS_COEFF_A_MT8195,
	.temp_offset	= LVTS_COEFF_B_MT8195,
	.gt_calib_bit_offset = 24,
	.def_calibration = 35000,
	.period_unit	= 12,
	.group_interval	= 1,
	.filter_interval = 1,
	.sensor_interval = 1,
	.rck		= &mt6853_rck_data,
	.fail_soft	= true,
	.devinfo_idx	= mt6853_devinfo_idx,
	.num_devinfo_idx = ARRAY_SIZE(mt6853_devinfo_idx),
	.context_lost_in_suspend = true,
};

static const struct of_device_id lvts_of_match[] = {
	{ .compatible = "mediatek,mt6853-lvts-mcu", .data = &mt6853_lvts_mcu_data },
	{ .compatible = "mediatek,mt6853-lvts-ap", .data = &mt6853_lvts_ap_data },
	{ .compatible = "mediatek,mt7988-lvts-ap", .data = &mt7988_lvts_ap_data },
	{ .compatible = "mediatek,mt8186-lvts", .data = &mt8186_lvts_data },
	{ .compatible = "mediatek,mt8188-lvts-mcu", .data = &mt8188_lvts_mcu_data },
	{ .compatible = "mediatek,mt8188-lvts-ap", .data = &mt8188_lvts_ap_data },
	{ .compatible = "mediatek,mt8192-lvts-mcu", .data = &mt8192_lvts_mcu_data },
	{ .compatible = "mediatek,mt8192-lvts-ap", .data = &mt8192_lvts_ap_data },
	{ .compatible = "mediatek,mt8195-lvts-mcu", .data = &mt8195_lvts_mcu_data },
	{ .compatible = "mediatek,mt8195-lvts-ap", .data = &mt8195_lvts_ap_data },
	{},
};
MODULE_DEVICE_TABLE(of, lvts_of_match);

static const struct dev_pm_ops lvts_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(lvts_suspend, lvts_resume)
};

static struct platform_driver lvts_driver = {
	.probe = lvts_probe,
	.remove = lvts_remove,
	.driver = {
		.name = "mtk-lvts-thermal",
		.of_match_table = lvts_of_match,
		.pm = &lvts_pm_ops,
	},
};
module_platform_driver(lvts_driver);

MODULE_AUTHOR("Balsam CHIHI <bchihi@baylibre.com>");
MODULE_DESCRIPTION("MediaTek LVTS Thermal Driver");
MODULE_LICENSE("GPL");
