// SPDX-License-Identifier: GPL-2.0
/*
 * cpufreq driver for the MediaTek MT6853 hybrid DVFS firmware (MCUPM).
 *
 * The MCUPM firmware owns the CPU PLLs, the cluster clock dividers and the
 * Vproc/Vsram rails. The AP seeds the voltage/frequency tables in the CSRAM,
 * starts the firmware with two IPIs and from then on only writes the wanted
 * OPP index of each cluster into a CSRAM word. The CCI clock is derived by the
 * firmware from both cluster indexes through the CCI map.
 *
 * The protocol follows the vendor 4.14 hybrid cpufreq driver
 * (cpufreq_v1/src/mtk_cpufreq_hybrid.c) and was verified on hardware.
 *
 * All 16 stock OPPs are normal operating points, including 2.0 GHz on LL
 * and 2.4 GHz on L. Optional ll_cap/l_cap boot parameters can put the faster
 * OPPs behind the boost switch for bring-up; neither cluster is capped by
 * default. Thermal cooling still constrains each policy through frequency QoS.
 *
 * Anything that fails before the firmware has been started leaves the
 * hardware exactly as the bootloader configured it.
 */

#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/energy_model.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/time64.h>
#include <linux/unaligned.h>

#define MT6853_NR_OPP		16
#define MT6853_NR_DOMAINS	2	/* LL (Cortex-A55) and L (Cortex-A76) */
#define MT6853_NR_PVT		3	/* LL, L and CCI */
#define MT6853_OPP_SET		4	/* the 2.4 GHz ("B24G") OPP set */

/* CSRAM layout, byte offsets from the CSRAM base */
#define CSRAM_GUARD		0x000
#define CSRAM_PVT		0x010
#define CSRAM_PVT_END		0x250
#define CSRAM_DATA		0x2a0
#define CSRAM_SCHED_DIS		0x2bc
#define CSRAM_PPM_LIMIT		0x320
#define CSRAM_OPP_CUR		0x354
#define CSRAM_OPP_REQ		0x37c
#define CSRAM_CCI_MAP		0xfa0
#define CSRAM_CLUSTER_ONOFF	0x1224
#define CSRAM_SIZE		0x1400

#define CSRAM_GUARD_A		0x55aa55aa
#define CSRAM_GUARD_B		0xaa55aa55
#define CLUSTER_ON		0x43434f4e
#define CLUSTER_OFF		0x43434f46
#define PVT_TERMINATOR		0xffffffff
#define PVT_BLOCK_WORDS		36
#define PVT_NR_BLOCKS		4
#define PVT_TERMINATOR_WORD	32
#define PPM_BASE_OPP		0xf

/* IPI mailboxes: one slot bank per channel */
#define MCUPM_IPI_PLAT		0
#define MCUPM_IPI_CPUDVFS	1
#define MCUPM_IPI_SLOT_SIZE	0xa0
#define MCUPM_IPI_SLOTS_SIZE	((MCUPM_IPI_CPUDVFS + 1) * MCUPM_IPI_SLOT_SIZE)
#define MCUPM_IPI_SEND_STATUS	0x0
#define MCUPM_IPI_SEND_SET	0x4
#define MCUPM_IPI_RECV_CLR	0x0
#define MCUPM_IPI_RECV_STATUS	0x4
#define MCUPM_IPI_REGS_SIZE	8
#define MCUPM_IPI_TIMEOUT_US	200000

#define MCUPM_PING		0xdead
#define MCUPM_CMD_INIT_PTBL	0
#define MCUPM_CMD_INIT		1

#define MCUPM_TRANSITION_LATENCY_NS	(100 * NSEC_PER_USEC)
#define MCUPM_TRANSITION_DELAY_US	2000

/* OPP indexes the bootloader leaves the clusters at */
static const unsigned int mcupm_boot_opp[MT6853_NR_DOMAINS] = { 9, 15 };

static unsigned int ll_cap;
module_param(ll_cap, uint, 0444);
MODULE_PARM_DESC(ll_cap, "Fastest LL OPP without boost (default 0 = 2.0 GHz ... 15 = 500 MHz)");

static unsigned int l_cap;
module_param(l_cap, uint, 0444);
MODULE_PARM_DESC(l_cap, "Fastest L OPP without boost (default 0 = 2.4 GHz ... 15 = 725 MHz)");

static bool cluster_onoff = true;
module_param(cluster_onoff, bool, 0644);
MODULE_PARM_DESC(cluster_onoff, "Tell the firmware when the last CPU of a cluster goes offline (the ON flag is always restored when it returns)");

static bool ignore_segment;
module_param(ignore_segment, bool, 0444);
MODULE_PARM_DESC(ignore_segment, "Start even if the efuse segment does not select the B24G OPP set");

/* { MHz, Vreg (400 mV + 6.25 mV * n), post divider, clock divider } */
static const u16 mt6853_opp[MT6853_NR_PVT][MT6853_NR_OPP][4] = {
	{ /* LL (Cortex-A55) */
		{ 2000,  96, 1, 1 },
		{ 1916,  92, 1, 1 },
		{ 1812,  87, 1, 1 },
		{ 1750,  84, 1, 1 },
		{ 1645,  79, 2, 1 },
		{ 1500,  72, 2, 1 },
		{ 1393,  68, 2, 1 },
		{ 1287,  64, 2, 1 },
		{ 1128,  58, 2, 1 },
		{ 1048,  55, 2, 1 },
		{  968,  52, 2, 1 },
		{  862,  48, 2, 1 },
		{  756,  44, 4, 1 },
		{  703,  42, 4, 1 },
		{  650,  40, 4, 1 },
		{  500,  40, 4, 1 },
	},
	{ /* L (Cortex-A76) */
		{ 2400,  96, 1, 1 },
		{ 2306,  93, 1, 1 },
		{ 2210,  90, 1, 1 },
		{ 2118,  87, 1, 1 },
		{ 1993,  83, 1, 1 },
		{ 1837,  78, 1, 1 },
		{ 1650,  72, 2, 1 },
		{ 1534,  68, 2, 1 },
		{ 1418,  64, 2, 1 },
		{ 1274,  59, 2, 1 },
		{ 1129,  54, 2, 1 },
		{ 1042,  51, 2, 1 },
		{  985,  49, 2, 1 },
		{  898,  46, 2, 1 },
		{  840,  44, 4, 1 },
		{  725,  40, 4, 1 },
	},
	{ /* CCI */
		{ 1400,  96, 2, 1 },
		{ 1356,  93, 2, 1 },
		{ 1254,  86, 2, 1 },
		{ 1152,  79, 2, 1 },
		{ 1108,  76, 2, 1 },
		{ 1050,  72, 2, 1 },
		{  975,  68, 2, 1 },
		{  900,  64, 2, 1 },
		{  825,  60, 2, 1 },
		{  768,  57, 4, 1 },
		{  675,  52, 4, 1 },
		{  600,  48, 4, 1 },
		{  562,  46, 4, 1 },
		{  525,  44, 4, 1 },
		{  487,  42, 4, 1 },
		{  450,  40, 4, 1 },
	},
};

/* CCI OPP the firmware picks for each pair of LL (row) and L (column) OPPs */
static const u8 mt6853_cci_map[32][MT6853_NR_OPP] = {
	{  3,  3,  4,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6 },
	{  3,  3,  4,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6 },
	{  3,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7 },
	{  3,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7 },
	{  3,  3,  4,  5,  6,  7,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8 },
	{  3,  3,  4,  5,  6,  7,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 10, 10, 10, 10, 10, 10, 10 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 10, 10, 10, 10, 10, 10, 10 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 12, 12, 12, 12, 12 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 13, 13, 13, 13 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 14, 14, 14 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  3,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
	{  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1 },
	{  0,  0,  0,  0,  0,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2 },
	{  0,  0,  0,  0,  0,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2 },
	{  0,  0,  0,  0,  0,  2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3 },
	{  0,  0,  0,  0,  0,  2,  3,  5,  5,  5,  5,  5,  5,  5,  5,  5 },
	{  0,  0,  0,  0,  0,  2,  3,  5,  6,  6,  6,  6,  6,  6,  6,  6 },
	{  0,  0,  0,  0,  0,  2,  3,  5,  6,  7,  7,  7,  7,  7,  7,  7 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 12, 12, 12, 12, 12 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 13, 13, 13, 13 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 14, 14, 14 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 15, 15, 15 },
	{  0,  0,  0,  0,  0,  2,  3,  5, 10, 11, 12, 13, 14, 15, 15, 15 },
};

static const char * const mcupm_supplies[] = {
	"vproc-ll", "vsram-ll", "vproc-l", "vsram-l",
};

struct mcupm_hw {
	void __iomem *csram;
	void __iomem *slots;
	void __iomem *send;
	void __iomem *recv;
};

struct mcupm_domain {
	unsigned int id;
	unsigned int cap;
	unsigned int req_idx;
	unsigned long dpc;
	bool on;
	void __iomem *opp_req;
	void __iomem *opp_cur;
	void __iomem *onoff;
	void __iomem *ppm;
	struct cpufreq_frequency_table table[MT6853_NR_OPP + 1];
};

static struct mcupm_domain mcupm_domains[MT6853_NR_DOMAINS];

static int mcupm_ipi(const struct mcupm_hw *hw, unsigned int ch, u32 w0, u32 w1)
{
	void __iomem *slot = hw->slots + ch * MCUPM_IPI_SLOT_SIZE;
	u32 bit = BIT(ch);
	u32 val;
	int ret;

	ret = readl_poll_timeout(hw->send + MCUPM_IPI_SEND_STATUS, val,
				 !(val & bit), 5, MCUPM_IPI_TIMEOUT_US);
	if (ret)
		return ret;

	/* A leftover acknowledge must not be mistaken for the one we wait for */
	if (readl(hw->recv + MCUPM_IPI_RECV_STATUS) & bit)
		writel(bit, hw->recv + MCUPM_IPI_RECV_CLR);

	writel(w0, slot);
	writel(w1, slot + 4);
	writel(0, slot + 8);
	writel(0, slot + 12);
	writel(bit, hw->send + MCUPM_IPI_SEND_SET);

	ret = readl_poll_timeout(hw->recv + MCUPM_IPI_RECV_STATUS, val,
				 val & bit, 5, MCUPM_IPI_TIMEOUT_US);
	if (ret)
		return ret;

	writel(bit, hw->recv + MCUPM_IPI_RECV_CLR);

	return 0;
}

static unsigned int mcupm_pvt_off(unsigned int block, unsigned int word)
{
	return CSRAM_PVT + (block * PVT_BLOCK_WORDS + word) * sizeof(u32);
}

static u32 mcupm_pvt_vf(unsigned int pvt, unsigned int opp)
{
	const u16 *e = mt6853_opp[pvt][opp];

	return ((e[1] & 0xfff) << 16) | e[0];
}

static u32 mcupm_pvt_div(unsigned int pvt, unsigned int opp)
{
	const u16 *e = mt6853_opp[pvt][opp];

	return ((e[3] & 0xff) << 16) | (e[2] & 0xff);
}

static u32 mcupm_cci_word(unsigned int i)
{
	return get_unaligned_le32((const u8 *)mt6853_cci_map + i * sizeof(u32));
}

static void mcupm_seed_csram(void __iomem *csram)
{
	unsigned int c, i, off;

	writel(CSRAM_GUARD_A, csram + CSRAM_GUARD);
	writel(CSRAM_GUARD_B, csram + CSRAM_GUARD + 4);
	writel(CSRAM_GUARD_A, csram + CSRAM_GUARD + 8);
	writel(CSRAM_GUARD_B, csram + CSRAM_GUARD + 12);

	for (off = CSRAM_DATA; off < CSRAM_SIZE; off += sizeof(u32))
		writel(0, csram + off);
	writel(CSRAM_GUARD_A, csram + CSRAM_DATA);

	/* Allow every OPP until the caps are applied */
	writel(PPM_BASE_OPP, csram + CSRAM_PPM_LIMIT);
	writel(PPM_BASE_OPP, csram + CSRAM_PPM_LIMIT + 4);

	for (off = CSRAM_PVT; off < CSRAM_PVT_END; off += sizeof(u32))
		writel(0, csram + off);

	for (c = 0; c < MT6853_NR_PVT; c++) {
		for (i = 0; i < MT6853_NR_OPP; i++) {
			writel(mcupm_pvt_vf(c, i),
			       csram + mcupm_pvt_off(c, i));
			writel(mcupm_pvt_div(c, i),
			       csram + mcupm_pvt_off(c, MT6853_NR_OPP + i));
		}
	}
	for (c = 0; c < PVT_NR_BLOCKS; c++)
		writel(PVT_TERMINATOR,
		       csram + mcupm_pvt_off(c, PVT_TERMINATOR_WORD));

	for (i = 0; i < sizeof(mt6853_cci_map) / sizeof(u32); i++)
		writel(mcupm_cci_word(i), csram + CSRAM_CCI_MAP + i * sizeof(u32));
}

/* Read back what the firmware is going to parse, before it is told to */
static int mcupm_verify_csram(void __iomem *csram)
{
	unsigned int c, i;

	if (readl(csram + CSRAM_GUARD) != CSRAM_GUARD_A ||
	    readl(csram + CSRAM_GUARD + 4) != CSRAM_GUARD_B)
		return -EIO;

	for (c = 0; c < MT6853_NR_PVT; c++) {
		for (i = 0; i < MT6853_NR_OPP; i++) {
			if (readl(csram + mcupm_pvt_off(c, i)) !=
			    mcupm_pvt_vf(c, i) ||
			    readl(csram + mcupm_pvt_off(c, MT6853_NR_OPP + i)) !=
			    mcupm_pvt_div(c, i))
				return -EIO;
		}
	}

	for (i = 0; i < sizeof(mt6853_cci_map) / sizeof(u32); i++) {
		if (readl(csram + CSRAM_CCI_MAP + i * sizeof(u32)) !=
		    mcupm_cci_word(i))
			return -EIO;
	}

	return 0;
}

static void mcupm_domain_set_limit(struct mcupm_domain *dom, bool boost)
{
	writel(((boost ? 0 : dom->cap) << 16) | PPM_BASE_OPP, dom->ppm);
}

static void mcupm_domain_set_on(struct mcupm_domain *dom, bool on)
{
	writel(on ? CLUSTER_ON : CLUSTER_OFF, dom->onoff);
	dom->on = on;
}

static void mcupm_domain_setup(struct mcupm_domain *dom, unsigned int id,
			       unsigned int cap, void __iomem *csram)
{
	unsigned int i;

	dom->id = id;
	dom->cap = min_t(unsigned int, cap, MT6853_NR_OPP - 1);
	dom->req_idx = max(mcupm_boot_opp[id], dom->cap);
	dom->opp_req = csram + CSRAM_OPP_REQ + id * sizeof(u32);
	dom->opp_cur = csram + CSRAM_OPP_CUR + id * sizeof(u32);
	dom->onoff = csram + CSRAM_CLUSTER_ONOFF + id * sizeof(u32);
	dom->ppm = csram + CSRAM_PPM_LIMIT + id * sizeof(u32);

	for (i = 0; i < MT6853_NR_OPP; i++) {
		dom->table[i].frequency = mt6853_opp[id][i][0] * 1000;
		dom->table[i].flags = i < dom->cap ? CPUFREQ_BOOST_FREQ : 0;
	}
	dom->table[MT6853_NR_OPP].frequency = CPUFREQ_TABLE_END;
}

/*
 * The seed, the OPP request words and the PPM limits must all be in place
 * before INIT, otherwise the clusters jump to the fastest OPP.
 */
static int mcupm_start_firmware(const struct mcupm_hw *hw)
{
	void __iomem *csram = hw->csram;
	unsigned int i;
	int ret;

	ret = mcupm_ipi(hw, MCUPM_IPI_PLAT, MCUPM_PING, 0);
	if (ret)
		return ret;

	mcupm_seed_csram(csram);
	ret = mcupm_verify_csram(csram);
	if (ret)
		return ret;

	for (i = 0; i < MT6853_NR_DOMAINS; i++) {
		struct mcupm_domain *dom = &mcupm_domains[i];

		writel(dom->req_idx, dom->opp_req);
		mcupm_domain_set_limit(dom, false);
	}
	/* The CCI request is derived by the firmware */
	writel(0, csram + CSRAM_OPP_REQ + MT6853_NR_DOMAINS * sizeof(u32));

	ret = mcupm_ipi(hw, MCUPM_IPI_CPUDVFS, MCUPM_CMD_INIT_PTBL, 0);
	if (ret)
		return ret;

	ret = mcupm_ipi(hw, MCUPM_IPI_CPUDVFS, MCUPM_CMD_INIT, MT6853_OPP_SET);
	if (ret)
		return ret;

	writel(1, csram + CSRAM_SCHED_DIS);

	for (i = 0; i < MT6853_NR_DOMAINS; i++)
		mcupm_domain_set_on(&mcupm_domains[i], true);

	return 0;
}

static int mcupm_cpufreq_target_index(struct cpufreq_policy *policy,
				      unsigned int index)
{
	struct mcupm_domain *dom = policy->driver_data;

	WRITE_ONCE(dom->req_idx, index);
	writel_relaxed(index, dom->opp_req);

	return 0;
}

static unsigned int mcupm_cpufreq_fast_switch(struct cpufreq_policy *policy,
					      unsigned int target_freq)
{
	int index = cpufreq_table_find_index_dl(policy, target_freq, false);

	if (index < 0)
		return 0;

	mcupm_cpufreq_target_index(policy, index);

	return policy->freq_table[index].frequency;
}

static unsigned int mcupm_cpufreq_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
	struct mcupm_domain *dom;
	unsigned int idx;

	if (!policy)
		return 0;

	dom = policy->driver_data;
	idx = readl_relaxed(dom->opp_cur);
	if (idx >= MT6853_NR_OPP)
		idx = READ_ONCE(dom->req_idx);

	return dom->table[idx].frequency;
}

/*
 * The firmware clamp goes first in both directions: it is what keeps the
 * clusters inside the cap, the policy limits only follow.
 */
static int mcupm_cpufreq_set_boost(struct cpufreq_policy *policy, int state)
{
	struct mcupm_domain *dom = policy->driver_data;
	int ret;

	mcupm_domain_set_limit(dom, state);

	ret = cpufreq_boost_set_sw(policy, state);
	if (ret) {
		mcupm_domain_set_limit(dom, !state);
		return ret;
	}

	/* The core never lowers cpuinfo.max_freq, which would let the boost range back in */
	if (!state)
		policy->cpuinfo.max_freq = dom->table[dom->cap].frequency;

	return 0;
}

static int mcupm_cpufreq_init(struct cpufreq_policy *policy)
{
	struct of_phandle_args args;
	struct mcupm_domain *dom;
	unsigned int id;
	int ret;

	ret = of_perf_domain_get_sharing_cpumask(policy->cpu,
						 "performance-domains",
						 "#performance-domain-cells",
						 policy->cpus, &args);
	if (ret < 0)
		return ret;

	id = args.args_count ? args.args[0] : MT6853_NR_DOMAINS;
	of_node_put(args.np);
	if (id >= MT6853_NR_DOMAINS)
		return -EINVAL;

	dom = &mcupm_domains[id];
	policy->driver_data = dom;
	policy->freq_table = dom->table;
	policy->cpuinfo.transition_latency = MCUPM_TRANSITION_LATENCY_NS;
	policy->transition_delay_us = MCUPM_TRANSITION_DELAY_US;
	policy->fast_switch_possible = true;
	policy->dvfs_possible_from_any_cpu = true;

	if (!dom->on)
		mcupm_domain_set_on(dom, true);

	return 0;
}

static int mcupm_cpufreq_online(struct cpufreq_policy *policy)
{
	struct mcupm_domain *dom = policy->driver_data;

	/* The ON flag is restored even if cluster_onoff was cleared meanwhile */
	if (!dom->on)
		mcupm_domain_set_on(dom, true);

	/* A policy that was parked keeps its own boost state */
	mcupm_domain_set_limit(dom, policy->boost_enabled &&
				    cpufreq_boost_enabled());

	return 0;
}

static int mcupm_cpufreq_offline(struct cpufreq_policy *policy)
{
	struct mcupm_domain *dom = policy->driver_data;

	/* The core also calls this when a policy fails to come up, CPUs running */
	if (cluster_onoff && dom->on && cpumask_empty(policy->cpus))
		mcupm_domain_set_on(dom, false);

	return 0;
}

static int __maybe_unused
mcupm_cpufreq_get_cpu_power(struct device *cpu_dev, unsigned long *power,
			    unsigned long *freq)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu_dev->id);
	struct mcupm_domain *dom;
	int cap, idx;
	u64 mv;

	if (!policy)
		return -EINVAL;

	dom = policy->driver_data;
	cap = dom->cap;

	/* The table is in descending order: find the lowest OPP not below *freq */
	for (idx = MT6853_NR_OPP - 1; idx >= cap; idx--) {
		if (dom->table[idx].frequency >= *freq)
			break;
	}
	if (idx < cap)
		return -EINVAL;

	mv = 400 + mt6853_opp[dom->id][idx][1] * 25 / 4;
	*freq = dom->table[idx].frequency;
	*power = div_u64(dom->dpc * mv * mv * (*freq / 1000), 1000000);

	return 0;
}

/* Only the OPPs the firmware may select without boost are modelled */
static void mcupm_cpufreq_register_em(struct cpufreq_policy *policy)
{
	struct em_data_callback em_cb = EM_DATA_CB(mcupm_cpufreq_get_cpu_power);
	struct mcupm_domain *dom = policy->driver_data;
	struct device *cpu_dev = get_cpu_device(policy->cpu);
	u32 dpc;
	int ret;

	if (!cpu_dev)
		return;

	if (of_property_read_u32(cpu_dev->of_node, "dynamic-power-coefficient",
				 &dpc) || !dpc) {
		dev_warn(cpu_dev, "no dynamic-power-coefficient, no energy model\n");
		return;
	}
	dom->dpc = dpc;

	ret = em_dev_register_perf_domain(cpu_dev, MT6853_NR_OPP - dom->cap,
					  &em_cb, policy->related_cpus, true);
	if (ret)
		dev_warn(cpu_dev, "energy model registration failed: %d\n", ret);
}

static struct cpufreq_driver mcupm_cpufreq_driver = {
	.name		= "mt6853-mcupm",
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.init		= mcupm_cpufreq_init,
	.online		= mcupm_cpufreq_online,
	.offline	= mcupm_cpufreq_offline,
	.target_index	= mcupm_cpufreq_target_index,
	.fast_switch	= mcupm_cpufreq_fast_switch,
	.get		= mcupm_cpufreq_get,
	.set_boost	= mcupm_cpufreq_set_boost,
	.register_em	= mcupm_cpufreq_register_em,
};

/*
 * The firmware switches these rails behind the regulator core's back and
 * nothing but the cpufreq driver references them. Enabling them keeps the core
 * from ever switching them off as unused. The consumers are never released
 * and the rails never disabled, not even when probing fails further on:
 * that would cut the power of running CPUs.
 */
static int mcupm_hold_rails(struct device *dev)
{
	struct regulator *rails[ARRAY_SIZE(mcupm_supplies)];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		rails[i] = regulator_get_optional(dev, mcupm_supplies[i]);
		if (IS_ERR(rails[i])) {
			ret = dev_err_probe(dev, PTR_ERR(rails[i]),
					    "no %s supply\n", mcupm_supplies[i]);
			while (i--)
				regulator_put(rails[i]);
			return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(rails); i++) {
		ret = regulator_enable(rails[i]);
		if (ret) {
			dev_err(dev, "cannot enable the %s supply: %d\n",
				mcupm_supplies[i], ret);
			for (; i < ARRAY_SIZE(rails); i++)
				regulator_put(rails[i]);
			return ret;
		}
	}

	return 0;
}

static void __iomem *mcupm_map(struct platform_device *pdev, const char *name,
			       resource_size_t min_size)
{
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res || resource_size(res) < min_size) {
		dev_err(&pdev->dev, "missing or short \"%s\" range\n", name);
		return IOMEM_ERR_PTR(-EINVAL);
	}

	/* The ranges overlap the vendor nodes of the DT, so do not claim them */
	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base)
		return IOMEM_ERR_PTR(-ENOMEM);

	return base;
}

/*
 * The OPP/CCI tables here are the B24G set (stock level 3/4). Stock picks the
 * plain MT6853 set for other segments, whose L voltages differ. The bootloader
 * passes the efuse words in /chosen "atag,devinfo" (word count, then words).
 */
static bool mcupm_segment_ok(struct device *dev)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	const __le32 *p;
	u32 n, seg, lv1, lv2;
	int len;

	if (ignore_segment)
		return true;

	p = chosen ? of_get_property(chosen, "atag,devinfo", &len) : NULL;
	if (!p || len < 2 * sizeof(u32)) {
		of_node_put(chosen);
		dev_warn(dev, "no devinfo, assuming the B24G segment\n");
		return true;
	}
	n = le32_to_cpu(p[0]);
	if (n < 63 || len < (n + 1) * sizeof(u32)) {
		of_node_put(chosen);
		dev_warn(dev, "short devinfo (%u words), assuming the B24G segment\n", n);
		return true;
	}
	seg = le32_to_cpu(p[1 + 7]) & 0xff;
	lv1 = le32_to_cpu(p[1 + 52]) & 0xff;
	lv2 = le32_to_cpu(p[1 + 62]) & 0x300;
	of_node_put(chosen);

	if (seg ? !(seg & 3) : (lv1 > 1 || lv2)) {
		dev_info(dev, "efuse segment 0x%x/0x%x/0x%x selects the B24G OPP set\n",
			 seg, lv1, lv2);
		return true;
	}

	dev_err(dev, "segment 0x%x/0x%x/0x%x needs the plain MT6853 OPP set, not supported; cpufreq not started (ignore_segment=1 overrides)\n",
		seg, lv1, lv2);
	return false;
}

static int mcupm_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mcupm_hw hw;
	int ret;

	if (!mcupm_segment_ok(dev))
		return -ENODEV;

	ret = mcupm_hold_rails(dev);
	if (ret)
		return ret;

	hw.csram = mcupm_map(pdev, "csram", CSRAM_SIZE);
	if (IS_ERR(hw.csram))
		return PTR_ERR(hw.csram);

	hw.slots = mcupm_map(pdev, "slots", MCUPM_IPI_SLOTS_SIZE);
	if (IS_ERR(hw.slots))
		return PTR_ERR(hw.slots);

	hw.send = mcupm_map(pdev, "send", MCUPM_IPI_REGS_SIZE);
	if (IS_ERR(hw.send))
		return PTR_ERR(hw.send);

	hw.recv = mcupm_map(pdev, "recv", MCUPM_IPI_REGS_SIZE);
	if (IS_ERR(hw.recv))
		return PTR_ERR(hw.recv);

	mcupm_domain_setup(&mcupm_domains[0], 0, ll_cap, hw.csram);
	mcupm_domain_setup(&mcupm_domains[1], 1, l_cap, hw.csram);

	ret = mcupm_start_firmware(&hw);
	if (ret) {
		dev_err(dev, "MCUPM did not start (%d), cpufreq not registered, CPUs stay as the bootloader left them\n",
			ret);
		return ret;
	}

	ret = cpufreq_register_driver(&mcupm_cpufreq_driver);
	if (ret) {
		dev_err(dev, "cpufreq registration failed: %d\n", ret);
		return ret;
	}

	dev_info(dev, "hybrid DVFS up: LL %u-%u MHz, L %u-%u MHz without boost\n",
		 mt6853_opp[0][MT6853_NR_OPP - 1][0],
		 mt6853_opp[0][mcupm_domains[0].cap][0],
		 mt6853_opp[1][MT6853_NR_OPP - 1][0],
		 mt6853_opp[1][mcupm_domains[1].cap][0]);

	return 0;
}

static const struct of_device_id mcupm_cpufreq_of_match[] = {
	{ .compatible = "mediatek,mt6853-mcupm-cpufreq" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcupm_cpufreq_of_match);

static struct platform_driver mcupm_cpufreq_platform_driver = {
	.probe = mcupm_cpufreq_probe,
	.driver = {
		.name = "mt6853-mcupm-cpufreq",
		.of_match_table = mcupm_cpufreq_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mcupm_cpufreq_platform_driver);

MODULE_DESCRIPTION("MediaTek MT6853 MCUPM hybrid DVFS cpufreq driver");
MODULE_LICENSE("GPL");
