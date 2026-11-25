#include "kstub.h"
#include <stdarg.h>
int fake_log_err, fake_log_warn;
/* ---- fake hardware ---- */
static struct regmap hw; static int hw_type = 0; /* USB_STATUS1 type when detector runs */
static int fail_reg = -1, fail_after = 0, fail_hits;    /* regmap fault injection */
static unsigned long vtime; static void (*sleep_hook)(unsigned int);
static struct phy uphy; static int phy_fail_on = -1, phy_calls; static int phy_present = 1;
static int notify_cnt;
static int maybe_fail(unsigned int r) { if ((int)r == fail_reg) { if (fail_after > 0) { fail_after--; return 0; } fail_hits++; return -EIO; } return 0; }
int regmap_read(struct regmap *m, unsigned int r, unsigned int *v) { if (maybe_fail(r)) return -EIO; *v = m->reg[r]; return 0; }
int regmap_update_bits(struct regmap *m, unsigned int r, unsigned int mask, unsigned int v) {
	if (maybe_fail(r)) return -EIO; m->reg[r] = (m->reg[r] & ~mask) | (v & mask);
	if (r == 0x322) { if ((m->reg[r] & 0x80) && (hw.reg[0x3E7] & 0x10)) hw.reg[0x327] = 1 << 4; else if (!(m->reg[r] & 0x80)) hw.reg[0x327] = 0; }
	return 0; }
void msleep(unsigned int ms) { vtime += ms; if (sleep_hook) sleep_hook(ms); }
void usleep_range(unsigned long a, unsigned long b) { (void)b; vtime += a / 1000; }
int linear_range_get_value(const struct linear_range *r, unsigned int sel, unsigned int *val) { if (sel < r->min_sel || sel > r->max_sel) return -EINVAL; *val = r->min + (sel - r->min_sel) * r->step; return 0; }
int linear_range_get_selector_within(const struct linear_range *r, unsigned int val, unsigned int *sel) {
	if (val < r->min) { *sel = r->min_sel; return 0; } *sel = r->min_sel + (val - r->min) / r->step; if (*sel > r->max_sel) *sel = r->max_sel; return 0; }
void power_supply_changed(struct power_supply *p) { (void)p; notify_cnt++; }
static struct power_supply psys[2]; static int npsy;
struct power_supply *devm_power_supply_register(struct device *d, const struct power_supply_desc *desc, const struct power_supply_config *c) { (void)d; psys[npsy].desc = desc; psys[npsy].drv_data = c->drv_data; return &psys[npsy++]; }
int regulator_list_voltage_linear(struct regulator_dev *r, unsigned s) { (void)r; return s; }
int regulator_enable_regmap(struct regulator_dev *r) { (void)r; return 0; }
int regulator_disable_regmap(struct regulator_dev *r) { (void)r; return 0; }
int regulator_is_enabled_regmap(struct regulator_dev *r) { (void)r; return 0; }
int regulator_set_voltage_sel_regmap(struct regulator_dev *r, unsigned s) { (void)r; (void)s; return 0; }
int regulator_get_voltage_sel_regmap(struct regulator_dev *r) { (void)r; return 0; }
static struct regulator_dev rdev; static int fail_regulator;
struct regulator_dev *devm_regulator_register(struct device *d, const struct regulator_desc *r, const struct regulator_config *c) { (void)d; (void)r; (void)c; return fail_regulator ? ERR_PTR(-EPROBE_DEFER) : &rdev; }
static irq_handler_t h_attach, h_chrdet; static void *irq_data;
int platform_get_irq_byname(struct platform_device *p, const char *n) { (void)p; return n[0] == 'a' ? 10 : 11; }
int devm_request_threaded_irq(struct device *d, int irq, irq_handler_t h, irq_handler_t t, unsigned long f, const char *n, void *data) {
	(void)d; (void)h; (void)f; (void)n; if (irq == 10) h_attach = t; else h_chrdet = t; irq_data = data;
	extern int devm_add_action_or_reset(struct device *, void (*)(void *), void *); void free_irq_fake(void *);
	return devm_add_action_or_reset(d, free_irq_fake, (void *)(intptr_t)irq); }
void free_irq_fake(void *i) { if ((intptr_t)i == 10) h_attach = NULL; else h_chrdet = NULL; }
/* devm: allocations + actions, released in reverse order */
struct devres { int kind; void (*fn)(void *); void *data; };
static struct devres dr[32]; static int ndr;
void *devm_kzalloc(struct device *d, size_t s, int gfp) { (void)d; (void)gfp; void *p = calloc(1, s); dr[ndr++] = (struct devres){ 0, free, p }; return p; }
int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data) { (void)d; dr[ndr++] = (struct devres){ 1, fn, data }; return 0; }
static void *cancelled_w[64]; static int ncancelled;
static void cancel_w(void *w) { struct work_struct *ws = w; ws->pending = 0; ws->cancelled = 1; cancelled_w[ncancelled++ % 64] = w; }
static bool was_cancelled(void *w) { for (int i = 0; i < ncancelled && i < 64; i++) if (cancelled_w[i] == w) return true; return false; }
int devm_work_autocancel(struct device *d, struct work_struct *w, void (*fn)(struct work_struct *)) { (void)d; *w = (struct work_struct){ fn, 0, 0 }; dr[ndr++] = (struct devres){ 1, cancel_w, w }; return 0; }
int devm_delayed_work_autocancel(struct device *d, struct delayed_work *w, void (*fn)(struct work_struct *)) { return devm_work_autocancel(d, &w->work, fn); }
/* runs while a work is being cancelled, as if it were executing concurrently */
static void (*cancel_hook)(struct work_struct *);
bool cancel_work_sync(struct work_struct *w) { if (cancel_hook) cancel_hook(w); bool p = w->pending; cancel_w(w); return p; }
bool cancel_delayed_work_sync(struct delayed_work *d) { return cancel_work_sync(&d->work); }
static void devres_release_all(void) { while (ndr) { ndr--; dr[ndr].fn(dr[ndr].data); } h_attach = h_chrdet = NULL; npsy = 0; }
struct phy *devm_phy_optional_get(struct device *d, const char *n) { (void)d; (void)n; return phy_present ? &uphy : NULL; }
/* environment for the real phy-mtk-tphy.c / phy-core.c functions in tphy_real.c */
#define __iomem
#define PHY_TYPE_USB2 3
#define PHY_TYPE_USB3 4
struct clk_bulk_data { int x; };
struct u2phy_banks { void *com; };
struct mtk_phy_instance { struct u2phy_banks u2_banks; struct clk_bulk_data clks[2]; u32 type; bool bc11_sw; };
struct mtk_tphy { int x; };
static u32 tphy_mmio[0x40]; static int clk_on, clk_fail_err;
static inline u32 readl(const void *a) { return *(const volatile u32 *)a; }
static inline void writel(u32 v, void *a) { *(volatile u32 *)a = v; }
static inline void udelay(unsigned long u) { (void)u; }
static inline int clk_bulk_prepare_enable(int n, struct clk_bulk_data *c) { (void)n; (void)c; if (clk_fail_err) return clk_fail_err; clk_on++; return 0; }
static inline void clk_bulk_disable_unprepare(int n, struct clk_bulk_data *c) { (void)n; (void)c; assert(clk_on > 0); clk_on--; }
#include "tphy_real.c"
#include "mtu3_env.h"
/* PHY role from the real IDDIG force bits */
static int phy_iddig(void) { u32 v = tphy_mmio[U3P_U2PHYDTM1 / 4]; if (!(v & P2C_FORCE_IDDIG)) return PHY_MODE_USB_OTG; return (v & P2C_RG_IDDIG) ? PHY_MODE_USB_DEVICE : PHY_MODE_USB_HOST; }
#define phy_bc11 (!!(tphy_mmio[0x18 / 4] & BIT(23)))
static struct mtk_phy_instance tinst; static struct mtk_tphy ttphy; static struct device tphy_dev;
static int phy_fail_cnt, phy_fail_err = -ETIMEDOUT, phy_ok_calls;
/* fault injection only: a failing request gets a failing clock in the real path */
static int test_set_mode(struct phy *p, enum phy_mode m, int sub) {
	phy_calls++; int saved = clk_fail_err;
	if (sub == phy_fail_on && phy_fail_cnt) { if (phy_fail_cnt > 0) phy_fail_cnt--; clk_fail_err = phy_fail_err; }
	int r = mtk_phy_set_mode(p, m, sub); clk_fail_err = saved; if (!r) phy_ok_calls++; return r; }
static const struct phy_ops test_phy_ops = { .set_mode = test_set_mode };
static void tphy_reset(enum phy_mode m) {
	memset(tphy_mmio, 0, sizeof(tphy_mmio)); memset(&tinst, 0, sizeof(tinst)); tinst.type = PHY_TYPE_USB2; tinst.u2_banks.com = tphy_mmio;
	tphy_dev.drvdata = &ttphy; uphy.dev.parent = &tphy_dev; uphy.drvdata = &tinst; uphy.ops = &test_phy_ops; uphy.attrs.mode = m; mutex_init(&uphy.mutex); clk_on = 0; }
#include "mt6360_charger.c"