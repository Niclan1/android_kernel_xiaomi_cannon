#!/usr/bin/env bash
# Builds the MT6360 BC1.2 / USB role harness from the real sources of this
# tree with ASan/UBSan and runs it. CC selects the compiler (default clang),
# B the build directory (default: a new mktemp directory, left in place).
set -euo pipefail
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
K=$(cd "$T/../../.." && pwd)
CC=${CC:-clang}
B=${B:-$(mktemp -d)}
mkdir -p "$B/inc/linux/phy" "$B/inc/linux/regulator"
for h in devm-helpers init interrupt kernel linear_range module of platform_device power_supply property regmap phy/phy regulator/driver workqueue; do
	: > "$B/inc/linux/$h.h"
done
cp "$K/include/linux/phy/phy-mtk-tphy.h" "$B/inc/linux/phy/"
cp "$T"/*.c "$T"/*.h "$B/"
cp "$K/drivers/power/supply/mt6360_charger.c" "$B/"
sed -i '1i #include "linux/phy/phy-mtk-tphy.h"' "$B/kstub.h"
P=$K/drivers/phy/mediatek/phy-mtk-tphy.c
{ grep -E '^#define (U3P_USBPHYACR6|PA6_RG_U2_BC11_SW_EN|TPHY_CLKS_CNT)\b' "$P"
  awk '/^static inline void mtk_phy_(set|clear)_bits\(/,/^}/' "$K/drivers/phy/mediatek/phy-mtk-io.h"
  awk '/^static int u2_phy_instance_set_bc11/,/^}/' "$P"
  grep -E '^#define (U3P_U2PHYDTM1|P2C_FORCE_IDDIG|P2C_RG_IDDIG)\b' "$P"
  awk '/^static void u2_phy_instance_set_mode\(/,/^}/' "$P"
  awk '/^static int mtk_phy_set_mode\(/,/^}/' "$P"
  awk '/^int phy_set_mode_ext/,/^}/' "$K/drivers/phy/phy-core.c"; } > "$B/tphy_real.c"
U=$K/drivers/usb/mtu3
{ grep -E '^#define (SSUSB_SIFSLV_IPPC_BASE|SSUSB_USB2_CSR_BASE|U3D_SSUSB_U2_CTRL_0P|U3D_SSUSB_U3_CTRL_0P|U3D_POWER_MANAGEMENT|U3D_DEVICE_CONTROL|SOFT_CONN|DC_SESSION|SSUSB_U2_PORT_[A-Z_]+|SSUSB_U3_PORT_(HOST_SEL|PDN|DIS)|SSUSB_U3_MAC_RST_B_STS)\b' "$U/mtu3_hw_regs.h"
  grep -E '^#define (SSUSB_U2_CTRL|SSUSB_U3_CTRL)\(' "$U/mtu3.h"
  grep -E '^#define USB[23]_PORT\b' "$U/mtu3_dr.c"
  awk '/^enum mtu3_dr_force_mode/,/^};/' "$U/mtu3.h"
  awk '/^static inline (u32|void) mtu3_(readl|writel|setbits|clrbits)\(/,/^}/' "$U/mtu3.h"
  echo 'void ssusb_set_force_mode(struct ssusb_mtk *, enum mtu3_dr_force_mode); int ssusb_phy_set_mode(struct ssusb_mtk *, enum phy_mode);'
  awk '/^static inline struct ssusb_mtk \*otg_sx_to_ssusb|^static void (toggle_opstate|switch_port_to_host|switch_port_to_device|ssusb_mode_sw_work)\(|^static int ssusb_port0_switch\(|^void ssusb_set_force_mode\(/,/^}/' "$U/mtu3_dr.c"
  awk '/^int ssusb_phy_set_mode\(/,/^}/' "$U/mtu3_plat.c"; } > "$B/mtu3_real.c"
if ! grep -q 'ssusb_phy_set_mode(ssusb, ssusb->is_host' "$U/mtu3_plat.c"; then
	echo "mtu3 probe does not set the PHY default role" >&2
	exit 1
fi
sha256sum "$P" "$K/drivers/phy/phy-core.c" "$U/mtu3_dr.c" "$U/mtu3_plat.c" "$K/drivers/power/supply/mt6360_charger.c"
cd "$B"
rm -f bc12test
"$CC" -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -I inc -include kstub.h test.c -o bc12test
ASAN_OPTIONS=detect_leaks=1 ./bc12test