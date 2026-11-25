/* SPDX-License-Identifier: GPL-2.0 */
/* Generated from the 4.14 cmdq_subsys_common.c. */
#ifndef __MDP_SUBSYS_TABLE_H__
#define __MDP_SUBSYS_TABLE_H__

static const struct mdp_subsys_name {
	const char *group;
	const char *name;
} mdp_subsys_names[CMDQ_SUBSYS_MAX_COUNT] = {
	[CMDQ_SUBSYS_G3D_CONFIG_BASE] = { "MFG", "g3d_config_base" },
	[CMDQ_SUBSYS_MMSYS_CONFIG] = { "MMSYS", "mmsys_config_base" },
	[CMDQ_SUBSYS_DISP_DITHER] = { "MMSYS", "disp_dither_base" },
	[CMDQ_SUBSYS_NA] = { "MMSYS", "mm_na_base" },
	[CMDQ_SUBSYS_IMGSYS] = { "CAM", "imgsys_base" },
	[CMDQ_SUBSYS_VDEC_GCON] = { "VDEC", "vdec_gcon_base" },
	[CMDQ_SUBSYS_VENC_GCON] = { "VENC", "venc_gcon_base" },
	[CMDQ_SUBSYS_CONN_PERIPHERALS] = { "PERISYS", "conn_peri_base" },
	[CMDQ_SUBSYS_TOPCKGEN] = { "TOP_AO_3", "topckgen_base" },
	[CMDQ_SUBSYS_KP] = { "INFRA_AO", "kp_base" },
	[CMDQ_SUBSYS_SCP_SRAM] = { "INFRA_AO", "scp_sram_base" },
	[CMDQ_SUBSYS_INFRA_NA3] = { "NA", "infra_na3_base" },
	[CMDQ_SUBSYS_INFRA_NA4] = { "NA", "infra_na4_base" },
	[CMDQ_SUBSYS_SCP] = { "SCP", "scp_base" },
	[CMDQ_SUBSYS_MCUCFG] = { "INFRASYS", "mcucfg_base" },
	[CMDQ_SUBSYS_GCPU] = { "INFRASYS", "gcpu_base" },
	[CMDQ_SUBSYS_USB0] = { "PERISYS", "usb0_base" },
	[CMDQ_SUBSYS_USB_SIF] = { "PERISYS", "usb_sif_base" },
	[CMDQ_SUBSYS_AUDIO] = { "PERISYS", "audio_base" },
	[CMDQ_SUBSYS_MSDC0] = { "PERISYS", "msdc0_base" },
	[CMDQ_SUBSYS_MSDC1] = { "PERISYS", "msdc1_base" },
	[CMDQ_SUBSYS_MSDC2] = { "PERISYS", "msdc2_base" },
	[CMDQ_SUBSYS_MSDC3] = { "PERISYS", "msdc3_base" },
	[CMDQ_SUBSYS_AP_DMA] = { "INFRASYS", "ap_dma_base" },
	[CMDQ_SUBSYS_GCE] = { "GCE", "gce_base" },
	[CMDQ_SUBSYS_VDEC] = { "VDEC", "vdec_base" },
	[CMDQ_SUBSYS_VDEC1] = { "VDEC", "vdec1_base" },
	[CMDQ_SUBSYS_VDEC2] = { "VDEC", "vdec2_base" },
	[CMDQ_SUBSYS_VDEC3] = { "VDEC", "vdec3_base" },
	[CMDQ_SUBSYS_CAMSYS] = { "CAMSYS", "camsys_base" },
	[CMDQ_SUBSYS_CAMSYS1] = { "CAMSYS", "camsys1_base" },
	[CMDQ_SUBSYS_CAMSYS2] = { "CAMSYS", "camsys2_base" },
	[CMDQ_SUBSYS_CAMSYS3] = { "CAMSYS", "camsys3_base" },
	[CMDQ_SUBSYS_IMGSYS1] = { "IMGSYS", "imgsys1_base" },
	[CMDQ_SUBSYS_SMI_LAB1] = { "SMI_LAB1", "smi_larb1_base" },
	[CMDQ_SUBSYS_PWM_SW] = { "SPECIAL", "pwm_sw_base" },
	[CMDQ_SUBSYS_PWM1_SW] = { "SPECIAL", "pwm1_sw_base" },
	[CMDQ_SUBSYS_DIP_A0_SW] = { "SPECIAL", "dip_a0_sw_base" },
	[CMDQ_SUBSYS_MIPITX0] = { "SPECIAL", "mipitx0_base" },
	[CMDQ_SUBSYS_MIPITX1] = { "SPECIAL", "mipitx1_base" },
	[CMDQ_SUBSYS_VENC] = { "SPECIAL", "venc_base" },
	[CMDQ_SUBSYS_IMGSYS2] = { "SPECIAL", "imgsys2_base" },
	[CMDQ_SUBSYS_IPESYS] = { "SPECIAL", "ipesys_base" },
	[CMDQ_SUBSYS_DPE] = { "SPECIAL", "dpe_base" },
};

#endif
