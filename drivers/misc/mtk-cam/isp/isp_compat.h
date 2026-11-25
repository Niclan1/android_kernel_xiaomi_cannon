/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Glue between the MT6853 4.14 vendor ISP P1 driver and mainline 6.18.
 *
 *  - SMI larb/MTCMOS control is replaced by runtime PM (see camera_isp.c):
 *    the camsys node attaches the cam/cam-rawa/cam-rawb power domains and
 *    the cam1/cam2/camsv nodes carry "iommus", so mtk_iommu links them to
 *    their larbs and configures the M4U ports.
 *  - ION handles kept by the driver are plain dma-buf references.
 *  - MET, mmdvfs PM QoS and the vendor timesync driver do not exist.
 */
#ifndef __MTK_CAM_ISP_COMPAT_H__
#define __MTK_CAM_ISP_COMPAT_H__

#include <linux/dma-buf.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <clocksource/arm_arch_timer.h>

#define EP_MARK_SMI
#define EP_NO_K_LOG_ADJUST

/* <mt-plat/sync_write.h> */
#define mt_reg_sync_writel(v, a)			\
	do {						\
		writel((v), (void __iomem *)(a));	\
		mb();					\
	} while (0)

/* clk-mt6853-pg.h: power-gating callbacks do not exist with genpd */
enum subsys_id {
	SYS_CAM = 20,
	SYS_CAM_RAWA = 21,
	SYS_CAM_RAWB = 22,
};

struct pg_callbacks {
	void (*before_off)(enum subsys_id id);
	void (*after_on)(enum subsys_id id);
	void (*debug_dump)(enum subsys_id id);
};

static inline struct pg_callbacks *register_pg_callback(struct pg_callbacks *pgcb)
{
	return NULL;
}

/* smi_debug.h */
static inline int smi_debug_bus_hang_detect(bool gce, const char *user)
{
	return 0;
}

/* struct timeval left the kernel; the driver only uses sec/usec pairs */
#define timeval __kernel_old_timeval

/* archcounter_timesync.h: arch counter ticks -> CLOCK_MONOTONIC/BOOTTIME ns */
static inline u64 isp_tick_to_ns(u64 tick, u64 now_ns)
{
	u64 now_tick = arch_timer_read_counter();
	u32 rate = arch_timer_get_rate();
	u64 delta;

	if (!rate)
		return now_ns;
	if (tick <= now_tick) {
		delta = mul_u64_u64_div_u64(now_tick - tick, NSEC_PER_SEC, rate);
		return delta < now_ns ? now_ns - delta : 0;
	}
	delta = mul_u64_u64_div_u64(tick - now_tick, NSEC_PER_SEC, rate);
	return now_ns + delta;
}

static inline u64 archcounter_timesync_to_monotonic(u64 tick)
{
	return isp_tick_to_ns(tick, ktime_get_ns());
}

static inline u64 archcounter_timesync_to_boot(u64 tick)
{
	return isp_tick_to_ns(tick, ktime_get_boottime_ns());
}

/* ion_drv.h: the driver only pins buffers, a dma-buf reference does that */
struct ion_device {
	int unused;
};

struct ion_client {
	int unused;
};

struct ion_handle;

static struct ion_device isp_ion_device;
static struct ion_client isp_ion_client;
#define g_ion_device (&isp_ion_device)

static inline struct ion_client *ion_client_create(struct ion_device *dev,
						   const char *name)
{
	return &isp_ion_client;
}

static inline void ion_client_destroy(struct ion_client *client)
{
}

static inline struct ion_handle *ion_import_dma_buf_fd(struct ion_client *client,
						       int fd)
{
	return (struct ion_handle *)dma_buf_get(fd);
}

static inline void ion_free(struct ion_client *client, struct ion_handle *handle)
{
	dma_buf_put((struct dma_buf *)handle);
}

#endif
