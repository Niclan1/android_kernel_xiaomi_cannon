/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Author: Balsam CHIHI <bchihi@baylibre.com>
 */

#ifndef __MEDIATEK_LVTS_DT_H
#define __MEDIATEK_LVTS_DT_H

#define MT7988_CPU_0		0
#define MT7988_CPU_1		1
#define MT7988_ETH2P5G_0	2
#define MT7988_ETH2P5G_1	3
#define MT7988_TOPS_0		4
#define MT7988_TOPS_1		5
#define MT7988_ETHWARP_0	6
#define MT7988_ETHWARP_1	7

#define MT8186_LITTLE_CPU0	0
#define MT8186_LITTLE_CPU1	1
#define MT8186_LITTLE_CPU2	2
#define MT8186_CAM		3
#define MT8186_BIG_CPU0	4
#define MT8186_BIG_CPU1	5
#define MT8186_NNA		6
#define MT8186_ADSP		7
#define MT8186_GPU		8

#define MT8188_MCU_LITTLE_CPU0	0
#define MT8188_MCU_LITTLE_CPU1	1
#define MT8188_MCU_LITTLE_CPU2	2
#define MT8188_MCU_LITTLE_CPU3	3
#define MT8188_MCU_BIG_CPU0	4
#define MT8188_MCU_BIG_CPU1	5

#define MT8188_AP_APU		0
#define MT8188_AP_GPU0		1
#define MT8188_AP_GPU1		2
#define MT8188_AP_ADSP		3
#define MT8188_AP_VDO		4
#define MT8188_AP_INFRA		5
#define MT8188_AP_CAM1		6
#define MT8188_AP_CAM2		7

#define MT8195_MCU_BIG_CPU0     0
#define MT8195_MCU_BIG_CPU1     1
#define MT8195_MCU_BIG_CPU2     2
#define MT8195_MCU_BIG_CPU3     3
#define MT8195_MCU_LITTLE_CPU0  4
#define MT8195_MCU_LITTLE_CPU1  5
#define MT8195_MCU_LITTLE_CPU2  6
#define MT8195_MCU_LITTLE_CPU3  7

#define MT8195_AP_VPU0  8
#define MT8195_AP_VPU1  9
#define MT8195_AP_GPU0  10
#define MT8195_AP_GPU1  11
#define MT8195_AP_VDEC  12
#define MT8195_AP_IMG   13
#define MT8195_AP_INFRA 14
#define MT8195_AP_CAM0  15
#define MT8195_AP_CAM1  16

#define MT8192_MCU_BIG_CPU0     0
#define MT8192_MCU_BIG_CPU1     1
#define MT8192_MCU_BIG_CPU2     2
#define MT8192_MCU_BIG_CPU3     3
#define MT8192_MCU_LITTLE_CPU0  4
#define MT8192_MCU_LITTLE_CPU1  5
#define MT8192_MCU_LITTLE_CPU2  6
#define MT8192_MCU_LITTLE_CPU3  7

#define MT8192_AP_VPU0  8
#define MT8192_AP_VPU1  9
#define MT8192_AP_GPU0  10
#define MT8192_AP_GPU1  11
#define MT8192_AP_INFRA 12
#define MT8192_AP_CAM   13
#define MT8192_AP_MD0   14
#define MT8192_AP_MD1   15
#define MT8192_AP_MD2   16

/*
 * MT6853: the ids follow the order of the vendor sensor enumeration
 * (LVTS1-0 .. LVTS5-2). LL is the Cortex-A55 cluster, L the Cortex-A76 one.
 */
#define MT6853_MCU_LL_CPU0	0	/* LVTS1-0 */
#define MT6853_MCU_LL_CPU1	1	/* LVTS1-1 */
#define MT6853_MCU_LL_CPU2	2	/* LVTS1-2 */
#define MT6853_MCU_LL_CPU3	3	/* LVTS1-3 */
#define MT6853_MCU_L_CPU0	4	/* LVTS2-0 */
#define MT6853_MCU_L_CPU1	5	/* LVTS2-1 */

#define MT6853_AP_GPU		6	/* LVTS3-0 */
#define MT6853_AP_CAM		7	/* LVTS3-1 */
#define MT6853_AP_SOC0		8	/* LVTS3-2 */
#define MT6853_AP_SOC1		9	/* LVTS3-3 */
#define MT6853_AP_VPU		10	/* LVTS4-0 */
#define MT6853_AP_MD0		11	/* LVTS5-0, MD 4G */
#define MT6853_AP_MD1		12	/* LVTS5-1, MD 5G */
#define MT6853_AP_MD2		13	/* LVTS5-2, MD 3G */

#endif /* __MEDIATEK_LVTS_DT_H */
