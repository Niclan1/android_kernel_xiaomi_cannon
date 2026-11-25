#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#define EXPORT_SYMBOL_GPL(x)
#define EINVAL 22
#define ERANGE 34
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define ENODATA 61
enum power_supply_property { POWER_SUPPLY_PROP_TEMP_AMBIENT_ALERT_MIN, POWER_SUPPLY_PROP_TEMP_AMBIENT_ALERT_MAX, POWER_SUPPLY_PROP_TEMP_ALERT_MIN, POWER_SUPPLY_PROP_TEMP_ALERT_MAX, POWER_SUPPLY_PROP_TEMP_MIN, POWER_SUPPLY_PROP_TEMP_MAX, POWER_SUPPLY_PROP_TECHNOLOGY, POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN, POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN, POWER_SUPPLY_PROP_PRECHARGE_CURRENT, POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX };
enum { POWER_SUPPLY_TECHNOLOGY_UNKNOWN };
union power_supply_propval { int intval; };
struct power_supply_battery_info { int technology, energy_full_design_uwh, charge_full_design_uah, voltage_min_design_uv, voltage_max_design_uv, precharge_current_ua, charge_term_current_ua, constant_charge_current_max_ua, constant_charge_voltage_max_uv, temp_ambient_alert_min, temp_ambient_alert_max, temp_alert_min, temp_alert_max, temp_min, temp_max; };
#include "dc.c"
#include "real.c"
int main(void) { int f = 0; union power_supply_propval v;
 struct power_supply_battery_info i = { .temp_ambient_alert_min = INT_MIN, .temp_ambient_alert_max = INT_MAX, .temp_alert_min = 0, .temp_alert_max = 60, .temp_min = INT_MIN, .temp_max = INT_MAX };
 /* cannong DT alert-celsius = <0 60>: published as 0 / 600 deci-C, same unit as TEMP 341 */
 f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_ALERT_MAX, &v) || v.intval != 600;
 f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_ALERT_MIN, &v) || v.intval != 0;
 i.temp_alert_min = -10; f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_ALERT_MIN, &v) || v.intval != -100;
 f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_MAX, &v) != -EINVAL; /* unset stays unpublished, no overflow */
 f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_AMBIENT_ALERT_MIN, &v) != -EINVAL;
 i.temp_max = 55; i.temp_ambient_alert_max = 45; f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_MAX, &v) || v.intval != 550;
 f |= power_supply_battery_info_get_prop(&i, POWER_SUPPLY_PROP_TEMP_AMBIENT_ALERT_MAX, &v) || v.intval != 450;
 /* direct calls: unset sentinels and out-of-range stored values, val untouched */
 struct power_supply_battery_info u = { .temp_ambient_alert_min = INT_MIN, .temp_ambient_alert_max = INT_MAX, .temp_alert_min = INT_MIN, .temp_alert_max = INT_MAX, .temp_min = INT_MIN, .temp_max = INT_MAX };
 for (int p = POWER_SUPPLY_PROP_TEMP_AMBIENT_ALERT_MIN; p <= POWER_SUPPLY_PROP_TEMP_MAX; p++) { v.intval = 12345; f |= power_supply_battery_info_get_prop(&u, p, &v) != -EINVAL || v.intval != 12345; }
 u.temp_alert_max = INT_MAX - 1; u.temp_alert_min = INT_MIN + 1; u.temp_max = 214748365; u.temp_min = -214748365;
 v.intval = 7; f |= power_supply_battery_info_get_prop(&u, POWER_SUPPLY_PROP_TEMP_ALERT_MAX, &v) != -ERANGE || v.intval != 7;
 f |= power_supply_battery_info_get_prop(&u, POWER_SUPPLY_PROP_TEMP_ALERT_MIN, &v) != -ERANGE || v.intval != 7;
 f |= power_supply_battery_info_get_prop(&u, POWER_SUPPLY_PROP_TEMP_MAX, &v) != -ERANGE || v.intval != 7;
 f |= power_supply_battery_info_get_prop(&u, POWER_SUPPLY_PROP_TEMP_MIN, &v) != -ERANGE || v.intval != 7;
 u.temp_max = 214748364; f |= power_supply_battery_info_get_prop(&u, POWER_SUPPLY_PROP_TEMP_MAX, &v) || v.intval != 2147483640;
 f |= power_supply_battery_info_get_prop(NULL, POWER_SUPPLY_PROP_TEMP_MAX, &v) != -EINVAL;
 printf("%s\n", f ? "FAILED" : "PASSED"); return f; }
