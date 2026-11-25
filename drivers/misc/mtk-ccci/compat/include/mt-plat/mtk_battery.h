/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mt-plat/mtk_battery.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_BATTERY_H
#define __MTK_CCCI_BATTERY_H
#include <linux/types.h>

/* Battery voltage in mV, see ccci_fsm_ioctl.c */
signed int battery_get_bat_voltage(void);
#endif
