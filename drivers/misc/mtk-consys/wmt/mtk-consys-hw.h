/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_CONSYS_HW_H
#define __MTK_CONSYS_HW_H

#include <linux/printk.h>
#include <linux/types.h>

/* mtk_consys.hw: start the vendor core at all (default 0, hardware untouched) */
extern bool mtk_consys_hw;
/* mtk_consys.power: with hw=1, allow the chip to be powered up (default 1) */
extern bool mtk_consys_power;

/* True once the vendor /dev/wmtdetect owns the loader handshake. */
bool mtk_consys_hw_detect_active(void);

/* Bring-up trace: always printed, one line per step, step number first. */
#define MTK_CONSYS_STEP(fmt, ...) pr_info("mtk-consys: " fmt, ##__VA_ARGS__)

#endif
