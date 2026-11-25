#include "fakes.c"
static struct platform_device parent_pd, pd; static struct mt6360_chg_info *M; static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __func__, __LINE__, #c); fails++; } } while (0)
static unsigned aicr(void) { return 100000 + ((hw.reg[0x313] >> 2) & 0x3f) * 50000; }
static bool usbchgen(void) { return hw.reg[0x322] & 0x80; }
static void run_works(void) {
	for (int i = 0; i < 1000; i++) {
		if (M->chrdet_work.pending) { M->chrdet_work.pending = 0; M->chrdet_work.func(&M->chrdet_work); continue; }
		if (M->bc12_wait_work.work.pending) { M->bc12_wait_work.work.pending = 0; vtime += M->bc12_wait_work.delay; M->bc12_wait_work.work.func(&M->bc12_wait_work.work); continue; }
		if (M->bc12_release_work.work.pending) { M->bc12_release_work.work.pending = 0; vtime += M->bc12_release_work.delay; M->bc12_release_work.work.func(&M->bc12_release_work.work); continue; }
		return; }
	assert(0 && "work livelock"); }
static void vbus(bool on) { if (on) hw.reg[0x3E7] |= 0x10; else hw.reg[0x3E7] &= ~0x10; if (!on) hw.reg[0x327] = 0; h_chrdet(11, irq_data); run_works(); }
static void detect(int type) { hw.reg[0x327] = type << 4; h_attach(10, irq_data); run_works(); }
static int step_mode; static int probe(void) {
	pd.dev.parent = &parent_pd.dev; parent_pd.dev.drvdata = &hw; npsy = 0;
	int ret = mt6360_charger_probe(&pd); M = pd.dev.drvdata; if (!ret && !step_mode) run_works(); return ret; }
static void setup(bool vb, int ready, int dcdtout) {
	if (ndr) devres_release_all();
	memset(&hw, 0, sizeof(hw)); fail_reg = -1; fail_after = 0; fail_hits = 0; phy_fail_on = -1; phy_calls = 0;
	tphy_reset(PHY_MODE_USB_DEVICE); phy_present = 1; cancel_hook = NULL; phy_fail_cnt = 0; phy_fail_err = -ETIMEDOUT; phy_ok_calls = 0; uphy.init_count = ready; sleep_hook = NULL; fail_regulator = 0; vtime = 0;
	fake_log_err = fake_log_warn = 0; hw.reg[0x313] = 0xff; /* AICR left high by a previous owner */
	if (dcdtout) hw.reg[0x322] |= 0x40; if (vb) hw.reg[0x3E7] |= 0x10;
	assert(probe() == 0); fake_log_warn = 0; }
static void clean_idle(const char *w) { (void)w; CHECK(clk_on == 0); CHECK(!tinst.bc11_sw); CHECK(!phy_bc11); CHECK(!M->bc11_sw); CHECK(!usbchgen()); CHECK(aicr() == 500000); CHECK(M->psy_usb_type == POWER_SUPPLY_USB_TYPE_UNKNOWN); }
static void t_unattached(void) { setup(0, 1, 0); clean_idle(__func__); CHECK(phy_calls == 0); }
static void t_type(int type, unsigned want, int usbt, bool ac, bool keep) {
	setup(0, 1, 0); vbus(1); CHECK(usbchgen()); CHECK(phy_bc11); CHECK(vtime >= 180); CHECK(aicr() == 500000);
	detect(type); CHECK(aicr() == want); CHECK(M->psy_usb_type == usbt); CHECK(phy_bc11 == keep);
	union power_supply_propval v; mt6360_ac_get_property(&psys[1], POWER_SUPPLY_PROP_ONLINE, &v); CHECK(v.intval == ac);
	mt6360_charger_get_online(M, &v); CHECK(v.intval == !ac);
	vbus(0); clean_idle(__func__); }
static void t_types(void) {
	t_type(MT6360_CHG_TYPE_SDP, 500000, POWER_SUPPLY_USB_TYPE_SDP, 0, 0);
	t_type(MT6360_CHG_TYPE_SDPNSTD, 1000000, POWER_SUPPLY_USB_TYPE_SDP, 1, 0);
	t_type(MT6360_CHG_TYPE_CDP, 1500000, POWER_SUPPLY_USB_TYPE_CDP, 0, 0);
	t_type(MT6360_CHG_TYPE_DCP, 1500000, POWER_SUPPLY_USB_TYPE_DCP, 1, 1); }
static void t_dcdtout(void) { setup(0, 1, 1); vbus(1); CHECK(vtime < 180); CHECK(usbchgen()); detect(MT6360_CHG_TYPE_DCP); CHECK(aicr() == 1500000); vbus(0); clean_idle(__func__); }
static void t_ready_never(void) { setup(0, 0, 0); vbus(1); CHECK(vtime >= 19900 && vtime <= 20100); CHECK(fake_log_warn == 1); CHECK(phy_calls == 0); clean_idle(__func__); vbus(0); clean_idle(__func__); }
static int late_at; static void late_hook(unsigned ms) { (void)ms; }
static void t_ready_late_earlyboot(void) {
	/* wall charger present at probe, controller initialises its PHY 3 s later */
	step_mode = 1; setup(1, 0, 0); step_mode = 0; M->chrdet_work.pending = 0; M->chrdet_work.func(&M->chrdet_work); CHECK(!usbchgen()); late_at = 30; (void)late_hook;
	for (int i = 0; i < late_at; i++) { CHECK(M->bc12_wait_work.work.pending); M->bc12_wait_work.work.pending = 0; vtime += 100; M->bc12_wait_work.work.func(&M->bc12_wait_work.work); }
	uphy.init_count = 1; run_works(); CHECK(usbchgen()); CHECK(phy_bc11);
	detect(MT6360_CHG_TYPE_DCP); CHECK(aicr() == 1500000); CHECK(phy_bc11);
	h_chrdet(11, irq_data); run_works(); CHECK(aicr() == 1500000); CHECK(phy_bc11); /* spurious chrdet keeps DCP */
	vbus(0); clean_idle(__func__); }
static void unplug_hook(unsigned ms) { (void)ms; hw.reg[0x3E7] &= ~0x10; }
static void t_unplug_in_settle(void) {
	setup(0, 1, 0); sleep_hook = unplug_hook; vbus(1); sleep_hook = NULL; CHECK(!phy_bc11); CHECK(!usbchgen());
	h_chrdet(11, irq_data); run_works(); clean_idle(__func__); CHECK(!M->pwr_rdy); }
static void t_unplug_in_wait(void) {
	setup(0, 0, 0); hw.reg[0x3E7] |= 0x10; h_chrdet(11, irq_data);
	for (int i = 0; i < 5; i++) { M->bc12_wait_work.work.pending = 0; M->bc12_wait_work.work.func(&M->bc12_wait_work.work); }
	vbus(0); uphy.init_count = 1; run_works(); clean_idle(__func__); CHECK(phy_calls == 0); }
static void t_unplug_before_attach_irq(void) {
	setup(0, 1, 0); vbus(1); CHECK(phy_bc11); vbus(0); clean_idle(__func__);
	detect(MT6360_CHG_TYPE_DCP); clean_idle(__func__); CHECK(fake_log_warn == 0); }
static void t_replug_preempt(void) {
	setup(0, 1, 0); vbus(1); detect(MT6360_CHG_TYPE_DCP); vbus(0); vbus(1); CHECK(aicr() == 500000); CHECK(phy_bc11);
	detect(MT6360_CHG_TYPE_SDP); CHECK(!phy_bc11); CHECK(aicr() == 500000);
	/* repeated/stale attach while already decided */
	detect(MT6360_CHG_TYPE_SDP); CHECK(!phy_bc11); vbus(0); clean_idle(__func__); }
static void t_phy_take_fail(void) { setup(0, 1, 0); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_ON; phy_fail_cnt = -1; vbus(1); clean_idle(__func__); CHECK(fake_log_warn == 1); vbus(0); clean_idle(__func__); }
#define RW (M->bc12_release_work.work)
static bool not_published(void) { return aicr() == 500000 && M->psy_usb_type == POWER_SUPPLY_USB_TYPE_UNKNOWN && M->bc12_type == MT6360_CHG_TYPE_NOVBUS; }
static void step_release(void) { RW.pending = 0; vtime += M->bc12_release_work.delay; RW.func(&RW); }
/* one-shot give-back failure recovers without unplug, publish only after release */
static void t_release_oneshot(int type, unsigned want) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = 1;
	hw.reg[0x327] = type << 4; h_attach(10, irq_data);
	CHECK(not_published()); CHECK(!usbchgen()); CHECK(phy_bc11); CHECK(RW.pending); CHECK(M->bc12_release_work.delay == 200);
	run_works(); CHECK(!phy_bc11); CHECK(!M->bc11_sw); CHECK(aicr() == want); CHECK(M->bc12_type == type); CHECK(!RW.pending);
	vbus(0); clean_idle(__func__); }
/* persistent retryable failure: bounded retries, explicit error, never published */
static void t_release_persistent(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = -1; fake_log_err = 0; unsigned long t0 = vtime;
	detect(MT6360_CHG_TYPE_CDP); CHECK(not_published()); CHECK(!RW.pending); CHECK(!usbchgen());
	CHECK(fake_log_err == 1 + 1 + MT6360_BC12_RELEASE_MAX); CHECK(vtime - t0 == 200 * MT6360_BC12_RELEASE_MAX);
	h_attach(10, irq_data); run_works(); CHECK(not_published()); /* stale irq on a disabled detector */
	phy_fail_cnt = 0; vbus(0); clean_idle(__func__); }
/* permanent provider error: no retry at all */
static void t_release_permanent(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = -1; phy_fail_err = -EINVAL; fake_log_err = 0;
	hw.reg[0x327] = MT6360_CHG_TYPE_SDP << 4; h_attach(10, irq_data); CHECK(!RW.pending); CHECK(not_published()); CHECK(fake_log_err == 2);
	phy_fail_cnt = 0; vbus(0); clean_idle(__func__); }
/* detach while a retry is pending: the old result is never published */
static void t_release_detach(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = 1;
	hw.reg[0x327] = MT6360_CHG_TYPE_SDP << 4; h_attach(10, irq_data); CHECK(RW.pending); unsigned g = M->bc12_release_gen;
	hw.reg[0x3E7] &= ~0x10; h_chrdet(11, irq_data); CHECK(M->bc12_gen != g); CHECK(!phy_bc11);
	step_release(); clean_idle(__func__);
	/* new plug while the stale retry is still queued */
	phy_fail_cnt = 1; vbus(1); hw.reg[0x327] = MT6360_CHG_TYPE_SDP << 4; h_attach(10, irq_data); CHECK(RW.pending);
	hw.reg[0x3E7] &= ~0x10; h_chrdet(11, irq_data); hw.reg[0x3E7] |= 0x10; h_chrdet(11, irq_data);
	CHECK(RW.pending); step_release(); CHECK(not_published()); /* stale generation dropped */
	run_works(); CHECK(usbchgen()); detect(MT6360_CHG_TYPE_CDP); CHECK(aicr() == 1500000); CHECK(!phy_bc11);
	vbus(0); clean_idle(__func__); }
/* detach give-back failing too is retried, and a later plug restarts detection */
static void t_release_detach_fail(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = 1;
	vbus(0); CHECK(!phy_bc11); CHECK(!RW.pending); clean_idle(__func__);
	vbus(1); phy_fail_cnt = 2; hw.reg[0x3E7] &= ~0x10; hw.reg[0x327] = 0; h_chrdet(11, irq_data);
	CHECK(phy_bc11); CHECK(RW.pending); hw.reg[0x3E7] |= 0x10; h_chrdet(11, irq_data); /* replug while blocked */
	CHECK(!M->bc12_wait_work.work.pending); run_works(); CHECK(!phy_bc11 || usbchgen()); CHECK(usbchgen());
	detect(MT6360_CHG_TYPE_DCP); CHECK(aicr() == 1500000); vbus(0); clean_idle(__func__); }
/* teardown with a pending retry */
static void t_release_teardown(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = 1;
	hw.reg[0x327] = MT6360_CHG_TYPE_SDP << 4; h_attach(10, irq_data); CHECK(RW.pending); CHECK(not_published());
	struct work_struct *rw = &RW; ncancelled = 0; devres_release_all(); CHECK(was_cancelled(rw)); CHECK(!phy_bc11); CHECK(!usbchgen());
	setup(0, 1, 0); vbus(1); phy_fail_cnt = -1; phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; fake_log_err = 0;
	hw.reg[0x327] = MT6360_CHG_TYPE_SDP << 4; h_attach(10, irq_data); rw = &RW; ncancelled = 0; devres_release_all();
	CHECK(was_cancelled(rw)); CHECK(phy_bc11); CHECK(fake_log_err == 3); /* failure stays explicit, no false success */
	tphy_reset(PHY_MODE_USB_DEVICE); phy_fail_cnt = 0; }
/* USB controller switches the PHY to host while the detector owns DP/DM */
static void t_host_preempt(void) {
	setup(0, 1, 0); vbus(1); CHECK(phy_bc11); CHECK(usbchgen());
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, 0) == 0); CHECK(!phy_bc11); /* host owns DP/DM */
	detect(MT6360_CHG_TYPE_SDP); CHECK(!phy_bc11); CHECK(!M->bc11_sw); CHECK(!usbchgen()); CHECK(aicr() == 500000);
	vbus(0); clean_idle(__func__);
	vbus(1); CHECK(!phy_bc11); CHECK(!usbchgen()); CHECK(not_published()); /* host role: never take DP/DM */
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, MTK_TPHY_USB_SUBMODE_BC11_ON) == -EBUSY);
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_DEVICE, MTK_TPHY_USB_SUBMODE_BC11_ON) == -EBUSY); /* stale mode */
	CHECK(!phy_bc11); vbus(0); clean_idle(__func__); }
/* teardown races: a wait work running during quiesce re-queues itself / a release retry */
static struct mt6360_chg_info *race_m; static int race_mode;
static void race_hook(struct work_struct *w) {
	if (w != &race_m->chrdet_work) return; cancel_hook = NULL;
	if (race_mode == 0) { race_m->bc12_wait_work.work.func(&race_m->bc12_wait_work.work); }
	if (race_mode == 1) { race_m->bc12_release_cnt = 0; race_m->bc12_release_gen = race_m->bc12_gen;
		race_m->bc12_release_work.work.func(&race_m->bc12_release_work.work); }
	if (race_mode == 2) { hw.reg[0x3E7] |= 0x10; race_m->chrdet_work.func(&race_m->chrdet_work); } }
static void t_teardown_race(void) {
	for (race_mode = 0; race_mode < 3; race_mode++) {
		setup(0, 0, 0); hw.reg[0x3E7] |= 0x10; h_chrdet(11, irq_data); CHECK(M->bc12_wait_work.work.pending);
		if (race_mode == 1) { uphy.init_count = 1; run_works(); CHECK(phy_bc11); }
		race_m = M; struct mt6360_chg_info *m = M; ncancelled = 0; cancel_hook = race_hook; devres_release_all();
		CHECK(was_cancelled(&m->bc12_wait_work.work)); CHECK(was_cancelled(&m->bc12_release_work.work)); CHECK(was_cancelled(&m->chrdet_work));
		CHECK(h_attach == NULL && h_chrdet == NULL); CHECK(!phy_bc11); CHECK(!usbchgen()); CHECK(clk_on == 0);
		tphy_reset(PHY_MODE_USB_DEVICE); phy_fail_cnt = 0; } }
/* probe failing after the bc12/quiesce actions: unwinds with DP/DM released, irqs gone */
static void t_probe_fail_late(void) {
	setup(1, 1, 0); devres_release_all(); memset(&hw, 0, sizeof(hw)); hw.reg[0x3E7] = 0x10; tphy_reset(PHY_MODE_USB_DEVICE);
	fail_regulator = 1; CHECK(probe() == -EPROBE_DEFER); CHECK(h_attach != NULL);
	h_chrdet(11, irq_data); run_works(); CHECK(phy_bc11); /* irq raced the failing probe */
	devres_release_all(); CHECK(!phy_bc11); CHECK(!usbchgen()); CHECK(h_chrdet == NULL); fail_regulator = 0; }
/* attach result arriving after the controller switched to host: never published */
static void t_host_late_attach(int type) {
	setup(0, 1, 0); vbus(1); CHECK(phy_bc11); CHECK(M->bc12_en);
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, 0) == 0); CHECK(!phy_bc11); CHECK(phy_iddig() == PHY_MODE_USB_HOST);
	detect(type); CHECK(not_published()); CHECK(!usbchgen()); CHECK(!M->bc12_en); CHECK(!M->bc11_sw); CHECK(!phy_bc11); CHECK(uphy.attrs.mode == PHY_MODE_USB_HOST);
	vbus(0); clean_idle(__func__); }
/* stale release retry after a host switch must not publish */
static void t_host_stale_release(void) {
	setup(0, 1, 0); vbus(1); phy_fail_on = MTK_TPHY_USB_SUBMODE_BC11_OFF; phy_fail_cnt = 1;
	hw.reg[0x327] = MT6360_CHG_TYPE_CDP << 4; h_attach(10, irq_data); CHECK(RW.pending); CHECK(not_published());
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, 0) == 0); CHECK(!phy_bc11);
	run_works(); CHECK(not_published()); CHECK(!M->bc11_sw); CHECK(!usbchgen()); vbus(0); clean_idle(__func__); }
/* host switch while clocks fail: the role change itself fails, DP/DM untouched, controller sees the error */
static void t_host_switch_clk_fail(void) {
	setup(0, 1, 0); vbus(1); CHECK(phy_bc11); int iddig0 = phy_iddig(); clk_fail_err = -EIO;
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, 0) == -EIO); clk_fail_err = 0;
	CHECK(phy_bc11); CHECK(phy_iddig() == iddig0); CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(clk_on == 0);
	CHECK(mtk_phy_set_mode(&uphy, PHY_MODE_USB_HOST, 0) == 0); CHECK(!phy_bc11); CHECK(clk_on == 0);
	vbus(0); clean_idle(__func__); }
/* attach status read failure: detector stopped, DP/DM returned, nothing published */
static void t_attach_read_fail(void) {
	setup(0, 1, 0); vbus(1); fail_reg = 0x327; fake_log_err = 0; h_attach(10, irq_data); fail_reg = -1;
	CHECK(fake_log_err == 1); CHECK(!usbchgen()); CHECK(!phy_bc11); CHECK(!M->bc11_sw); CHECK(not_published());
	vbus(0); clean_idle(__func__); }
/* VBUS gone at the attach irq but chrdet not yet handled: dropped */
static void t_attach_novbus(void) {
	setup(0, 1, 0); vbus(1); hw.reg[0x3E7] &= ~0x10; hw.reg[0x327] = MT6360_CHG_TYPE_DCP << 4; h_attach(10, irq_data);
	CHECK(not_published()); CHECK(!phy_bc11); CHECK(!usbchgen()); h_chrdet(11, irq_data); run_works(); clean_idle(__func__); }
static void t_phy_contract(void) {
	tphy_reset(PHY_MODE_USB_DEVICE); clk_fail_err = -EIO;
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_DEVICE, MTK_TPHY_USB_SUBMODE_BC11_ON) == -EIO); CHECK(!phy_bc11); CHECK(clk_on == 0); clk_fail_err = 0;
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_DEVICE, MTK_TPHY_USB_SUBMODE_BC11_ON) == 0); CHECK(phy_bc11); CHECK(clk_on == 0); CHECK(!uphy.mutex.locked);
	tinst.type = PHY_TYPE_USB3; CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_DEVICE, MTK_TPHY_USB_SUBMODE_BC11_OFF) == -EINVAL); tinst.type = PHY_TYPE_USB2;
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_HOST, MTK_TPHY_USB_SUBMODE_BC11_OFF) == -EBUSY); CHECK(phy_bc11);
	CHECK(phy_set_mode_ext(&uphy, PHY_MODE_USB_DEVICE, MTK_TPHY_USB_SUBMODE_BC11_OFF) == 0); CHECK(!phy_bc11); CHECK(!uphy.mutex.locked); }static void t_phy_release_fail(void) {
	t_release_oneshot(MT6360_CHG_TYPE_SDP, 500000); t_release_oneshot(MT6360_CHG_TYPE_CDP, 1500000); t_release_oneshot(MT6360_CHG_TYPE_SDPNSTD, 1000000);
	t_release_persistent(); t_release_permanent(); t_release_detach(); t_release_detach_fail(); t_release_teardown(); t_host_preempt(); t_teardown_race(); t_probe_fail_late();
	t_host_late_attach(MT6360_CHG_TYPE_DCP); t_host_late_attach(MT6360_CHG_TYPE_CDP); t_host_late_attach(MT6360_CHG_TYPE_SDP);
	t_host_stale_release(); t_host_switch_clk_fail(); t_attach_read_fail(); t_attach_novbus(); t_phy_contract(); }static void t_regmap(unsigned reg, int after, int phase) {
	setup(0, 1, 0); if (phase == 0) { fail_reg = reg; fail_after = after; }
	vbus(1); if (phase == 1) { fail_reg = reg; fail_after = after; detect(MT6360_CHG_TYPE_SDP); }
	fail_reg = -1;
	if (phase == 1) { CHECK(aicr() == 500000); }
	if (phase == 0) { CHECK(aicr() == 500000); CHECK(!phy_bc11 || usbchgen()); }
	vbus(0); clean_idle(__func__); CHECK(fail_hits >= 1); }
static void t_regmaps(void) {
	t_regmap(0x322, 0, 0); /* stop before start */
	t_regmap(0x322, 2, 0); /* DCDTOUTEN read ok? fails USBCHGEN set */
	t_regmap(0x3E7, 1, 0); /* pwr_rdy re-check after settle */
	t_regmap(0x327, 0, 1); /* attach status read */
	t_regmap(0x313, 0, 1); /* AICR write */ }
static void t_attach_unknown(int st) { setup(0, 1, 0); vbus(1); detect(st); CHECK(aicr() == 500000); vbus(0); clean_idle(__func__); }
static void t_teardown(void) {
	setup(0, 1, 0); vbus(1); detect(MT6360_CHG_TYPE_DCP); CHECK(phy_bc11); devres_release_all(); CHECK(!phy_bc11); CHECK(!usbchgen());
	setup(0, 0, 0); vbus(1); CHECK(!M->bc12_wait_work.work.pending); hw.reg[0x3E7] |= 0x10; M->pwr_rdy = 0; h_chrdet(11, irq_data);
	CHECK(M->bc12_wait_work.work.pending); devres_release_all(); CHECK(!phy_bc11); /* pending wait work cancelled */
	setup(1, 1, 0); CHECK(phy_bc11); CHECK(usbchgen()); detect(MT6360_CHG_TYPE_CDP); CHECK(aicr() == 1500000); CHECK(!phy_bc11); vbus(0); clean_idle(__func__); }
static void t_probe_fail(void) {
	setup(0, 1, 0); devres_release_all(); memset(&hw, 0, sizeof(hw)); hw.reg[0x3E7] = 0x10; fail_regulator = 1;
	CHECK(probe() == -EPROBE_DEFER); devres_release_all(); CHECK(!phy_bc11); CHECK(!usbchgen()); fail_regulator = 0;
	fail_reg = 0x361; CHECK(probe() == -EIO); devres_release_all(); fail_reg = -1; CHECK(!phy_bc11); CHECK(phy_calls == 0); }
static void t_no_phy(void) { if (ndr) devres_release_all(); memset(&hw, 0, sizeof(hw)); phy_present = 0; phy_calls = 0; hw.reg[0x3E7] = 0x10; assert(probe() == 0);
	CHECK(!usbchgen()); CHECK(aicr() == 500000); CHECK(phy_calls == 0); devres_release_all(); phy_present = 1; }
/* real SSUSB role work + probe default PHY mode driving the shared DP/DM handoff */
static struct mtu3 u3d; static struct ssusb_mtk ss; static struct phy *ss_phys[3]; static struct phy phy2; static struct device ssdev;
static int fail_ops(struct phy *p, enum phy_mode m, int s) { (void)p; (void)m; (void)s; return -EIO; }
static const struct phy_ops phy2_ops = { .set_mode = fail_ops };
static void ss_init(bool host) { memset(ippc_mmio, 0, sizeof(ippc_mmio)); memset(mac_mmio, 0, sizeof(mac_mmio)); ss = (struct ssusb_mtk){ .dev = &ssdev, .u3d = &u3d, .mac_base = mac_mmio, .ippc_base = ippc_mmio, .phys = ss_phys, .num_phys = 1, .is_host = host };
	ss_phys[0] = &uphy; INIT_WORK(&ss.otg_switch.dr_work, ssusb_mode_sw_work); u3d.started = !host; vbus_on = 0; pm_ref = 0; }
static void role(enum usb_role r) { ss.otg_switch.desired_role = r; ss.otg_switch.dr_work.func(&ss.otg_switch.dr_work); CHECK(pm_ref == 0); }
#define U2CTL ippc_mmio[SSUSB_U2_CTRL(0) / 4]
#define DTM1 tphy_mmio[U3P_U2PHYDTM1 / 4]
static void t_role_probe_default(void) {
	/* PHY initialised but no role set yet: charger must not take DP/DM */
	step_mode = 1; setup(1, 1, 0); step_mode = 0; uphy.attrs.mode = PHY_MODE_INVALID;
	M->chrdet_work.pending = 0; M->chrdet_work.func(&M->chrdet_work);
	for (int i = 0; i < 3; i++) { CHECK(M->bc12_wait_work.work.pending); M->bc12_wait_work.work.pending = 0; M->bc12_wait_work.work.func(&M->bc12_wait_work.work); }
	CHECK(!phy_bc11); CHECK(!usbchgen()); CHECK(phy_calls == 0);
	ss_init(false); CHECK(ssusb_phy_set_mode(&ss, PHY_MODE_USB_DEVICE) == 0); /* what mtu3_probe does */
	CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(phy_iddig() == PHY_MODE_USB_DEVICE);
	run_works(); CHECK(phy_bc11); CHECK(usbchgen()); detect(MT6360_CHG_TYPE_CDP); CHECK(aicr() == 1500000); vbus(0); clean_idle(__func__); }
static void t_role_unknown(void) {
	/* OTG/unknown PHY role: ownership not proven, never detect */
	setup(0, 1, 0); uphy.attrs.mode = PHY_MODE_USB_OTG; vbus(1); CHECK(!phy_bc11); CHECK(!usbchgen()); CHECK(phy_calls == 0); CHECK(aicr() == 500000); vbus(0); clean_idle(__func__); }
static void t_role_work_preempt(void) {
	setup(0, 1, 0); ss_init(false); vbus(1); CHECK(phy_bc11); CHECK(usbchgen());
	role(USB_ROLE_HOST); CHECK(ss.is_host); CHECK(!u3d.started); CHECK(vbus_on == 1); CHECK(!phy_bc11); CHECK(clk_on == 0);
	CHECK(uphy.attrs.mode == PHY_MODE_USB_HOST); CHECK(phy_iddig() == PHY_MODE_USB_HOST);
	CHECK((U2CTL & SSUSB_U2_PORT_HOST_SEL) == SSUSB_U2_PORT_HOST_SEL); CHECK(U2CTL & SSUSB_U2_PORT_FORCE_IDDIG); CHECK(!(U2CTL & SSUSB_U2_PORT_RG_IDDIG));
	detect(MT6360_CHG_TYPE_SDP); CHECK(not_published()); CHECK(!usbchgen()); CHECK(!M->bc11_sw); CHECK(!phy_bc11);
	vbus(0); clean_idle(__func__); vbus(1); CHECK(!phy_bc11); CHECK(not_published()); vbus(0);
	role(USB_ROLE_DEVICE); CHECK(!ss.is_host); CHECK(u3d.started); CHECK(vbus_on == 0); CHECK(phy_iddig() == PHY_MODE_USB_DEVICE); CHECK(!(U2CTL & SSUSB_U2_PORT_HOST));
	vbus(1); CHECK(phy_bc11); detect(MT6360_CHG_TYPE_CDP); CHECK(aicr() == 1500000); CHECK(!phy_bc11); vbus(0); clean_idle(__func__); }
static void t_role_work_clk_fail(void) {
	setup(0, 1, 0); ss_init(false); vbus(1); CHECK(phy_bc11); u32 u2 = U2CTL; int e = fake_log_err;
	clk_fail_err = -EIO; role(USB_ROLE_HOST); clk_fail_err = 0;
	CHECK(!ss.is_host); CHECK(u3d.started); CHECK(vbus_on == 0); CHECK(U2CTL == u2); CHECK(fake_log_err == e + 1);
	CHECK(phy_bc11); CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(clk_on == 0);
	role(USB_ROLE_HOST); CHECK(ss.is_host); CHECK(!phy_bc11); CHECK(vbus_on == 1);
	detect(MT6360_CHG_TYPE_DCP); CHECK(not_published()); vbus(0); clean_idle(__func__); }
static void t_role_multi_phy_unwind(void) {
	setup(0, 1, 0); ss_init(false); phy2 = (struct phy){ .ops = &phy2_ops }; phy2.attrs.mode = PHY_MODE_USB_DEVICE; mutex_init(&phy2.mutex);
	ss_phys[1] = &phy2; ss.num_phys = 2; u32 u2 = U2CTL;
	role(USB_ROLE_HOST); CHECK(!ss.is_host); CHECK(u3d.started); CHECK(U2CTL == u2); CHECK(vbus_on == 0);
	CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(phy_iddig() == PHY_MODE_USB_DEVICE); CHECK(!uphy.mutex.locked && !phy2.mutex.locked);
	ss.num_phys = 1; clean_idle(__func__); }
static struct phy phy3; static int phy3_calls; static int ok_ops(struct phy *p, enum phy_mode m, int s) { (void)p; (void)m; (void)s; phy3_calls++; return 0; }
static const struct phy_ops phy3_ops = { .set_mode = ok_ops };
static void t_role_mixed_unwind(void) {
	/* PHYs in different prior modes: refused before any PHY write, role, port or VBUS change */
	setup(0, 1, 0); ss_init(false); phy3 = (struct phy){ .ops = &phy3_ops }; phy3.attrs.mode = PHY_MODE_USB_OTG; mutex_init(&phy3.mutex);
	phy2 = (struct phy){ .ops = &phy2_ops }; phy2.attrs.mode = PHY_MODE_INVALID; mutex_init(&phy2.mutex);
	ss_phys[0] = &uphy; ss_phys[1] = &phy3; ss_phys[2] = &phy2; ss.num_phys = 3; phy3_calls = 0;
	u32 u2 = U2CTL, dtm = DTM1; int e = fake_log_err;
	CHECK(ssusb_phy_set_mode(&ss, PHY_MODE_USB_HOST) == -EINVAL); CHECK(fake_log_err == e + 1);
	CHECK(DTM1 == dtm); CHECK(phy3_calls == 0);
	CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(phy3.attrs.mode == PHY_MODE_USB_OTG); CHECK(phy2.attrs.mode == PHY_MODE_INVALID);
	role(USB_ROLE_HOST); CHECK(!ss.is_host); CHECK(u3d.started); CHECK(U2CTL == u2); CHECK(vbus_on == 0); CHECK(DTM1 == dtm); CHECK(phy3_calls == 0);
	CHECK(uphy.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(phy3.attrs.mode == PHY_MODE_USB_OTG); CHECK(phy2.attrs.mode == PHY_MODE_INVALID);
	CHECK(!uphy.mutex.locked && !phy3.mutex.locked && !phy2.mutex.locked);
	ss_phys[1] = &phy3; ss.num_phys = 2; phy3.attrs.mode = PHY_MODE_USB_DEVICE; /* common mode, second PHY accepts */
	role(USB_ROLE_HOST); CHECK(ss.is_host); CHECK(phy3_calls == 1); CHECK(phy3.attrs.mode == PHY_MODE_USB_HOST); CHECK(vbus_on == 1);
	role(USB_ROLE_DEVICE); CHECK(!ss.is_host); CHECK(phy3.attrs.mode == PHY_MODE_USB_DEVICE); CHECK(phy_iddig() == PHY_MODE_USB_DEVICE);
	ss.num_phys = 1; clean_idle(__func__); }
static void t_roles(void) { t_role_mixed_unwind(); t_role_probe_default(); t_role_unknown(); t_role_work_preempt(); t_role_work_clk_fail(); t_role_multi_phy_unwind(); }
int main(void) {	t_unattached(); t_types(); t_dcdtout(); t_ready_never(); t_ready_late_earlyboot(); t_unplug_in_settle(); t_unplug_in_wait();
	t_unplug_before_attach_irq(); t_replug_preempt(); t_phy_take_fail(); t_phy_release_fail(); t_regmaps();
	t_attach_unknown(MT6360_CHG_TYPE_DISABLE_BC12); t_attach_unknown(7); t_attach_unknown(MT6360_CHG_TYPE_UNDER_GOING);
	t_teardown(); t_probe_fail(); t_no_phy(); t_roles(); devres_release_all();
	printf("%s: %d failures\n", fails ? "FAILED" : "PASSED", fails); return !!fails; }