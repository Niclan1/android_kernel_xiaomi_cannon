/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_CAM_H__
#define __MTK_CAM_H__

#include <linux/types.h>

/*
 * Kill switch for the legacy MediaTek camera stack. With mtk_cam.hw=0
 * (the default) no driver is registered and no camera hardware is touched.
 */
bool mtk_cam_hw_enabled(void);

/*
 * Second switch for the ISP P1 (camsys) driver: mtk_cam.isp=1 together with
 * mtk_cam.hw=1. Off by default; without it /dev/camera-isp does not exist
 * and the HAL fails to open the pipeline instead of touching camsys.
 */
bool mtk_cam_isp_enabled(void);

/*
 * Further switches, each also needs mtk_cam.hw=1 and is off by default:
 *   mtk_cam.dip=1    ISP P2 /dev/camera-dip
 *   mtk_cam.mdp=1    MDP/GCE compat /dev/mtk_mdp, /dev/mtk_mdp_limit
 *   mtk_cam.flash=1  flashlight compat /dev/flashlight (MT6360 LED class)
 */
bool mtk_cam_dip_enabled(void);
bool mtk_cam_mdp_enabled(void);
bool mtk_cam_flash_enabled(void);

/* DIP clock/power reference for GCE jobs touching imgsys (-ENODEV if absent) */
int mtk_cam_dip_hold(void);
void mtk_cam_dip_release(void);

#endif
