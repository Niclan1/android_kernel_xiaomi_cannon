#!/bin/sh
set -eu

KERNEL=${KERNEL:-/home/builder/worktrees/cannong-android17-6.18-cannong-port}
KOUT=${KOUT:-/home/builder/out/cannong-android17-618-gce-probe}
OUT=${OUT:-/home/builder/out/cannong-android17-618-gce-source-probe}
SRC=$OUT/mtk-cmdq-mailbox.c

mkdir -p "$KOUT" "$OUT"
cp "$KERNEL/drivers/mailbox/mtk-cmdq-mailbox.c" "$SRC"

python3 - "$SRC" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

platform = '''static const struct gce_plat gce_plat_mt6853_probe = {
	.thread_nr = 24,
	.shift = 3,
	.control_by_sw = false,
	.gce_num = 1
};

'''
needle = "static const struct gce_plat gce_plat_mt6779 = {"
if "gce_plat_mt6853_probe" not in text:
    text = text.replace(needle, platform + needle, 1)

needle = '\t{.compatible = "mediatek,mt6779-gce", .data = (void *)&gce_plat_mt6779},\n'
entry = '\t{.compatible = "mediatek,mt6853-gce", .data = (void *)&gce_plat_mt6853_probe},\n'
if entry not in text:
    text = text.replace(needle, needle + entry, 1)

path.write_text(text)
PY

cat > "$OUT/Makefile" <<EOF
obj-m += mtk-cmdq-mailbox.o
EOF

if [ ! -s "$KOUT/.config" ]; then
	make -C "$KERNEL" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 gki_defconfig
fi
"$KERNEL/scripts/config" --file "$KOUT/.config" --disable GENDWARFKSYMS --disable DEBUG_INFO
make -C "$KERNEL" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 olddefconfig
make -C "$KERNEL" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 prepare modules_prepare
make -C "$KERNEL" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	M="$OUT" mtk-cmdq-mailbox.o -j2
sha256sum "$SRC" "$OUT/mtk-cmdq-mailbox.o"
