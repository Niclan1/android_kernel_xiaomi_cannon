/* environment for the real mtu3_dr.c / mtu3_plat.c role code in mtu3_real.c */
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
static inline const char *usb_role_string(enum usb_role r) { return r == USB_ROLE_HOST ? "host" : "device"; }
#define mtu3_dbg_trace(d, f, ...) do { } while (0)
#define phy_set_mode(p, m) phy_set_mode_ext(p, m, 0)
struct regulator;
struct mtu3 { int started; };
struct otg_switch_mtk { struct regulator *vbus; struct work_struct dr_work; enum usb_role desired_role, default_role; bool is_u3_drd; };
struct ssusb_mtk { struct device *dev; struct mtu3 *u3d; void *mac_base, *ippc_base; struct phy **phys; int num_phys; bool is_host; struct otg_switch_mtk otg_switch; };
static u32 ippc_mmio[0x40], mac_mmio[0x2500 / 4]; static int vbus_on, pm_ref;
static inline void pm_runtime_get_sync(struct device *d) { (void)d; pm_ref++; }
static inline void pm_runtime_put(struct device *d) { (void)d; pm_ref--; }
static void mtu3_stop(struct mtu3 *m) { m->started = 0; }
static void mtu3_start(struct mtu3 *m) { m->started = 1; }
static int ssusb_check_clocks(struct ssusb_mtk *s, u32 c) { (void)s; (void)c; return 0; }
static int ssusb_set_vbus(struct otg_switch_mtk *o, int on) { (void)o; vbus_on = on; return 0; }
#include "mtu3_real.c"