// SPDX-License-Identifier: GPL-2.0
/*
 * Battery monitor for the Redmi Note 9T (MT6853T)
 *
 * The pack voltage, the thermistor and the FGADC current are read from the
 * MT6359 AUXADC. The state of charge is estimated from the OCV table of the
 * monitored "simple-battery" node and the MT6360 charger is kept inside the
 * current, voltage and temperature limits of the cell.
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iio/consumer.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define BAT_POLL_MS			5000
#define BAT_MAX_DT_MS			(12LL * 60 * 60 * 1000)
#define BAT_FIRST_POLL_TRIES		5
#define BAT_HOLD_POLLS			12

/* MT6359 AUXADC: 1.8 V reference behind a 7/2 (VBAT) or 5/2 (12 bit) divider */
#define BAT_VBAT_FULL_SCALE_UV		6300000
#define BAT_VBAT_RAW_RANGE		32768
#define BAT_VBAT_MIN_UV			2500000
#define BAT_VBAT_MAX_UV			5000000
#define BAT_ADC12_FULL_SCALE_MV		4500
#define BAT_ADC12_RAW_RANGE		4096

/* FGADC current register: 610.352 uA per step, negative while discharging */
#define BAT_IBAT_UA_PER_K		610352
#define BAT_IBAT_SANE_UA		10000000
#define BAT_IBAT_MAX_ERRS		3
#define BAT_IBAT_BACKOFF_POLLS		12
#define BAT_IBAT_HOLD_POLLS		2
#define BAT_SIGN_CHECK_UA		50000
#define BAT_SIGN_CHECK_POLLS		6

/* NTC: 100 kOhm pull-up to VBIF, which is a fixed 2.8 V supply */
#define BAT_NTC_PULLUP_OHM		100000
#define BAT_VBIF_NOMINAL_MV		2800
#define BAT_VBIF_MIN_MV			2500
#define BAT_VBIF_MAX_MV			3000

#define BAT_TEMP_HYST_MC		2000
#define BAT_OVP_MARGIN_UV		50000
#define BAT_ALERT_MIN_DEFAULT_C		0
#define BAT_ALERT_MAX_DEFAULT_C		60
#define BAT_HOT_START_DEFAULT_C		45
#define BAT_HOT_CV_DEFAULT_UV		4090000

/* State of charge is kept in units of 0.001 % */
#define BAT_SOC_FULL			100000
#define BAT_SOC_TAU_MS			80000
#define BAT_SOC_SETTLE_POLLS		6
#define BAT_SOC_RISE_IDLE		2
#define BAT_SOC_FALL_IDLE		40
#define BAT_SOC_RISE_CHARGING		30
#define BAT_SOC_FALL_CHARGING		2
#define BAT_SOC_FULL_RAMP		200
#define BAT_FULL_MARGIN_UV		100000
#define BAT_FULL_IDLE_MARGIN_UV		100000
#define BAT_FULL_RELEASE_UV		120000
#define BAT_IR_COMP_MAX_UV		500000
#define BAT_ASSUMED_LOAD_UA		300000
#define BAT_EMPTY_UV			3400000
#define BAT_EMPTY_NO_IBAT_UV		3300000
#define BAT_EMPTY_OCV_UV		3450000
#define BAT_EMPTY_POLLS			6

struct mt6853_bat_snap {
	bool ready;
	bool voltage_ok;
	bool current_ok;
	bool temp_ok;
	int status;
	int health;
	int capacity;
	int voltage_uv;
	int current_ua;
	int temp_dc;
	int charge_counter_uah;
};

struct mt6853_bat {
	struct device *dev;
	struct power_supply *psy;
	struct power_supply_battery_info *info;
	struct iio_channel *vbat_chan;
	struct iio_channel *ts_chan;
	struct iio_channel *vbif_chan;
	struct iio_channel *ibat_chan;
	struct delayed_work work;
	struct mutex lock;		/* protects snap */
	struct mt6853_bat_snap snap;
	bool started;

	int alert_min_c;
	int alert_max_c;
	int hot_start_c;
	int hot_cv_uv;

	/* Everything below is only touched by the poll */
	int vbat_uv;
	int ibat_ua;
	int temp_mc;
	bool vbat_ok;
	bool ibat_ok;
	bool temp_ok;
	unsigned int vbat_stale;
	unsigned int ibat_stale;
	unsigned int temp_stale;
	unsigned int ibat_errs;
	unsigned int ibat_backoff;
	int ibat_sign;
	unsigned int sign_votes;
	bool sign_done;

	bool cold;
	bool hot;
	bool hot_cv;
	bool ovp;
	bool inhibited;
	bool full;
	unsigned int empty_polls;
	unsigned int settle_polls;
	int soc_mp;
	int ocv_uv;
	ktime_t last_poll;
};

static bool ignore_temp;
module_param(ignore_temp, bool, 0644);
MODULE_PARM_DESC(ignore_temp, "Do not stop charging on battery temperature faults (debug only)");

static const struct {
	s16 temp_c;
	u32 ohm;
} mt6853_bat_ntc[] = {
	{ -40, 4251000 }, { -35, 3005000 }, { -30, 2149000 },
	{ -25, 1554000 }, { -20, 1135000 }, { -15, 837800 },
	{ -10, 624100 }, { -5, 469100 }, { 0, 355600 },
	{ 5, 271800 }, { 10, 209400 }, { 15, 162500 },
	{ 20, 127000 }, { 25, 100000 }, { 30, 79230 },
	{ 35, 63180 }, { 40, 50680 }, { 45, 40900 },
	{ 50, 33190 }, { 55, 27090 }, { 60, 22220 },
};

static int mt6853_bat_read_raw(struct iio_channel *chan)
{
	int raw, ret;

	if (!chan)
		return -ENODEV;

	ret = iio_read_channel_raw(chan, &raw);
	if (ret < 0)
		return ret;

	return raw < 0 ? -ERANGE : raw;
}

static int mt6853_bat_read_vbat(struct mt6853_bat *bat)
{
	int raw = mt6853_bat_read_raw(bat->vbat_chan);
	int uv;

	if (raw < 0)
		return raw;

	uv = div_u64((u64)raw * BAT_VBAT_FULL_SCALE_UV, BAT_VBAT_RAW_RANGE);
	if (uv < BAT_VBAT_MIN_UV || uv > BAT_VBAT_MAX_UV)
		return -ERANGE;

	return uv;
}

static int mt6853_bat_read_mv(struct iio_channel *chan)
{
	int raw = mt6853_bat_read_raw(chan);

	if (raw < 0)
		return raw;
	if (!raw)
		return -EIO;

	return DIV_ROUND_CLOSEST(raw * BAT_ADC12_FULL_SCALE_MV, BAT_ADC12_RAW_RANGE);
}

/* The modem samples the same ADC input and now and then causes a low reading */
static int mt6853_bat_read_ts_mv(struct mt6853_bat *bat)
{
	int v[3], n = 0, i, ret = -EIO;

	for (i = 0; i < ARRAY_SIZE(v); i++) {
		ret = mt6853_bat_read_mv(bat->ts_chan);
		if (ret >= 0)
			v[n++] = ret;
	}

	switch (n) {
	case 0:
		return ret;
	case 1:
		return v[0];
	case 2:
		return (v[0] + v[1]) / 2;
	}

	if (v[0] > v[1])
		swap(v[0], v[1]);
	if (v[1] > v[2])
		swap(v[1], v[2]);
	if (v[0] > v[1])
		swap(v[0], v[1]);

	return v[1];
}

static int mt6853_bat_ntc_to_mc(u32 ohm, int *mc)
{
	const int last = ARRAY_SIZE(mt6853_bat_ntc) - 1;
	int i;

	/* An open sensor reads far above the coldest table entry */
	if (ohm > mt6853_bat_ntc[0].ohm)
		return -ERANGE;

	if (ohm <= mt6853_bat_ntc[last].ohm) {
		*mc = mt6853_bat_ntc[last].temp_c * 1000;
		return 0;
	}

	for (i = 1; i < last; i++)
		if (ohm > mt6853_bat_ntc[i].ohm)
			break;

	*mc = mt6853_bat_ntc[i - 1].temp_c * 1000 +
	      (int)div_u64((u64)(mt6853_bat_ntc[i - 1].ohm - ohm) * 5000,
			   mt6853_bat_ntc[i - 1].ohm - mt6853_bat_ntc[i].ohm);
	return 0;
}

static int mt6853_bat_read_temp(struct mt6853_bat *bat, int *mc)
{
	int vts, vbif;

	vts = mt6853_bat_read_ts_mv(bat);
	if (vts < 0)
		return vts;

	vbif = mt6853_bat_read_mv(bat->vbif_chan);
	if (vbif < BAT_VBIF_MIN_MV || vbif > BAT_VBIF_MAX_MV) {
		dev_warn_once(bat->dev, "VBIF reads %d mV, assuming %d mV\n",
			      vbif, BAT_VBIF_NOMINAL_MV);
		vbif = BAT_VBIF_NOMINAL_MV;
	}
	if (vts >= vbif)
		return -ERANGE;

	return mt6853_bat_ntc_to_mc(div_u64((u64)BAT_NTC_PULLUP_OHM * vts, vbif - vts), mc);
}

/* The AUXADC resets itself after repeated conversion timeouts, so back off */
static int mt6853_bat_read_ibat(struct mt6853_bat *bat, int *ua)
{
	s64 val;
	int raw;

	if (!bat->ibat_chan)
		return -ENODEV;

	if (bat->ibat_backoff) {
		bat->ibat_backoff--;
		return -EAGAIN;
	}

	raw = mt6853_bat_read_raw(bat->ibat_chan);
	if (raw < 0) {
		if (bat->ibat_errs < BAT_IBAT_MAX_ERRS)
			bat->ibat_errs++;
		if (bat->ibat_errs >= BAT_IBAT_MAX_ERRS)
			bat->ibat_backoff = BAT_IBAT_BACKOFF_POLLS;
		return raw;
	}

	bat->ibat_errs = 0;
	if (!raw)
		return -ENODATA;

	val = div_s64((s64)(s16)raw * BAT_IBAT_UA_PER_K, 1000) * bat->ibat_sign;
	if (abs(val) > BAT_IBAT_SANE_UA)
		return -ERANGE;

	*ua = val;
	return 0;
}

static bool mt6853_bat_hold(unsigned int *stale, unsigned int max)
{
	if (*stale >= max)
		return false;

	(*stale)++;
	return true;
}

static void mt6853_bat_measure(struct mt6853_bat *bat)
{
	int val, ret;

	ret = mt6853_bat_read_vbat(bat);
	if (ret >= 0) {
		bat->vbat_uv = ret;
		bat->vbat_ok = true;
		bat->vbat_stale = 0;
	} else if (!mt6853_bat_hold(&bat->vbat_stale, BAT_HOLD_POLLS)) {
		bat->vbat_ok = false;
	}

	ret = mt6853_bat_read_temp(bat, &val);
	if (!ret) {
		bat->temp_mc = val;
		bat->temp_ok = true;
		bat->temp_stale = 0;
	} else if (!mt6853_bat_hold(&bat->temp_stale, BAT_HOLD_POLLS)) {
		bat->temp_ok = false;
	}

	ret = mt6853_bat_read_ibat(bat, &val);
	if (!ret) {
		bat->ibat_ua = val;
		bat->ibat_ok = true;
		bat->ibat_stale = 0;
	} else if (!mt6853_bat_hold(&bat->ibat_stale, BAT_IBAT_HOLD_POLLS)) {
		bat->ibat_ok = false;
	}
}

/* The current has to be negative while no charger is online */
static void mt6853_bat_check_sign(struct mt6853_bat *bat, bool unplugged)
{
	if (bat->sign_done || !unplugged || !bat->ibat_ok ||
	    bat->ibat_ua <= BAT_SIGN_CHECK_UA) {
		bat->sign_votes = 0;
		return;
	}

	if (++bat->sign_votes < BAT_SIGN_CHECK_POLLS)
		return;

	bat->ibat_sign = -bat->ibat_sign;
	bat->ibat_ua = -bat->ibat_ua;
	bat->sign_done = true;
	dev_warn(bat->dev, "positive current while discharging, inverting its sign\n");
}

static bool mt6853_bat_above(bool active, int t, int trip)
{
	return active ? t > trip - BAT_TEMP_HYST_MC : t >= trip;
}

static bool mt6853_bat_below(bool active, int t, int trip)
{
	return active ? t < trip + BAT_TEMP_HYST_MC : t <= trip;
}

static void mt6853_bat_limits(struct mt6853_bat *bat)
{
	int max_uv = bat->info->voltage_max_design_uv;

	if (bat->temp_ok) {
		bat->cold = mt6853_bat_below(bat->cold, bat->temp_mc, bat->alert_min_c * 1000);
		bat->hot = mt6853_bat_above(bat->hot, bat->temp_mc, bat->alert_max_c * 1000);
		bat->hot_cv = mt6853_bat_above(bat->hot_cv, bat->temp_mc, bat->hot_start_c * 1000);
	}

	if (bat->vbat_ok) {
		if (bat->vbat_uv > max_uv + BAT_OVP_MARGIN_UV)
			bat->ovp = true;
		else if (bat->vbat_uv < max_uv)
			bat->ovp = false;
	}
}

/* Charging stops on an unknown temperature as well, unless told otherwise */
static bool mt6853_bat_inhibit(struct mt6853_bat *bat)
{
	if (bat->ovp)
		return true;
	if (ignore_temp)
		return false;

	return !bat->temp_ok || bat->cold || bat->hot;
}

static void mt6853_bat_set_if_differs(struct mt6853_bat *bat, struct power_supply *chg,
				      enum power_supply_property psp, int want, int tol)
{
	union power_supply_propval val;
	int ret;

	ret = power_supply_get_property(chg, psp, &val);
	if (!ret && abs(val.intval - want) <= tol)
		return;

	val.intval = want;
	ret = power_supply_set_property(chg, psp, &val);
	if (ret)
		dev_warn_ratelimited(bat->dev, "charger rejected %d for property %d: %d\n",
				     want, psp, ret);
}

static void mt6853_bat_charger(struct mt6853_bat *bat, struct power_supply *chg, bool inhibit)
{
	union power_supply_propval val;
	int ichg = bat->info->constant_charge_current_max_ua;
	int cv = bat->info->constant_charge_voltage_max_uv;
	int behaviour = inhibit ? POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE :
				  POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;

	if (!power_supply_get_property(chg, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, &val))
		ichg = min(ichg, val.intval);
	if (!power_supply_get_property(chg, POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX, &val))
		cv = min(cv, val.intval);
	if (bat->hot_cv && !ignore_temp)
		cv = min(cv, bat->hot_cv_uv);

	/* Stop first, relax only after the limits are in place */
	if (inhibit)
		mt6853_bat_set_if_differs(bat, chg, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
					  behaviour, 0);
	mt6853_bat_set_if_differs(bat, chg, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
				  ichg, 99999);
	mt6853_bat_set_if_differs(bat, chg, POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
				  cv, 9999);
	if (!inhibit)
		mt6853_bat_set_if_differs(bat, chg, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
					  behaviour, 0);
}

static int mt6853_bat_status(struct mt6853_bat *bat, bool supplied, bool inhibit,
			     int chg_status)
{
	if (!bat->vbat_ok)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	if (!supplied)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (inhibit)
		return POWER_SUPPLY_STATUS_NOT_CHARGING;

	switch (chg_status) {
	case POWER_SUPPLY_STATUS_CHARGING:
	case POWER_SUPPLY_STATUS_FULL:
		return chg_status;
	default:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
}

static int mt6853_bat_health(struct mt6853_bat *bat)
{
	if (!ignore_temp) {
		if (!bat->temp_ok)
			return POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		if (bat->hot)
			return POWER_SUPPLY_HEALTH_OVERHEAT;
		if (bat->cold)
			return POWER_SUPPLY_HEALTH_COLD;
	}

	if (bat->ovp)
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;

	return POWER_SUPPLY_HEALTH_GOOD;
}

static int mt6853_bat_ocv_to_soc(const struct power_supply_battery_ocv_table *tbl,
				 int len, int ocv)
{
	int i;

	if (ocv >= tbl[0].ocv)
		return tbl[0].capacity * 1000;
	if (ocv <= tbl[len - 1].ocv)
		return tbl[len - 1].capacity * 1000;

	for (i = 1; i < len - 1; i++)
		if (ocv > tbl[i].ocv)
			break;

	return tbl[i].capacity * 1000 +
	       div_s64((s64)(ocv - tbl[i].ocv) * (tbl[i - 1].capacity - tbl[i].capacity) * 1000,
		       tbl[i - 1].ocv - tbl[i].ocv);
}

/*
 * The charger ended the charge and the pack is close to the end-of-charge
 * voltage. A weak USB port may not cover the system load, so the latch is
 * dropped again once the load-compensated OCV of the last poll shows a real
 * discharge.
 */
static void mt6853_bat_update_full(struct mt6853_bat *bat, bool supplied, bool inhibit,
				   int chg_status)
{
	int cv = bat->info->constant_charge_voltage_max_uv;

	if (supplied && !inhibit && chg_status == POWER_SUPPLY_STATUS_FULL &&
	    bat->vbat_uv >= cv - BAT_FULL_MARGIN_UV)
		bat->full = true;
	else if (!supplied || bat->ocv_uv < cv - BAT_FULL_RELEASE_UV)
		bat->full = false;
}

static void mt6853_bat_gauge(struct mt6853_bat *bat, bool supplied, bool charging, s64 dt_ms)
{
	struct power_supply_battery_info *info = bat->info;
	const struct power_supply_battery_ocv_table *tbl;
	int temp_c = 25, len, i_ua, ocv, target, rise, fall;
	bool gaining;
	s64 r_uohm = 0, step;

	if (bat->temp_ok)
		temp_c = DIV_ROUND_CLOSEST(bat->temp_mc, 1000);

	if (bat->ibat_ok)
		i_ua = bat->ibat_ua;
	else if (charging)
		i_ua = info->constant_charge_current_max_ua / 2;
	else if (supplied)
		i_ua = 0;
	else
		i_ua = -BAT_ASSUMED_LOAD_UA;

	if (info->factory_internal_resistance_uohm > 0) {
		r_uohm = info->factory_internal_resistance_uohm;
		if (power_supply_supports_temp2ri(info))
			r_uohm = div_s64(r_uohm * power_supply_temp2resist_simple(info->resist_table,
								info->resist_table_size,
								temp_c), 100);
	}

	/* Positive current is charging and raises the terminal voltage above the OCV */
	ocv = bat->vbat_uv - clamp_t(s64, div_s64((s64)i_ua * r_uohm, 1000000),
				     -BAT_IR_COMP_MAX_UV, BAT_IR_COMP_MAX_UV);

	bat->ocv_uv = ocv;

	tbl = power_supply_find_ocv2cap_table(info, temp_c, &len);
	target = mt6853_bat_ocv_to_soc(tbl, len, ocv);

	/* A charger that cannot cover the system load still drains the pack */
	gaining = charging && !(bat->ibat_ok && bat->ibat_ua < 0);

	/* Single readings scatter by a few percent under a varying load */
	if (bat->soc_mp < 0) {
		bat->soc_mp = target;
	} else if (bat->settle_polls) {
		bat->soc_mp = (bat->soc_mp + target) / 2;
		bat->settle_polls--;
	} else {
		rise = gaining ? BAT_SOC_RISE_CHARGING : BAT_SOC_RISE_IDLE;
		fall = gaining ? BAT_SOC_FALL_CHARGING : BAT_SOC_FALL_IDLE;
		step = div_s64((s64)(target - bat->soc_mp) * min_t(s64, dt_ms, BAT_SOC_TAU_MS),
			       BAT_SOC_TAU_MS);
		step = clamp_t(s64, step, -(s64)fall * dt_ms / 1000, (s64)rise * dt_ms / 1000);
		bat->soc_mp += step;
	}

	if (bat->full)
		bat->soc_mp += div_s64(BAT_SOC_FULL_RAMP * dt_ms, 1000);
	else if (charging)
		bat->soc_mp = min(bat->soc_mp, BAT_SOC_FULL - 1000);
	bat->soc_mp = clamp(bat->soc_mp, 0, BAT_SOC_FULL);

	if (!supplied &&
	    bat->vbat_uv < (bat->ibat_ok ? BAT_EMPTY_UV : BAT_EMPTY_NO_IBAT_UV) &&
	    ocv < BAT_EMPTY_OCV_UV)
		bat->empty_polls = min_t(unsigned int, bat->empty_polls + 1, BAT_EMPTY_POLLS);
	else
		bat->empty_polls = 0;

	if (bat->empty_polls >= BAT_EMPTY_POLLS)
		bat->soc_mp = 0;
}

static int mt6853_bat_capacity(struct mt6853_bat *bat)
{
	if (bat->empty_polls >= BAT_EMPTY_POLLS)
		return 0;

	return clamp(DIV_ROUND_CLOSEST(bat->soc_mp, 1000), 1, 100);
}

static void mt6853_bat_update(struct mt6853_bat *bat)
{
	struct power_supply_battery_info *info = bat->info;
	struct mt6853_bat_snap old, snap = {};
	int chg_status = POWER_SUPPLY_STATUS_UNKNOWN;
	ktime_t now = ktime_get_boottime();
	union power_supply_propval val;
	bool supplied, inhibit;
	struct power_supply *chg;
	s64 dt_ms;
	int sup;

	mt6853_bat_measure(bat);
	mt6853_bat_limits(bat);
	inhibit = mt6853_bat_inhibit(bat);

	if (inhibit != bat->inhibited) {
		bat->inhibited = inhibit;
		dev_info(bat->dev, "charging %s (temp %d mC%s, vbat %d uV%s)\n",
			 inhibit ? "inhibited" : "allowed", bat->temp_mc,
			 bat->temp_ok ? "" : " invalid", bat->vbat_uv,
			 bat->ovp ? ", overvoltage" : "");
	}

	sup = power_supply_am_i_supplied(bat->psy);
	supplied = sup > 0;
	mt6853_bat_check_sign(bat, sup == 0);

	chg = power_supply_get_by_name("usb");
	if (chg) {
		mt6853_bat_charger(bat, chg, inhibit);
		if (!power_supply_get_property(chg, POWER_SUPPLY_PROP_STATUS, &val))
			chg_status = val.intval;
		power_supply_put(chg);
	}

	snap.status = mt6853_bat_status(bat, supplied, inhibit, chg_status);
	dt_ms = clamp_t(s64, ktime_ms_delta(now, bat->last_poll), 1, BAT_MAX_DT_MS);
	bat->last_poll = now;

	if (bat->vbat_ok) {
		mt6853_bat_update_full(bat, supplied, inhibit, chg_status);
		mt6853_bat_gauge(bat, supplied, snap.status == POWER_SUPPLY_STATUS_CHARGING, dt_ms);

		if (snap.status == POWER_SUPPLY_STATUS_NOT_CHARGING &&
		    bat->soc_mp >= BAT_SOC_FULL - 1000 &&
		    bat->vbat_uv >= info->constant_charge_voltage_max_uv - BAT_FULL_IDLE_MARGIN_UV)
			snap.status = POWER_SUPPLY_STATUS_FULL;
	}

	snap.ready = bat->soc_mp >= 0;
	snap.health = mt6853_bat_health(bat);
	snap.capacity = mt6853_bat_capacity(bat);
	snap.voltage_ok = bat->vbat_ok;
	snap.voltage_uv = bat->vbat_uv;
	snap.current_ok = bat->ibat_ok;
	snap.current_ua = bat->ibat_ua;
	snap.temp_ok = bat->temp_ok;
	snap.temp_dc = DIV_ROUND_CLOSEST(bat->temp_mc, 100);
	snap.charge_counter_uah = div_s64((s64)info->charge_full_design_uah * bat->soc_mp,
					  BAT_SOC_FULL);

	dev_dbg(bat->dev, "vbat %d uV (%d) ibat %d uA (%d) temp %d mC (%d) supplied %d charger %d soc %d status %d\n",
		bat->vbat_uv, bat->vbat_ok, bat->ibat_ua, bat->ibat_ok, bat->temp_mc,
		bat->temp_ok, sup, chg_status, bat->soc_mp, snap.status);

	mutex_lock(&bat->lock);
	old = bat->snap;
	bat->snap = snap;
	mutex_unlock(&bat->lock);

	if (snap.ready &&
	    (!old.ready || old.status != snap.status || old.health != snap.health ||
	     old.capacity != snap.capacity || old.voltage_ok != snap.voltage_ok ||
	     old.temp_ok != snap.temp_ok || abs(old.temp_dc - snap.temp_dc) >= 5))
		power_supply_changed(bat->psy);
}

static void mt6853_bat_work(struct work_struct *work)
{
	struct mt6853_bat *bat = container_of(work, struct mt6853_bat, work.work);

	mt6853_bat_update(bat);

	if (READ_ONCE(bat->started))
		queue_delayed_work(system_freezable_wq, &bat->work, msecs_to_jiffies(BAT_POLL_MS));
}

static void mt6853_bat_external_power_changed(struct power_supply *psy)
{
	struct mt6853_bat *bat = power_supply_get_drvdata(psy);

	if (READ_ONCE(bat->started))
		mod_delayed_work(system_freezable_wq, &bat->work, 0);
}

static int mt6853_bat_get_property(struct power_supply *psy, enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct mt6853_bat *bat = power_supply_get_drvdata(psy);
	struct mt6853_bat_snap snap;

	mutex_lock(&bat->lock);
	snap = bat->snap;
	mutex_unlock(&bat->lock);

	if (!snap.ready)
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = snap.status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = snap.health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = bat->info->technology;
		if (val->intval == POWER_SUPPLY_TECHNOLOGY_UNKNOWN)
			val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = snap.capacity;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (!snap.voltage_ok)
			return -ENODATA;
		val->intval = snap.voltage_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (!snap.current_ok)
			return -ENODATA;
		val->intval = snap.current_ua;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (!snap.temp_ok)
			return -ENODATA;
		val->intval = snap.temp_dc;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = snap.charge_counter_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->info->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = bat->info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = bat->info->voltage_min_design_uv;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property mt6853_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
};

static const struct power_supply_desc mt6853_bat_desc = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= mt6853_bat_props,
	.num_properties		= ARRAY_SIZE(mt6853_bat_props),
	.get_property		= mt6853_bat_get_property,
	.external_power_changed	= mt6853_bat_external_power_changed,
};

static int mt6853_bat_get_chan(struct device *dev, const char *name,
			       struct iio_channel **chan, bool optional)
{
	*chan = devm_iio_channel_get(dev, name);
	if (!IS_ERR(*chan))
		return 0;

	if (optional && PTR_ERR(*chan) == -ENODEV) {
		*chan = NULL;
		return 0;
	}

	return dev_err_probe(dev, PTR_ERR(*chan), "cannot get the %s channel\n", name);
}

static int mt6853_bat_check_info(struct device *dev, struct power_supply_battery_info *info)
{
	const struct power_supply_battery_ocv_table *tbl = info->ocv_table[0];
	int i;

	if (info->voltage_min_design_uv <= 0 ||
	    info->voltage_max_design_uv <= info->voltage_min_design_uv ||
	    info->charge_full_design_uah <= 0 ||
	    info->constant_charge_current_max_ua <= 0 ||
	    info->constant_charge_voltage_max_uv <= 0 ||
	    info->constant_charge_voltage_max_uv > info->voltage_max_design_uv ||
	    !tbl || info->ocv_table_size[0] < 2)
		return dev_err_probe(dev, -EINVAL, "incomplete battery description\n");

	for (i = 1; i < info->ocv_table_size[0]; i++)
		if (tbl[i].ocv >= tbl[i - 1].ocv)
			return dev_err_probe(dev, -EINVAL,
					     "OCV table is not strictly decreasing\n");

	return 0;
}

static void mt6853_bat_stop(void *data)
{
	struct mt6853_bat *bat = data;

	WRITE_ONCE(bat->started, false);
	disable_delayed_work_sync(&bat->work);
}

static int mt6853_bat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct power_supply_battery_info *info;
	struct mt6853_bat *bat;
	u32 val;
	int ret, i;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &bat->lock);
	if (ret)
		return ret;

	bat->dev = dev;
	bat->soc_mp = -1;
	bat->ibat_sign = 1;
	bat->settle_polls = BAT_SOC_SETTLE_POLLS;
	INIT_DELAYED_WORK(&bat->work, mt6853_bat_work);
	platform_set_drvdata(pdev, bat);

	ret = mt6853_bat_get_chan(dev, "vbat", &bat->vbat_chan, false);
	if (ret)
		return ret;
	ret = mt6853_bat_get_chan(dev, "ts", &bat->ts_chan, false);
	if (ret)
		return ret;
	ret = mt6853_bat_get_chan(dev, "vbif", &bat->vbif_chan, true);
	if (ret)
		return ret;
	ret = mt6853_bat_get_chan(dev, "ibat", &bat->ibat_chan, true);
	if (ret)
		return ret;

	/* VBIF feeds the NTC divider and is not always-on */
	ret = devm_regulator_get_enable_optional(dev, "vbif28");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "cannot enable the vbif28 supply\n");
	if (!ret)
		msleep(20);

	cfg.fwnode = dev_fwnode(dev);
	cfg.drv_data = bat;
	bat->psy = devm_power_supply_register(dev, &mt6853_bat_desc, &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(dev, PTR_ERR(bat->psy), "cannot register the battery\n");

	ret = power_supply_get_battery_info(bat->psy, &bat->info);
	if (ret)
		return dev_err_probe(dev, ret, "no usable monitored battery\n");
	info = bat->info;

	ret = mt6853_bat_check_info(dev, info);
	if (ret)
		return ret;

	bat->alert_min_c = info->temp_alert_min == INT_MIN ? BAT_ALERT_MIN_DEFAULT_C :
			   info->temp_alert_min;
	bat->alert_max_c = info->temp_alert_max == INT_MAX ? BAT_ALERT_MAX_DEFAULT_C :
			   info->temp_alert_max;
	bat->hot_start_c = BAT_HOT_START_DEFAULT_C;
	bat->hot_cv_uv = BAT_HOT_CV_DEFAULT_UV;
	if (!device_property_read_u32(dev, "xiaomi,hot-charge-start-celsius", &val))
		bat->hot_start_c = val;
	if (!device_property_read_u32(dev, "xiaomi,hot-charge-voltage-microvolt", &val))
		bat->hot_cv_uv = val;
	bat->hot_cv_uv = min(bat->hot_cv_uv, info->constant_charge_voltage_max_uv);

	ret = devm_add_action_or_reset(dev, mt6853_bat_stop, bat);
	if (ret)
		return ret;

	for (i = 0; i < BAT_FIRST_POLL_TRIES && !bat->vbat_ok; i++) {
		if (i)
			msleep(50);
		mt6853_bat_update(bat);
	}
	if (!bat->vbat_ok)
		return dev_err_probe(dev, -ENODEV, "battery voltage is not readable\n");

	dev_info(dev, "vbat %d mV, temp %d mC (%s), ibat %d uA (%s), capacity %d%%\n",
		 bat->vbat_uv / 1000, bat->temp_mc, bat->temp_ok ? "ok" : "invalid",
		 bat->ibat_ua, bat->ibat_ok ? "ok" : "invalid", bat->snap.capacity);

	WRITE_ONCE(bat->started, true);
	queue_delayed_work(system_freezable_wq, &bat->work, msecs_to_jiffies(BAT_POLL_MS));

	return 0;
}

static int mt6853_bat_resume(struct device *dev)
{
	struct mt6853_bat *bat = dev_get_drvdata(dev);

	if (READ_ONCE(bat->started))
		mod_delayed_work(system_freezable_wq, &bat->work, 0);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6853_bat_pm_ops, NULL, mt6853_bat_resume);

static const struct of_device_id mt6853_bat_of_match[] = {
	{ .compatible = "xiaomi,cannong-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6853_bat_of_match);

static struct platform_driver mt6853_bat_driver = {
	.probe = mt6853_bat_probe,
	.driver = {
		.name = "mt6853-battery",
		.of_match_table = mt6853_bat_of_match,
		.pm = pm_sleep_ptr(&mt6853_bat_pm_ops),
	},
};
module_platform_driver(mt6853_bat_driver);

MODULE_DESCRIPTION("Battery monitor for the Redmi Note 9T (MT6853T)");
MODULE_LICENSE("GPL");
