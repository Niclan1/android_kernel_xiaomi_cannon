#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise MT6360 source power, attach interrupts and parent I2C supply."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "drivers/usb/typec/tcpm/tcpci_mt6360.c"


class VbusTest(unittest.TestCase):
    def test_attach_interrupts_survive_initialization_and_reset(self):
        source = SOURCE.read_text()
        functions = []
        for name in ("mt6360_tcpc_write16", "mt6360_tcpc_init"):
            body = re.search(
                rf"^static (?:inline )?int {name}\(.*?^\}}",
                source,
                re.MULTILINE | re.DOTALL,
            )
            self.assertIsNotNone(body, f"{name} was not found")
            functions.append(body[0])
        header = (ROOT / "include/linux/usb/tcpci.h").read_text()
        defines = re.findall(r"^#define MT6360[^\n]+", source, re.MULTILINE)
        defines += re.findall(r"^#define TCPC_ALERT[^\n]+", header, re.MULTILINE)
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

typedef uint16_t u16;
#define BIT(n) (1U << (n))
DEFINES
struct regmap {
	unsigned char registers[256];
	bool fail_unmask;
	int fail_register;
};
struct tcpci {};
struct tcpci_data { struct regmap *regmap; };
static void usleep_range(int min, int max) {}
static int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	if ((int)reg == map->fail_register)
		return -EIO;
	if (reg == MT6360_REG_SWRESET)
		memset(map->registers, 0, sizeof(map->registers));
	map->registers[reg] = val;
	return 0;
}
static int regmap_raw_write(struct regmap *map, unsigned int reg,
			    const void *value, size_t size)
{
	u16 val;

	assert(size == sizeof(val));
	memcpy(&val, value, size);
	if (reg == TCPC_ALERT_MASK && val && map->fail_unmask)
		return -EIO;
	map->registers[reg] = val;
	map->registers[reg + 1] = val >> 8;
	return 0;
}
static int regmap_update_bits(struct regmap *map, unsigned int reg,
			      unsigned int mask, unsigned int val)
{
	return regmap_write(map, reg, (map->registers[reg] & ~mask) | val);
}
FUNCTIONS
static bool cable_event_reaches_irq(struct regmap *map, u16 event)
{
	u16 mask = map->registers[TCPC_ALERT_MASK] |
		   map->registers[TCPC_ALERT_MASK + 1] << 8;

	return !!(mask & event);
}
int main(void)
{
	struct regmap map = { .fail_register = -1 };
	struct tcpci_data data = { .regmap = &map };
	int i;

	/* Both cold boot and TCPM recovery reset must deliver cable changes. */
	for (i = 0; i < 2; i++) {
		assert(mt6360_tcpc_init(NULL, &data) == 0);
		assert(cable_event_reaches_irq(&map, TCPC_ALERT_CC_STATUS));
		assert(cable_event_reaches_irq(&map, TCPC_ALERT_POWER_STATUS));
	}

	/* A port with masked interrupts must not report successful setup. */
	map.fail_unmask = true;
	assert(mt6360_tcpc_init(NULL, &data) == -EIO);
	assert(!cable_event_reaches_irq(&map, TCPC_ALERT_CC_STATUS));
	map.fail_unmask = false;
	map.fail_register = MT6360_REG_MODECTRL2;
	assert(mt6360_tcpc_init(NULL, &data) == -EIO);
	assert(!cable_event_reaches_irq(&map, TCPC_ALERT_CC_STATUS));
	return 0;
}
"""
        self.run_harness(
            harness.replace("DEFINES", "\n".join(defines)).replace(
                "FUNCTIONS", "\n".join(functions)
            )
        )

    def test_role_changes_and_regulator_failures(self):
        body = re.search(
            r"^static int mt6360_tcpc_set_vbus\(.*?^\}",
            SOURCE.read_text(),
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(body, "source VBUS must be owned by TCPM")
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

struct tcpci {};
struct tcpci_data {};
struct regulator {
	int enables;
	int disables;
	int result;
};
struct mt6360_tcpc_info {
	struct tcpci_data tdata;
	struct regulator *vbus;
	bool vbus_on;
};
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
static int regulator_enable(struct regulator *r)
{
	r->enables++;
	return r->result;
}
static int regulator_disable(struct regulator *r)
{
	r->disables++;
	return r->result;
}
CALLBACK
int main(void)
{
	struct regulator r = {};
	struct mt6360_tcpc_info m = { .vbus = &r };

	/* A sink and repeated role notifications must not change source power. */
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, false, true) == 0);
	assert(!r.enables && !r.disables && !m.vbus_on);
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, true, false) == 0);
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, true, false) == 0);
	assert(r.enables == 1 && !r.disables && m.vbus_on);
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, false, true) == 0);
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, false, true) == 0);
	assert(r.disables == 1 && !m.vbus_on);

	/* Failed enables must be reported and remain retryable. */
	r.result = -EIO;
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, true, false) == -EIO);
	assert(!m.vbus_on);
	r.result = 0;
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, true, false) == 0);
	assert(r.enables == 3 && m.vbus_on);

	/* Failed disables must not falsely report that VBUS is off. */
	r.result = -EIO;
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, false, true) == -EIO);
	assert(m.vbus_on);
	r.result = 0;
	assert(mt6360_tcpc_set_vbus(NULL, &m.tdata, false, true) == 0);
	assert(r.disables == 3 && !m.vbus_on);
	return 0;
}
"""
        self.run_harness(harness.replace("CALLBACK", body[0]))

    def test_i2c_supply_must_not_come_from_its_child(self):
        source = ROOT / "drivers/i2c/busses/i2c-mt65xx.c"
        body = re.search(
            r"i2c->adap.retries = 1;\n(.*?)\n\s*ret = mtk_i2c_parse_dt",
            source.read_text(),
            re.DOTALL,
        )
        self.assertIsNotNone(body, "I2C probe supply lookup was not found")
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#define EPROBE_DEFER 517
struct regulator {};
struct device_node { bool own_supply; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; };
struct i2c_adapter { struct regulator *bus_regulator; };
struct mtk_i2c { struct i2c_adapter adap; };
static struct regulator rail, error;
static int lookup_count, lookup_error;
#define IS_ERR(p) ((p) == &error)
#define PTR_ERR(p) (lookup_error)
static inline bool of_property_present(struct device_node *node, const char *name)
{
	return node->own_supply;
}
static struct regulator *devm_regulator_get_optional(struct device *dev,
						    const char *name)
{
	lookup_count++;
	return lookup_error ? &error : &rail;
}
static inline int dev_err_probe(struct device *dev, int err, const char *fmt)
{
	return err;
}
static int probe_supply(struct platform_device *pdev, struct mtk_i2c *i2c)
{
SUPPLY_LOOKUP
	return 0;
}
int main(void)
{
	struct device_node node = {};
	struct platform_device pdev = { .dev.of_node = &node };
	struct mtk_i2c i2c = {};

	/* A child boost cannot exist until its parent adapter has registered. */
	lookup_error = -EPROBE_DEFER;
	assert(probe_supply(&pdev, &i2c) == 0);
	assert(!lookup_count && !i2c.adap.bus_regulator);

	node.own_supply = true;
	assert(probe_supply(&pdev, &i2c) == -EPROBE_DEFER);
	assert(lookup_count == 1);
	lookup_error = -ENODEV;
	assert(probe_supply(&pdev, &i2c) == -ENODEV);
	assert(lookup_count == 2);
	lookup_error = 0;
	assert(probe_supply(&pdev, &i2c) == 0);
	assert(lookup_count == 3 && i2c.adap.bus_regulator == &rail);
	return 0;
}
"""
        self.run_harness(harness.replace("SUPPLY_LOOKUP", body[1]))

    def run_harness(self, harness):
        with tempfile.TemporaryDirectory(prefix="mt6360-vbus-test-") as tmp:
            src = Path(tmp) / "test.c"
            exe = Path(tmp) / "test"
            src.write_text(harness)
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-o",
                    str(exe),
                    str(src),
                ],
                check=True,
            )
            subprocess.run([str(exe)], check=True, cwd=tmp)


if __name__ == "__main__":
    unittest.main()
