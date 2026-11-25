/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mt-plat/mtk_boot_common.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_BOOT_COMMON_H
#define __MTK_CCCI_BOOT_COMMON_H
#include <linux/types.h>

enum boot_mode_t {
	NORMAL_BOOT = 0,
	META_BOOT = 1,
	RECOVERY_BOOT = 2,
	SW_REBOOT = 3,
	FACTORY_BOOT = 4,
	ADVMETA_BOOT = 5,
	ATE_FACTORY_BOOT = 6,
	ALARM_BOOT = 7,
	KERNEL_POWER_OFF_CHARGING_BOOT = 8,
	LOW_POWER_OFF_CHARGING_BOOT = 9,
	DONGLE_BOOT = 10,
	UNKNOWN_BOOT
};

/* META/factory modes are not supported by this port. */
static inline enum boot_mode_t get_boot_mode(void) { return NORMAL_BOOT; }
static inline bool is_meta_mode(void) { return false; }
static inline bool is_advanced_meta_mode(void) { return false; }
#endif
