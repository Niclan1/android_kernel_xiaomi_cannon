#pragma once
/* Host fakes for the kernel APIs used by drivers/power/supply/mt6360_charger.c */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifndef EPROBE_DEFER
#define EPROBE_DEFER 517
#endif
typedef uint8_t u8; typedef uint32_t u32;
typedef int irqreturn_t;
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_FALLING 2
#define IRQF_ONESHOT 0x2000
#define __maybe_unused __attribute__((unused))
#define THIS_MODULE NULL
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define module_param_named(n, v, t, p)
#define MODULE_PARM_DESC(n, d)
#define MODULE_DEVICE_TABLE(t, n)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define of_match_ptr(x) (x)
#define module_platform_driver(d) void *keep_##d = &d;
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
struct device { const char *name; struct device *parent; void *drvdata; };
extern int fake_log_err, fake_log_warn;
#define dev_err(d, f, ...) (fake_log_err++, printf("  ERR  " f, ##__VA_ARGS__))
#define dev_warn(d, f, ...) (fake_log_warn++, printf("  WARN " f, ##__VA_ARGS__))
#define dev_info(d, f, ...) printf("  INFO " f, ##__VA_ARGS__)
#define dev_dbg(d, f, ...) do { if (0) printf(f, ##__VA_ARGS__); } while (0)
static inline int dev_err_probe(struct device *d, int e, const char *f, ...) { (void)d; (void)f; fake_log_err++; return e; }
struct fwnode_handle; static inline struct fwnode_handle *dev_fwnode(struct device *d) { (void)d; return NULL; }
static inline int device_property_read_u32(struct device *d, const char *p, u32 *v) { (void)d; (void)p; (void)v; return -EINVAL; }
struct mutex { int locked; };
static inline void mutex_init(struct mutex *m) { m->locked = 0; }
static inline void mutex_lock(struct mutex *m) { assert(!m->locked && "recursive/held lock"); m->locked = 1; }
static inline void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
struct work_struct { void (*func)(struct work_struct *); int pending, cancelled; };
struct delayed_work { struct work_struct work; unsigned long delay; };
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define system_wq NULL
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
static inline bool schedule_work(struct work_struct *w) { assert(!w->cancelled); w->pending = 1; return true; }
static inline bool schedule_delayed_work(struct delayed_work *d, unsigned long j) { assert(!d->work.cancelled); d->work.pending = 1; d->delay = j; return true; }
#define INIT_WORK(w, f) (*(w) = (struct work_struct){ (f), 0, 0 })
#define INIT_DELAYED_WORK(d, f) ((d)->delay = 0, INIT_WORK(&(d)->work, f))
bool cancel_work_sync(struct work_struct *w); bool cancel_delayed_work_sync(struct delayed_work *d);
static inline bool mod_delayed_work(void *wq, struct delayed_work *d, unsigned long j) { (void)wq; return schedule_delayed_work(d, j); }
void msleep(unsigned int ms); void usleep_range(unsigned long a, unsigned long b);
struct regmap { u8 reg[0x400]; };
int regmap_read(struct regmap *m, unsigned int r, unsigned int *v);
int regmap_update_bits(struct regmap *m, unsigned int r, unsigned int mask, unsigned int v);
static inline struct regmap *dev_get_regmap(struct device *d, const char *n) { (void)n; return d->drvdata; }
struct linear_range { unsigned int min, min_sel, max_sel, step; };
#define LINEAR_RANGE_IDX(i, m, mi, ma, s) [i] = { .min = (m), .min_sel = (mi), .max_sel = (ma), .step = (s) }
int linear_range_get_value(const struct linear_range *r, unsigned int sel, unsigned int *val);
int linear_range_get_selector_within(const struct linear_range *r, unsigned int val, unsigned int *sel);
enum power_supply_property { POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CHARGE_TYPE, POWER_SUPPLY_PROP_ONLINE,
 POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
 POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
 POWER_SUPPLY_PROP_CURRENT_MAX, POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT, POWER_SUPPLY_PROP_PRECHARGE_CURRENT,
 POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, POWER_SUPPLY_PROP_USB_TYPE };
enum { POWER_SUPPLY_STATUS_UNKNOWN, POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_DISCHARGING, POWER_SUPPLY_STATUS_NOT_CHARGING, POWER_SUPPLY_STATUS_FULL };
enum { POWER_SUPPLY_CHARGE_TYPE_UNKNOWN, POWER_SUPPLY_CHARGE_TYPE_NONE, POWER_SUPPLY_CHARGE_TYPE_TRICKLE, POWER_SUPPLY_CHARGE_TYPE_FAST };
enum { POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO, POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE };
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_DCP, POWER_SUPPLY_USB_TYPE_CDP };
enum power_supply_type { POWER_SUPPLY_TYPE_MAINS = 3, POWER_SUPPLY_TYPE_USB = 4 };
union power_supply_propval { int intval; const char *strval; };
struct power_supply;
struct power_supply_desc { const char *name; enum power_supply_type type; const enum power_supply_property *properties; size_t num_properties;
 int (*get_property)(struct power_supply *, enum power_supply_property, union power_supply_propval *);
 int (*set_property)(struct power_supply *, enum power_supply_property, const union power_supply_propval *);
 int (*property_is_writeable)(struct power_supply *, enum power_supply_property); unsigned int charge_behaviours, usb_types; };
struct power_supply_config { void *drv_data; struct fwnode_handle *fwnode; char **supplied_to; size_t num_supplicants; };
struct power_supply { const struct power_supply_desc *desc; void *drv_data; };
static inline void *power_supply_get_drvdata(struct power_supply *p) { return p->drv_data; }
void power_supply_changed(struct power_supply *p);
struct power_supply *devm_power_supply_register(struct device *d, const struct power_supply_desc *desc, const struct power_supply_config *c);
struct regulator_dev { int x; };
struct regulator_ops { int (*list_voltage)(struct regulator_dev *, unsigned); int (*enable)(struct regulator_dev *);
 int (*disable)(struct regulator_dev *); int (*is_enabled)(struct regulator_dev *); int (*set_voltage_sel)(struct regulator_dev *, unsigned);
 int (*get_voltage_sel)(struct regulator_dev *); };
enum regulator_type { REGULATOR_VOLTAGE };
struct regulator_desc { const char *of_match, *name; const struct regulator_ops *ops; void *owner; enum regulator_type type;
 unsigned min_uV, uV_step, n_voltages, vsel_reg, vsel_mask, enable_reg, enable_mask; };
struct regulator_config { struct device *dev; struct regmap *regmap; };
int regulator_list_voltage_linear(struct regulator_dev *, unsigned); int regulator_enable_regmap(struct regulator_dev *);
int regulator_disable_regmap(struct regulator_dev *); int regulator_is_enabled_regmap(struct regulator_dev *);
int regulator_set_voltage_sel_regmap(struct regulator_dev *, unsigned); int regulator_get_voltage_sel_regmap(struct regulator_dev *);
struct regulator_dev *devm_regulator_register(struct device *d, const struct regulator_desc *r, const struct regulator_config *c);
struct platform_device { struct device dev; };
struct of_device_id { char compatible[128]; };
struct platform_device_id { char name[20]; unsigned long driver_data; };
struct platform_driver { struct { const char *name; const struct of_device_id *of_match_table; } driver;
 int (*probe)(struct platform_device *); const struct platform_device_id *id_table; };
static inline void platform_set_drvdata(struct platform_device *p, void *d) { p->dev.drvdata = d; }
static inline void *platform_get_drvdata(struct platform_device *p) { return p->dev.drvdata; }
int platform_get_irq_byname(struct platform_device *p, const char *n);
typedef irqreturn_t (*irq_handler_t)(int, void *);
int devm_request_threaded_irq(struct device *d, int irq, irq_handler_t h, irq_handler_t t, unsigned long f, const char *n, void *data);
void *devm_kzalloc(struct device *d, size_t s, int gfp);
#define GFP_KERNEL 0
int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data);
int devm_work_autocancel(struct device *d, struct work_struct *w, void (*fn)(struct work_struct *));
int devm_delayed_work_autocancel(struct device *d, struct delayed_work *w, void (*fn)(struct work_struct *));
enum phy_mode { PHY_MODE_INVALID, PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE, PHY_MODE_USB_OTG };
struct phy; struct phy_ops { int (*set_mode)(struct phy *, enum phy_mode, int); };
struct phy { int init_count; struct { enum phy_mode mode; } attrs; struct mutex mutex; const struct phy_ops *ops; struct device dev; void *drvdata; };
static inline void *phy_get_drvdata(struct phy *p) { return p->drvdata; }
static inline void *dev_get_drvdata(struct device *d) { return d->drvdata; }
static inline enum phy_mode phy_get_mode(struct phy *p) { return p->attrs.mode; }
struct phy *devm_phy_optional_get(struct device *d, const char *n);
int phy_set_mode_ext(struct phy *p, enum phy_mode m, int sub);