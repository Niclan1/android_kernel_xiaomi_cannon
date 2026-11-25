#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual SCPSYS functions against MT6853's single MD1 ACK."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DRIVERS = ROOT / "drivers/pmdomain/mediatek"


def block(text, start):
    begin = text.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


source = (DRIVERS / "mtk-pm-domains.c").read_text()
functions = []
for name in ("scpsys_domain_is_on", "scpsys_modem_pwrseq_on",
             "scpsys_modem_pwrseq_off", "scpsys_power_on", "scpsys_power_off"):
    match = re.search(r"static (?:bool|int|void) " + name + r"\(", source)
    if not match:
        raise RuntimeError("Missing source function: " + name)
    functions.append(block(source, match.start()))
data = (DRIVERS / "mt6853-pm-domains.h").read_text()
md = block(data, data.index("{", data.index("[MT6853_POWER_DOMAIN_MD]")))
caps = "\n".join(line for line in (DRIVERS / "mtk-pm-domains.h").read_text().splitlines()
                 if re.match(r"#define MTK_SCPD_\w+\s+BIT\(", line))
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1U << (n))
typedef uint32_t u32;
#define PWR_RST_B_BIT BIT(0)
#define PWR_ON_BIT BIT(2)
#define MTK_POLL_DELAY_US 1
#define MTK_POLL_TIMEOUT 4
#define BUS_PROT_WR_IGN(...) 0
#define MTK_SCPD_CAPS(pd, cap) ((pd)->data->caps & (cap))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
struct scpsys_domain_data {
    const char *name;
    u32 sta_mask, ctl_offs, pwr_sta_offs, pwr_sta2nd_offs, caps;
    u32 ext_buck_iso_offs, ext_buck_iso_mask;
    int bp_cfg[8];
};
struct generic_pm_domain { int unused; };
struct scpsys { void *base; };
struct scpsys_domain {
    struct generic_pm_domain genpd;
    struct scpsys *scpsys;
    const struct scpsys_domain_data *data;
    void *supply, *clks, *subsys_clks;
    int num_clks, num_subsys_clks;
};
static u32 regs[0x400 / 4];
static bool fail_on, hold_ack;
static int releases;
static int regmap_read(void *base, u32 offset, u32 *value)
{
    *value = regs[offset / 4];
    return 0;
}
static void regmap_set_bits(void *base, u32 offset, u32 mask)
{
    if (offset == 0x398)
        assert(!(regs[0x16c / 4] & BIT(0)));
    regs[offset / 4] |= mask;
    if (offset == 0x300 && (mask & PWR_ON_BIT) && !fail_on &&
        !(regs[0x398 / 4] & 3) && (regs[0x300 / 4] & PWR_RST_B_BIT))
        regs[0x16c / 4] |= BIT(0);
}
static void regmap_clear_bits(void *base, u32 offset, u32 mask)
{
    if (offset == 0x300 && (mask & PWR_RST_B_BIT))
        assert(!(regs[0x16c / 4] & BIT(0)) && (regs[0x398 / 4] & 3) == 3);
    regs[offset / 4] &= ~mask;
    if (offset == 0x300 && (mask & PWR_ON_BIT) && !hold_ack)
        regs[0x16c / 4] &= ~BIT(0);
}
#define readx_poll_timeout(fn, pd, value, condition, delay, timeout) \
    ({ int rc = -110; for (int i = 0; i < 4; ++i) { \
        value = fn(pd); if (condition) { rc = 0; break; } } rc; })
static int scpsys_regulator_enable(void *supply) { return 0; }
static void scpsys_regulator_disable(void *supply) {}
static int clk_bulk_prepare_enable(int count, void *clks) { return 0; }
static void clk_bulk_disable_unprepare(int count, void *clks) {}
static int scpsys_ctl_pwrseq_on(struct scpsys_domain *pd) { return 0; }
static void scpsys_ctl_pwrseq_off(struct scpsys_domain *pd) {}
static int scpsys_sram_enable(struct scpsys_domain *pd) { return 0; }
static int scpsys_sram_disable(struct scpsys_domain *pd) { return 0; }
static int scpsys_bus_protect_enable(struct scpsys_domain *pd) { return 0; }
static int scpsys_bus_protect_disable(struct scpsys_domain *pd)
{
    ++releases;
    return 0;
}
"""
tests = r"""
static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    regs[0x398 / 4] = 3;
    fail_on = hold_ack = false;
    releases = 0;
}
int main(void)
{
    struct scpsys scpsys = {0};
    struct scpsys_domain pd = {.scpsys = &scpsys, .data = &md};
    reset();
    assert(scpsys_power_on(&pd.genpd) == 0);
    assert(regs[0x16c / 4] == 1 && regs[0x170 / 4] == 0 && releases == 1);
    assert(scpsys_power_off(&pd.genpd) == 0);
    assert(regs[0x398 / 4] == 3 && regs[0x300 / 4] == 0);
    reset();
    regs[0x398 / 4] = 0;
    regs[0x300 / 4] = PWR_RST_B_BIT | PWR_ON_BIT;
    regs[0x16c / 4] = 1;
    assert(scpsys_power_on(&pd.genpd) == 0);
    reset();
    fail_on = true;
    assert(scpsys_power_on(&pd.genpd) == -110 && releases == 0);
    reset();
    assert(scpsys_power_on(&pd.genpd) == 0);
    hold_ack = true;
    assert(scpsys_power_off(&pd.genpd) == -110);
    assert(regs[0x398 / 4] == 0 && (regs[0x300 / 4] & PWR_RST_B_BIT));
    struct scpsys_domain_data normal = md;
    normal.pwr_sta2nd_offs = 0x170;
    normal.caps = 0;
    pd.data = &normal;
    assert(!scpsys_domain_is_on(&pd));
    regs[0x170 / 4] = 1;
    assert(scpsys_domain_is_on(&pd));
    puts("PASS: isolated recovery startup, pre-powered startup, ACK timeouts, ordered shutdown, dual-ACK domains");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="scpsys-md-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(prefix + caps + "\nstatic const struct scpsys_domain_data md = "
                               + md + ";\n" + "\n".join(functions) + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
