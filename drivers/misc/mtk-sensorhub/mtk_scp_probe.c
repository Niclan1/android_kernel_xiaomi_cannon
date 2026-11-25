// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6853 SCP state probe.
 *
 * The sensor hub firmware runs on the SCP (a RISC-V/CM4 class co-processor)
 * which the bootloader loads and starts. The stock 4.14 kernel talks to it
 * through drivers/misc/mediatek/scp (IPI over mailboxes) and exposes the
 * sensors via hf_manager. None of that exists on 6.18 yet. This driver is
 * the first, read-only step: it reports whether the SCP core is still
 * running after the 6.18 boot and what the firmware published in its
 * region info block, so the IPI/hf_manager port can be planned on facts.
 *
 * Registers are only touched when /sys/.../scp_state is read.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

/* scp_cfgreg_core0 offsets (stock scp/mt6853/scp_reg.h) */
#define SCP_CORE_STATUS		0x70
#define  SCP_CORE_GATED		BIT(0)
#define  SCP_CORE_HALT		BIT(1)
#define SCP_CORE_MON_PC		0x80
#define SCP_CORE_MON_LR		0x84
#define SCP_CORE_MON_SP		0x88

/* region info block at SRAM offset 4 (stock scp/mt6853/scp_helper.h) */
#define SCP_REGION_INFO_OFFSET	0x4
#define SCP_REGION_INFO_WORDS	8

/* reg indexes in the vendor "mediatek,scp" node */
#define SCP_RES_SRAM		0
#define SCP_RES_CORE0		3

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Bind the read-only SCP state probe");

struct scp_probe {
	void __iomem *sram;
	void __iomem *core0;
	resource_size_t sram_size;
};

static const char * const region_names[SCP_REGION_INFO_WORDS] = {
	"ap_loader_start", "ap_loader_size", "ap_firmware_start",
	"ap_firmware_size", "ap_dram_start", "ap_dram_size",
	"ap_dram_backup_start", "struct_size",
};

static ssize_t scp_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct scp_probe *sp = dev_get_drvdata(dev);
	u32 status;
	int len, i;

	status = readl(sp->core0 + SCP_CORE_STATUS);
	len = sysfs_emit(buf, "sram_size=%pa\nstatus=0x%08x%s%s\n",
			 &sp->sram_size, status,
			 status & SCP_CORE_GATED ? " gated" : "",
			 status & SCP_CORE_HALT ? " halt" : "");
	len += sysfs_emit_at(buf, len, "pc=0x%08x lr=0x%08x sp=0x%08x\n",
			     readl(sp->core0 + SCP_CORE_MON_PC),
			     readl(sp->core0 + SCP_CORE_MON_LR),
			     readl(sp->core0 + SCP_CORE_MON_SP));
	for (i = 0; i < SCP_REGION_INFO_WORDS; i++)
		len += sysfs_emit_at(buf, len, "%s=0x%08x\n", region_names[i],
				     readl(sp->sram + SCP_REGION_INFO_OFFSET +
					   4 * i));
	return len;
}
static DEVICE_ATTR_RO(scp_state);

static struct attribute *scp_probe_attrs[] = {
	&dev_attr_scp_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(scp_probe);

static int scp_probe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct scp_probe *sp;

	if (!enable)
		return -ENODEV;

	sp = devm_kzalloc(dev, sizeof(*sp), GFP_KERNEL);
	if (!sp)
		return -ENOMEM;

	/* Plain ioremap: no region request, nothing else owns these yet. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, SCP_RES_SRAM);
	if (!res || resource_size(res) < PAGE_SIZE)
		return -ENODEV;
	sp->sram_size = resource_size(res);
	sp->sram = devm_ioremap(dev, res->start, PAGE_SIZE);

	res = platform_get_resource(pdev, IORESOURCE_MEM, SCP_RES_CORE0);
	if (!res || resource_size(res) < SCP_CORE_MON_SP + 4)
		return -ENODEV;
	sp->core0 = devm_ioremap(dev, res->start, resource_size(res));

	if (!sp->sram || !sp->core0)
		return -ENOMEM;

	platform_set_drvdata(pdev, sp);
	dev_info(dev, "SCP state probe ready (read scp_state)\n");
	return 0;
}

static const struct of_device_id scp_probe_of_match[] = {
	{ .compatible = "mediatek,scp" },
	{ }
};
MODULE_DEVICE_TABLE(of, scp_probe_of_match);

static struct platform_driver scp_probe_driver = {
	.probe = scp_probe_probe,
	.driver = {
		.name = "mtk-scp-probe",
		.of_match_table = scp_probe_of_match,
		.dev_groups = scp_probe_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(scp_probe_driver);

MODULE_DESCRIPTION("MT6853 SCP read-only state probe");
MODULE_LICENSE("GPL");
