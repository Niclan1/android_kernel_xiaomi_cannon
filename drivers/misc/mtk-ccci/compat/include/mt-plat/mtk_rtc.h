/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <mt-plat/mtk_rtc.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_RTC_H
#define __MTK_CCCI_RTC_H
#include <linux/rtc.h>
#define rtc_time_to_tm(t, tm)	rtc_time64_to_tm((time64_t)(t), tm)
bool crystal_exist_status(void);
#endif
