/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DIAG ONLY: raw snapshots of the display pipeline registers, printed with
 * the DIAG-DISP prefix. Every block is read only while its clock gate is
 * open, because reading a gated block hangs the bus and resets the SoC.
 * Not for release.
 */
#ifndef __MTK_DIAG_H
#define __MTK_DIAG_H

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/types.h>

#define MTK_DIAG_TX	BIT(0)	/* the mipi-tx PHY is powered */

extern unsigned long mtk_diag_state;

static inline void mtk_diag_range(void __iomem *base, const char *tag,
				  const char *name, u32 from, u32 to)
{
	u32 o;

	for (o = from; o < to; o += 16)
		pr_debug("DIAG-DISP %s %s+%03x: %08x %08x %08x %08x\n", tag, name, o,
			readl_relaxed(base + o), readl_relaxed(base + o + 4),
			readl_relaxed(base + o + 8), readl_relaxed(base + o + 12));
}

static inline void mtk_diag_block(unsigned long pa, const char *tag,
				  const char *name, u32 from, u32 to)
{
	void __iomem *base = ioremap(pa, 0x1000);

	if (!base)
		return;
	mtk_diag_range(base, tag, name, from, to);
	iounmap(base);
}

/* PLL/bandgap/top registers plus the five per-lane control blocks */
static inline void mtk_diag_miptx(void __iomem *base, const char *tag)
{
	u32 lane;

	mtk_diag_range(base, tag, "miptx", 0x00, 0x70);
	for (lane = 0x100; lane < 0x600; lane += 0x100)
		mtk_diag_range(base, tag, "miptx", lane, lane + 0x80);
}

static inline void mtk_diag_snap_regs(const char *tag)
{
	void __iomem *mm = ioremap(0x14000000, 0x1000);
	void __iomem *b;
	u32 cg0, cg1;

	if (!mm)
		return;

	/* clock gate status: a set bit means the block is gated off */
	cg0 = readl_relaxed(mm + 0x100);
	cg1 = readl_relaxed(mm + 0x1a0);
	pr_debug("DIAG-DISP %s state=%lx CG0=%08x CG1=%08x\n", tag,
		mtk_diag_state, cg0, cg1);
	mtk_diag_range(mm, tag, "mmsys", 0xf00, 0xf60);
	iounmap(mm);

	if (!(cg0 & BIT(0)))
		mtk_diag_block(0x14001000, tag, "mutex", 0x00, 0x80);

	if (!(cg0 & BIT(2))) {
		b = ioremap(0x14005000, 0x1000);
		if (b) {
			mtk_diag_range(b, tag, "ovl0", 0x00, 0x70);
			mtk_diag_range(b, tag, "ovl0", 0x1d0, 0x1f0);
			mtk_diag_range(b, tag, "ovl0", 0x240, 0x2b0);
			mtk_diag_range(b, tag, "ovl0", 0xf40, 0xf50);
			iounmap(b);
		}
	}

	if (!(cg0 & BIT(3))) {
		b = ioremap(0x14007000, 0x1000);
		if (b) {
			mtk_diag_range(b, tag, "rdma0", 0x00, 0x100);
			mtk_diag_range(b, tag, "rdma0", 0xf00, 0xf10);
			iounmap(b);
		}
	}

	if (!(cg0 & BIT(19)) && !(cg1 & BIT(0)))
		mtk_diag_block(0x14013000, tag, "dsi0", 0x00, 0x1a0);

	if (mtk_diag_state & MTK_DIAG_TX) {
		b = ioremap(0x11e50000, 0x1000);
		if (b) {
			mtk_diag_miptx(b, tag);
			iounmap(b);
		}
	}
}


/* Register snapshots only in DEBUG builds of the driver. */
static inline void mtk_diag_snap(const char *tag)
{
#ifdef DEBUG
	mtk_diag_snap_regs(tag);
#endif
}

#endif
