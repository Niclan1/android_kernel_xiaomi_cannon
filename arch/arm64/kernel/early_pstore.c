// SPDX-License-Identifier: GPL-2.0
/*
 * Mirror printk into the ramoops console zone from the start of setup_arch()
 * until ramoops probes, so a hang before the postcore initcalls still leaves
 * a log for the next boot. The zone is written in the persistent_ram_buffer
 * layout and ramoops continues appending to it instead of zapping it.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/pstore_ram.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/early_ioremap.h>
#include <asm/setup.h>

#define EP_SIG		0x43474244	/* PERSISTENT_RAM_SIG, console type */

/*
 * MediaTek DBGTOP: with DDR reserve mode set, a watchdog reset keeps DRAM in
 * self-refresh. Without it the bootloader sees a full PMIC reset, DRAM is
 * lost and only a stale ramoops snapshot from expdb is restored.
 */
#define DBGTOP_MODE		0x00
#define DBGTOP_MODE_KEY		0x22000000
#define DBGTOP_MODE_DDR_RESERVE	BIT(0)
#define DBGTOP_DEBUG_CTL2	0x34
#define DBGTOP_DEBUG_CTL2_KEY	0x55000000
#define DBGTOP_DVFSRC_EN	BIT(9)
#define DBGTOP_LATCH_CTL	0x40
#define DBGTOP_LATCH_CTL_KEY	0x95000000
#define DBGTOP_DVFSRC_LATCH_EN	BIT(13)
#define EP_HDR_SIZE	12

static void __iomem *ep_buf;
static u32 ep_map_len, ep_cap, ep_start, ep_size;
static bool ep_active;

static void ep_write(struct console *con, const char *s, unsigned int n)
{
	if (!ep_active)
		return;

	while (n--) {
		writeb_relaxed(*s++, ep_buf + EP_HDR_SIZE + ep_start);
		if (++ep_start == ep_cap)
			ep_start = 0;
		if (ep_size < ep_cap)
			ep_size++;
	}
	writel(ep_start, ep_buf + 4);
	writel(ep_size, ep_buf + 8);
}

static struct console ep_console = {
	.name	= "pstore_early",
	.write	= ep_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

static bool __init ep_param(const char *cmd, const char *key,
			    unsigned long long *val)
{
	const char *p = strstr(cmd, key);

	if (!p)
		return false;
	*val = memparse(p + strlen(key), NULL);
	return true;
}

static void __init ep_ddr_reserve(const void *fdt, bool dvfsrc)
{
	const fdt32_t *reg;
	void __iomem *base;
	u64 addr;
	u32 mode, ctl2, latch;
	int node, len;

	node = fdt_node_offset_by_compatible(fdt, -1, "mediatek,dbgtop");
	if (node < 0)
		return;
	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len < 8)
		return;
	addr = ((u64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);

	base = early_ioremap(addr, PAGE_SIZE);
	if (!base)
		return;
	/*
	 * The DVFSRC handshake needs a running DVFSRC/SSPM setup; without it
	 * the preloader reports "DVFSRC fail" and drops the reserved DRAM.
	 */
	ctl2 = readl(base + DBGTOP_DEBUG_CTL2) & 0xffffff & ~DBGTOP_DVFSRC_EN;
	writel(ctl2 | (dvfsrc ? DBGTOP_DVFSRC_EN : 0) | DBGTOP_DEBUG_CTL2_KEY,
	       base + DBGTOP_DEBUG_CTL2);
	latch = readl(base + DBGTOP_LATCH_CTL) & 0xffffff & ~DBGTOP_DVFSRC_LATCH_EN;
	writel(latch | (dvfsrc ? DBGTOP_DVFSRC_LATCH_EN : 0) | DBGTOP_LATCH_CTL_KEY,
	       base + DBGTOP_LATCH_CTL);
	mode = readl(base + DBGTOP_MODE) & 0xffffff;
	writel(mode | DBGTOP_MODE_DDR_RESERVE | DBGTOP_MODE_KEY,
	       base + DBGTOP_MODE);
	mode = readl(base + DBGTOP_MODE);
	early_iounmap(base, PAGE_SIZE);
	pr_info("early-pstore: dbgtop %#llx DDR reserve mode %#x ctl2 %#x latch %#x\n",
		addr, mode, ctl2, latch);
}

/* ramoops module parameter default for omitted zone sizes (MIN_MEM_SIZE). */
#define EP_DEFAULT_ZONE	SZ_4K

static unsigned long long __init ep_pow2(unsigned long long v)
{
	return v && !is_power_of_2(v) ? rounddown_pow_of_two(v) : v;
}

void __init arm64_early_pstore_init(const void *fdt)
{
	unsigned long long addr, size, con, ftrace = EP_DEFAULT_ZONE;
	unsigned long long pmsg = EP_DEFAULT_ZONE, rec = EP_DEFAULT_ZONE;
	unsigned long long dump, cnt;
	const char *cmd;
	int chosen;

	if (!fdt)
		return;
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return;
	cmd = fdt_getprop(fdt, chosen, "bootargs", NULL);
	if (!cmd || strstr(cmd, "early_pstore=off"))
		return;

	if (!ep_param(cmd, "ramoops.mem_address=", &addr) ||
	    !ep_param(cmd, "ramoops.mem_size=", &size) ||
	    !ep_param(cmd, "ramoops.console_size=", &con))
		return;
	ep_param(cmd, "ramoops.ftrace_size=", &ftrace);
	ep_param(cmd, "ramoops.pmsg_size=", &pmsg);
	ep_param(cmd, "ramoops.record_size=", &rec);

	/*
	 * Mirror ramoops_probe(): sizes are rounded down to powers of two and
	 * the dmesg area is split into equal zones before the console zone.
	 * Getting this wrong makes the early console scribble over a dmesg
	 * zone header after ramoops validated it.
	 */
	con = ep_pow2(con);
	ftrace = ep_pow2(ftrace);
	pmsg = ep_pow2(pmsg);
	rec = ep_pow2(rec);
	if (con <= EP_HDR_SIZE || con + ftrace + pmsg > size)
		return;
	dump = size - con - ftrace - pmsg;
	if (dump && rec) {
		cnt = div64_u64(dump, rec);
		if (!cnt)
			return;
		addr += cnt * ALIGN_DOWN(div64_u64(dump, cnt), 2);
	}
	if (!PAGE_ALIGNED(addr))
		return;

	ep_map_len = min_t(unsigned long long, con, SZ_256K);
	ep_buf = early_ioremap(addr, ep_map_len);
	if (!ep_buf)
		return;

	if (!strstr(cmd, "early_pstore.ddr_reserve=off"))
		ep_ddr_reserve(fdt, strstr(cmd, "early_pstore.ddr_reserve=dvfsrc"));

	ep_cap = ep_map_len - EP_HDR_SIZE;
	writel(0, ep_buf + 4);
	writel(0, ep_buf + 8);
	writel(EP_SIG, ep_buf);
	ep_active = true;
	register_console(&ep_console);
	pr_info("early-pstore: console zone at %#llx\n", addr);
}

bool early_pstore_console_handover(void)
{
	bool was_active = ep_active;

	ep_active = false;
	return was_active;
}

static int __init ep_release(void)
{
	if (!ep_buf || ep_active)
		return 0;
	unregister_console(&ep_console);
	early_iounmap(ep_buf, ep_map_len);
	ep_buf = NULL;
	return 0;
}
device_initcall(ep_release);
