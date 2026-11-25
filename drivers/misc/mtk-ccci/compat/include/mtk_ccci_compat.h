/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Glue between the 4.14 MediaTek CCCI (modem) vendor sources and this
 * kernel. Force-included into every object of drivers/misc/mtk-ccci.
 */
#ifndef __MTK_CCCI_COMPAT_H
#define __MTK_CCCI_COMPAT_H

#include <linux/io.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mm.h>

/* ---- time: struct timeval / timespec are gone from the kernel ---- */
struct timeval {
	long tv_sec;
	long tv_usec;
};
#define timespec timespec64
#define compat_timeval old_timeval32
static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}
#define ktime_to_timespec(kt)		ktime_to_timespec64(kt)
#define getnstimeofday(ts)		ktime_get_real_ts64(ts)
#define get_monotonic_boottime(ts)	ktime_get_boottime_ts64(ts)
#define getrawmonotonic(ts)		ktime_get_raw_ts64(ts)
#define timespec_to_ns(ts)		timespec64_to_ns(ts)

/* ---- timers ---- */
#define from_timer(var, t, f)		timer_container_of(var, t, f)
#define del_timer(t)			timer_delete(t)
#define del_timer_sync(t)		timer_delete_sync(t)

/* ---- driver model / io / net ---- */
#define class_create(owner, name)	class_create(name)
#define ioremap_nocache(a, s)		ioremap(a, s)
#define netif_rx_ni(skb)		netif_rx(skb)
#define strlcpy(d, s, n)		strscpy(d, s, n)

/* <mt-plat/sync_write.h> */
#define mt_reg_sync_writeb(v, a)	do { writeb((v), (void __iomem *)(a)); mb(); } while (0)
#define mt_reg_sync_writew(v, a)	do { writew((v), (void __iomem *)(a)); mb(); } while (0)
#define mt_reg_sync_writel(v, a)	do { writel((v), (void __iomem *)(a)); mb(); } while (0)

/* ---- wakeup sources were embedded structs in 4.14, pointers here ---- */
#include <linux/pm_wakeup.h>
#define wakeup_source_init(ws, n)	((ws) = wakeup_source_register(NULL, (n)))

/* ---- cache maintenance (arm64 renamed the helpers in 5.14) ---- */
#include <asm/cacheflush.h>
#define __inval_dcache_area(p, l)	\
	dcache_inval_poc((unsigned long)(p), (unsigned long)(p) + (l))
#define __flush_dcache_area(p, l)	\
	dcache_clean_inval_poc((unsigned long)(p), (unsigned long)(p) + (l))

/* ---- rtc ---- */
#include <linux/rtc.h>
#define rtc_time_to_tm(t, tm)		rtc_time64_to_tm((time64_t)(t), tm)

/* ---- SMC: MTK SiP service of the CCCI driver ---- */
#include <linux/soc/mediatek/mtk_sip_svc.h>
#define MTK_SIP_KERNEL_CCCI_CONTROL	MTK_SIP_SMC_CMD(0x505)

/* Kill switch, see mtk-ccci-hw.c */
extern bool mtk_ccci_hw;
extern bool mtk_ccci_power;

/* Rate-limited bring-up trace to dmesg (CCCI_*_LOG only reach ccci_dump) */
#include <linux/ratelimit.h>
extern struct ratelimit_state mtk_ccci_log_rs;
#define mtk_ccci_log(fmt, ...)						\
	do {								\
		if (__ratelimit(&mtk_ccci_log_rs))			\
			pr_info("mtk-ccci: " fmt, ##__VA_ARGS__);	\
	} while (0)
#define mtk_ccci_log_err(fmt, ...)					\
	do {								\
		if (__ratelimit(&mtk_ccci_log_rs))			\
			pr_err("mtk-ccci: " fmt, ##__VA_ARGS__);	\
	} while (0)

/*
 * The 4.14 vendor code is not -Wall clean for this clang; these warning
 * classes are style issues in the imported sources.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#pragma clang diagnostic ignored "-Wcast-function-type-strict"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types-discards-qualifiers"
#pragma clang diagnostic ignored "-Wvisibility"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#endif
