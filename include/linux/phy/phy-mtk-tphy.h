/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PHY_MTK_TPHY_H__
#define __PHY_MTK_TPHY_H__

/*
 * Submodes of PHY_MODE_USB_DEVICE understood by the MediaTek T-PHY. They hand
 * DP/DM of a USB2 port to an external BC1.2 detector (RG_USB20_BC11_SW_EN)
 * and back, as the vendor Charger_Detect_Init/Release did; the IDDIG role
 * forcing of the port is left untouched. They may be passed with the current
 * PHY mode only: a stale mode gives -EBUSY, as does taking DP/DM from a host.
 * Switching the port to host mode returns DP/DM to the controller.
 */
enum mtk_tphy_usb_submode {
	MTK_TPHY_USB_SUBMODE_NONE = 0,
	MTK_TPHY_USB_SUBMODE_BC11_ON,
	MTK_TPHY_USB_SUBMODE_BC11_OFF,
};

#endif
