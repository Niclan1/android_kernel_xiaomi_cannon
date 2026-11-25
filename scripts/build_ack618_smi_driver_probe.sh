#!/bin/sh
set -eu

KERNEL=${KERNEL:-/home/builder/worktrees/cannong-android17-6.18-cannong-port}
KOUT=${KOUT:-/home/builder/out/cannong-android17-618-smi-probe}
OUT=${OUT:-/home/builder/out/cannong-android17-618-smi-driver-probe}
DONOR=${DONOR:-/home/builder/worktrees/cannong-android17-6.18-smi-probe-20261102}
SRC=$OUT/mtk-smi.c

mkdir -p "$OUT"
cp "$KERNEL/drivers/memory/mtk-smi.c" "$SRC"

python3 - "$SRC" "$DONOR/drivers/memory/mtk-smi.c" <<'PY'
from pathlib import Path
import re
import sys

target = Path(sys.argv[1])
donor = Path(sys.argv[2])
text = target.read_text()
donor_lines = donor.read_text().splitlines(True)

def donor_range(start, end):
    return "".join(donor_lines[start - 1:end])

text = re.sub(
    r"#define SMI_LARB_PORT_NR_MAX\s+32\n",
    "#define SMI_LARB_PORT_NR_MAX\t\t32\n"
    "#define SMI_LARB_MISC_NR\t\t3\n"
    "#define MTK_COMMON_NR_MAX\t\t11\n"
    "#define SMI_COMMON_LARB_NR_MAX\t8\n"
    "#define SMI_COMMON_MISC_NR\t7\n"
    "#define SMI_L1ARB0\t\t0x104\n"
    "#define SMI_LARB_FORCE_ULTRA\t0x78\n"
    "#define SMI_PREULTRA_MASK1\t0x244\n",
    text,
    count=1,
)
text = text.replace(
    "\tMTK_SMI_GEN2_SUB_COMM,\t/* gen2 smi sub common */\n",
    "\tMTK_SMI_GEN2_SUB_COMM,\t/* gen2 smi sub common */\n"
    "\tMTK_SMI_GEN3,\t\t/* isolated MT6853 GEN3 probe */\n",
    1,
)
text = text.replace(
    "\tconst u8\t\t\t(*ostd)[SMI_LARB_PORT_NR_MAX];\n",
    "\tconst u8\t\t\t(*ostd)[SMI_LARB_PORT_NR_MAX];\n"
    "\tint\t\t\tport_in_larb_gen2[MTK_LARB_NR_MAX + 1];\n"
    "\tconst u8\t\t\t(*bwl)[SMI_LARB_PORT_NR_MAX];\n"
    "\tconst u8\t\t\t(*cmd_group)[2];\n"
    "\tconst u8\t\t\t(*bw_thrt_en)[2];\n"
    "\tconst struct mtk_smi_reg_pair\t(*misc)[SMI_LARB_MISC_NR];\n",
    1,
)
text = text.replace(
    "\tint\t\t\tport_in_larb_gen2[MTK_LARB_NR_MAX + 1];\n"
    "\tconst u8\t\t\t(*bwl)[SMI_LARB_PORT_NR_MAX];\n",
    "\tint\t\t\tport_in_larb_gen2[MTK_LARB_NR_MAX + 1];\n"
    "\tbool\t\t\thas_bwl;\n"
    "\tbool\t\t\thas_grouping;\n"
    "\tbool\t\t\thas_bw_thrt;\n"
    "\tconst u8\t\t\t(*bwl)[SMI_LARB_PORT_NR_MAX];\n",
    1,
)
text = text.replace(
    "\tconst struct mtk_smi_reg_pair\t*init;\n",
    "\tconst struct mtk_smi_reg_pair\t*init;\n"
    "\tconst u32\t\t\t(*bwl)[SMI_COMMON_LARB_NR_MAX];\n"
    "\tconst struct mtk_smi_reg_pair\t(*misc)[SMI_COMMON_MISC_NR];\n",
    1,
)

larb_runtime = r'''
	if (larb->larb_gen->has_bwl) {
		for (i = 0; i < larb->larb_gen->port_in_larb_gen2[larb->larbid]; i++)
			writel_relaxed(larb->larb_gen->bwl[
				larb->larbid][i],
				larb->base + SMI_LARB_OSTDL_PORTx(i));
		for (i = 0; i < SMI_LARB_MISC_NR; i++)
			writel_relaxed(larb->larb_gen->misc[
				larb->larbid][i].value,
				larb->base + larb->larb_gen->misc[
				larb->larbid][i].offset);
		if (larb->larb_gen->has_grouping)
			for (i = larb->larb_gen->cmd_group[larb->larbid][0];
			     i < larb->larb_gen->cmd_group[larb->larbid][1]; i++)
				writel_relaxed(readl_relaxed(
					larb->base + SMI_LARB_NONSEC_CON(i)) | BIT(1),
					larb->base + SMI_LARB_NONSEC_CON(i));
		if (larb->larb_gen->has_bw_thrt)
			for (i = larb->larb_gen->bw_thrt_en[larb->larbid][0];
			     i < larb->larb_gen->bw_thrt_en[larb->larbid][1]; i++)
				writel_relaxed(readl_relaxed(
					larb->base + SMI_LARB_NONSEC_CON(i)) | BIT(3),
					larb->base + SMI_LARB_NONSEC_CON(i));
	}
'''
needle = "\tif (BIT(larb->larbid) & larb->larb_gen->larb_direct_to_common_mask)\n\t\treturn 0;\n"
if "larb->larb_gen->has_bwl" not in text:
    text = text.replace(needle, needle + larb_runtime, 1)

common_runtime = r'''
	if (common->plat->type == MTK_SMI_GEN3 && common->plat->bwl) {
		writel(common->plat->bus_sel, common->base + SMI_BUS_SEL);
		for (i = 0; i < SMI_COMMON_LARB_NR_MAX; i++)
			writel_relaxed(common->plat->bwl[0][i],
				common->base + SMI_L1ARB0 + i * 4);
		for (i = 0; i < SMI_COMMON_MISC_NR; i++)
			writel_relaxed(common->plat->misc[0][i].value,
				common->base + common->plat->misc[0][i].offset);
		wmb();
		return 0;
	}
'''
needle = "\tif (common->plat->type != MTK_SMI_GEN2)\n\t\treturn 0;\n"
if "common->plat->type == MTK_SMI_GEN3" not in text:
    text = text.replace(needle, common_runtime + "\n" + needle, 1)

larb_tables = (
    donor_range(964, 1019) +
    donor_range(2667, 2694) +
    r'''
static const struct mtk_smi_larb_gen mtk_smi_larb_mt6853_probe = {
	.port_in_larb_gen2 = {4, 5, 5, 0, 12, 0, 0, 13, 0, 29,
		0, 29, 0, 12, 6, 5, 17, 17, 17, 4, 6,},
	.config_port = mtk_smi_larb_config_port_gen2_general,
	.larb_direct_to_common_mask =
		BIT(3) | BIT(5) | BIT(6) | BIT(8) | BIT(10) | BIT(12),
	.has_bwl = true,
	.has_grouping = true,
	.has_bw_thrt = true,
	.bwl = (const u8 (*)[SMI_LARB_PORT_NR_MAX])mtk_smi_larb_mt6853_bwl,
	.cmd_group = (const u8 (*)[2])mtk_smi_larb_mt6853_cmd_group,
	.bw_thrt_en = (const u8 (*)[2])mtk_smi_larb_mt6853_bw_thrt_en,
	.misc = (const struct mtk_smi_reg_pair (*)[SMI_LARB_MISC_NR])
		mtk_smi_larb_mt6853_misc,
};
''')
needle = "static const struct mtk_smi_larb_gen mtk_smi_larb_mt2701 = {"
if "mtk_smi_larb_mt6853_probe" not in text:
    text = text.replace(needle, larb_tables + "\n" + needle, 1)

common_tables = (
    donor_range(4238, 4248) +
    donor_range(4531, 4558) +
    r'''
static const struct mtk_smi_common_plat mtk_smi_common_mt6853_probe = {
	.type = MTK_SMI_GEN3,
	.has_gals = true,
	.bus_sel = F_MMU1_LARB(1) | F_MMU1_LARB(2) | F_MMU1_LARB(4) |
		   F_MMU1_LARB(5) | F_MMU1_LARB(7),
	.bwl = (const u32 (*)[SMI_COMMON_LARB_NR_MAX])
		mtk_smi_common_mt6853_bwl,
	.misc = (const struct mtk_smi_reg_pair (*)[SMI_COMMON_MISC_NR])
		mtk_smi_common_mt6853_misc,
};
''')
needle = "static const struct mtk_smi_common_plat mtk_smi_common_gen1 = {"
if "mtk_smi_common_mt6853_probe" not in text:
    text = text.replace(needle, common_tables + "\n" + needle, 1)

target.write_text(text)
PY

cat > "$OUT/Makefile" <<EOF
obj-m += mtk-smi.o
EOF

make -C "$KERNEL" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	M="$OUT" mtk-smi.o -j2
sha256sum "$SRC" "$OUT/mtk-smi.o"
