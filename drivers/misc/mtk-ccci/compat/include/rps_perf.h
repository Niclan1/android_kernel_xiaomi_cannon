/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <rps_perf.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_RPS_PERF_H
#define __MTK_CCCI_RPS_PERF_H
struct netdev_rx_queue;
/* RPS masks are set by the vendor init rc through sysfs. */
static inline int set_rps_map(struct netdev_rx_queue *queue, unsigned long rps_value)
{
	return 0;
}
#endif
