/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * MT6853 power-domain data translated to the ACK 6.18 PM-domain API.
 *
 * Register data is derived from MotorolaMobilityLLC/kernel-kernel_device_modules-6.6,
 * commit 9b049a46b9fc6b08372fb1bb782d0f49dc7c9ef1. The ACK probe maps the SPM
 * register resource directly; device-tree hierarchy and runtime behavior remain
 * subject to hardware validation.
 */
#ifndef __PMDOMAIN_MEDIATEK_MT6853_PM_DOMAINS_H
#define __PMDOMAIN_MEDIATEK_MT6853_PM_DOMAINS_H

#include <dt-bindings/power/mt6853-power.h>
#include "mtk-pm-domains.h"

#define MT6853_TOP_AXI_PROT_EN_MD	(BIT(7))
#define MT6853_TOP_AXI_PROT_EN_VDNR_MD	(BIT(2) | BIT(12) |  \
			BIT(20))
#define MT6853_TOP_AXI_PROT_EN_CONN	(BIT(13) | BIT(18))
#define MT6853_TOP_AXI_PROT_EN_CONN_2ND	(BIT(14))
#define MT6853_TOP_AXI_PROT_EN_1_CONN	(BIT(10))
#define MT6853_TOP_AXI_PROT_EN_1_MFG1	(BIT(21))
#define MT6853_TOP_AXI_PROT_EN_2_MFG1	(BIT(5) | BIT(6))
#define MT6853_TOP_AXI_PROT_EN_MFG1	(BIT(21) | BIT(22))
#define MT6853_TOP_AXI_PROT_EN_2_MFG1_2ND	(BIT(7))
/* INFRA_PDN (infra_bcrm, "mediatek,infracfg-nao") MFG1 slave way enable */
#define MT6853_INFRA_PDN_MFG1_WAY_EN	0x004C
#define MT6853_MFG1_WAY_EN		(BIT(11))
#define MT6853_TOP_AXI_PROT_EN_MM_2_ISP	(BIT(8))
#define MT6853_TOP_AXI_PROT_EN_MM_2_ISP_2ND	(BIT(9))
#define MT6853_TOP_AXI_PROT_EN_MM_IPE	(BIT(16))
#define MT6853_TOP_AXI_PROT_EN_MM_IPE_2ND	(BIT(17))
#define MT6853_TOP_AXI_PROT_EN_MM_VDEC	(BIT(24))
#define MT6853_TOP_AXI_PROT_EN_MM_VDEC_2ND	(BIT(25))
#define MT6853_TOP_AXI_PROT_EN_MM_VENC	(BIT(26))
#define MT6853_TOP_AXI_PROT_EN_MM_VENC_2ND	(BIT(27))
#define MT6853_TOP_AXI_PROT_EN_MM_DISP	(BIT(0) | BIT(2) |  \
			BIT(10) | BIT(12) |  \
			BIT(16) | BIT(24) |  \
			BIT(26))
#define MT6853_TOP_AXI_PROT_EN_MM_2_DISP	(BIT(8))
#define MT6853_TOP_AXI_PROT_EN_DISP	(BIT(6) | BIT(23))
#define MT6853_TOP_AXI_PROT_EN_MM_DISP_2ND	(BIT(1) | BIT(3) |  \
			BIT(17) | BIT(25) |  \
			BIT(27))
#define MT6853_TOP_AXI_PROT_EN_MM_2_DISP_2ND	(BIT(9))
#define MT6853_TOP_AXI_PROT_EN_2_AUDIO	(BIT(4))
#define MT6853_TOP_AXI_PROT_EN_2_ADSP_DORMANT	(BIT(3))
#define MT6853_TOP_AXI_PROT_EN_2_CAM	(BIT(0))
#define MT6853_TOP_AXI_PROT_EN_MM_CAM	(BIT(0) | BIT(2))
#define MT6853_TOP_AXI_PROT_EN_1_CAM	(BIT(22))
#define MT6853_TOP_AXI_PROT_EN_MM_CAM_2ND	(BIT(1) | BIT(3))
#define MT6853_TOP_AXI_PROT_EN_VDNR_CAM	(BIT(19))

enum regmap_type {
	INVALID_TYPE = 0,
	IFR_TYPE,
	SMI_TYPE,
	BUS_TYPE_NUM,
};

static const char *bus_list[BUS_TYPE_NUM] __maybe_unused = {
	[IFR_TYPE] = "infracfg",
	[SMI_TYPE] = "smi_comm",
};
/*
 * MT6853 power domain support
 */

static const struct scpsys_domain_data scpsys_domain_data_mt6853[] = {
	[MT6853_POWER_DOMAIN_MD] = {
		.name = "md",
		.sta_mask = BIT(0),
		.ctl_offs = 0x300,
		.pwr_sta_offs = 0x16C,
		/* MD1 acknowledges power only in PWR_STATUS. */
		.pwr_sta2nd_offs = 0x16C,
		.caps = MTK_SCPD_MODEM_PWRSEQ | MTK_SCPD_EXT_BUCK_ISO,
		.ext_buck_iso_offs = 0x398,
		.ext_buck_iso_mask = 0x3,
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MD, 0x02A0, 0x02A4, 0x0228),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_VDNR_MD, 0x0B84, 0x0B88, 0x0B90),
		},
	},
	[MT6853_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = BIT(1),
		.ctl_offs = 0x304,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_CONN, 0x02A0, 0x02A4, 0x0228),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_CONN_2ND, 0x02A0, 0x02A4, 0x0228),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_1_CONN, 0x02A8, 0x02AC, 0x0258),
		},
	},
	[MT6853_POWER_DOMAIN_MFG0] = {
		.name = "mfg0",
		.sta_mask = BIT(2),
		.ctl_offs = 0x308,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_MFG1] = {
		.name = "mfg1",
		.sta_mask = BIT(3),
		.ctl_offs = 0x30C,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			/*
			 * 4.14 STEP0: way_en=0 before bus protect on power-off,
			 * way_en=1 on power-on (here after the bus protect release).
			 */
			_BUS_PROT_STA(INFRA_NAO, INFRA_NAO, MT6853_MFG1_WAY_EN,
				      MT6853_INFRA_PDN_MFG1_WAY_EN,
				      MT6853_INFRA_PDN_MFG1_WAY_EN,
				      MT6853_MFG1_WAY_EN,
				      MT6853_INFRA_PDN_MFG1_WAY_EN,
				      BUS_PROT_INVERTED | BUS_PROT_REG_UPDATE |
				      BUS_PROT_IGNORE_CLR_ACK),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_1_MFG1, 0x02A8, 0x02AC, 0x0258),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_2_MFG1, 0x0714, 0x0718, 0x0724),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MFG1, 0x02A0, 0x02A4, 0x0228),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_2_MFG1_2ND, 0x0714, 0x0718, 0x0724),
		},
	},
	[MT6853_POWER_DOMAIN_MFG2] = {
		.name = "mfg2",
		.sta_mask = BIT(4),
		.ctl_offs = 0x310,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_MFG3] = {
		.name = "mfg3",
		.sta_mask = BIT(5),
		.ctl_offs = 0x314,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_MFG5] = {
		.name = "mfg5",
		.sta_mask = BIT(7),
		.ctl_offs = 0x31C,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(12),
		.ctl_offs = 0x330,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_2_ISP, 0x0DCC, 0x0DD0, 0x0DD8),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_2_ISP_2ND, 0x0DCC, 0x0DD0, 0x0DD8),
		},
	},
	[MT6853_POWER_DOMAIN_ISP2] = {
		.name = "isp2",
		.sta_mask = BIT(13),
		.ctl_offs = 0x334,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_IPE] = {
		.name = "ipe",
		.sta_mask = BIT(14),
		.ctl_offs = 0x338,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_IPE, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_IPE_2ND, 0x02D4, 0x02D8, 0x02EC),
		},
	},
	[MT6853_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(15),
		.ctl_offs = 0x33C,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_VDEC, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_VDEC_2ND, 0x02D4, 0x02D8, 0x02EC),
		},
	},
	[MT6853_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = BIT(17),
		.ctl_offs = 0x344,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_VENC, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_VENC_2ND, 0x02D4, 0x02D8, 0x02EC),
		},
	},
	[MT6853_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = BIT(20),
		.ctl_offs = 0x350,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_DISP, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_2_DISP, 0x0DCC, 0x0DD0, 0x0DD8),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_DISP, 0x02A0, 0x02A4, 0x0228),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_DISP_2ND, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_2_DISP_2ND, 0x0DCC, 0x0DD0, 0x0DD8),
		},
	},
	[MT6853_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(21),
		.ctl_offs = 0x354,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_2_AUDIO, 0x0714, 0x0718, 0x0724),
		},
	},
	[MT6853_POWER_DOMAIN_ADSP_DORMANT] = {
		.name = "adsp_dormant",
		.sta_mask = BIT(22),
		.ctl_offs = 0x358,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_slp_bits = GENMASK(9, 9),
		.sram_slp_ack_bits = GENMASK(13, 13),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_2_ADSP_DORMANT, 0x0714, 0x0718, 0x0724),
		},
		.caps = MTK_SCPD_SRAM_ISO | MTK_SCPD_SRAM_SLP,
	},
	[MT6853_POWER_DOMAIN_CAM] = {
		.name = "cam",
		.sta_mask = BIT(23),
		.ctl_offs = 0x35C,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_2_CAM, 0x0714, 0x0718, 0x0724),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_CAM, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_1_CAM, 0x02A8, 0x02AC, 0x0258),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_MM_CAM_2ND, 0x02D4, 0x02D8, 0x02EC),
			BUS_PROT_WR_IGN(INFRA, MT6853_TOP_AXI_PROT_EN_VDNR_CAM, 0x0B84, 0x0B88, 0x0B90),
		},
	},
	[MT6853_POWER_DOMAIN_CAM_RAWA] = {
		.name = "cam_rawa",
		.sta_mask = BIT(24),
		.ctl_offs = 0x360,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_CAM_RAWB] = {
		.name = "cam_rawb",
		.sta_mask = BIT(25),
		.ctl_offs = 0x0364,
		.pwr_sta_offs = 0x16C,
		.pwr_sta2nd_offs = 0x170,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6853_POWER_DOMAIN_APU] = {
		.name = "apu",
		.caps = 0, /* APU callback is not represented by the ACK framework */
	},
};



static enum scpsys_bus_prot_block __maybe_unused
mt6853_bus_prot_blocks[] = {
	BUS_PROT_BLOCK_INFRA,
};

static const struct scpsys_soc_data mt6853_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6853,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6853),
	.bus_prot_blocks = mt6853_bus_prot_blocks,
	.num_bus_prot_blocks = ARRAY_SIZE(mt6853_bus_prot_blocks),
};

#endif
