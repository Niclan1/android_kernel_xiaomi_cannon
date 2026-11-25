// SPDX-License-Identifier: GPL-2.0
#include <linux/moduleparam.h>
#include <linux/module.h>

#include "mtk_cam.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "mtk_cam."

static bool hw;
module_param(hw, bool, 0444);
MODULE_PARM_DESC(hw, "Enable the legacy MediaTek camera drivers (default 0)");

bool mtk_cam_hw_enabled(void)
{
	return hw;
}
EXPORT_SYMBOL_GPL(mtk_cam_hw_enabled);

static bool isp;
module_param(isp, bool, 0444);
MODULE_PARM_DESC(isp, "Enable the ISP P1 driver /dev/camera-isp (needs hw=1, default 0)");

bool mtk_cam_isp_enabled(void)
{
	return hw && isp;
}
EXPORT_SYMBOL_GPL(mtk_cam_isp_enabled);

static bool dip;
module_param(dip, bool, 0444);
MODULE_PARM_DESC(dip, "Enable the ISP P2 driver /dev/camera-dip (needs hw=1, default 0)");

bool mtk_cam_dip_enabled(void)
{
	return hw && dip;
}
EXPORT_SYMBOL_GPL(mtk_cam_dip_enabled);

static bool mdp;
module_param(mdp, bool, 0444);
MODULE_PARM_DESC(mdp, "Enable the MDP/GCE compat driver /dev/mtk_mdp (needs hw=1, default 0)");

bool mtk_cam_mdp_enabled(void)
{
	return hw && mdp;
}
EXPORT_SYMBOL_GPL(mtk_cam_mdp_enabled);

static bool flash;
module_param(flash, bool, 0444);
MODULE_PARM_DESC(flash, "Enable the flashlight compat driver /dev/flashlight (needs hw=1, default 0)");

bool mtk_cam_flash_enabled(void)
{
	return hw && flash;
}
EXPORT_SYMBOL_GPL(mtk_cam_flash_enabled);
