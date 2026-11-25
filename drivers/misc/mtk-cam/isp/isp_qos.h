/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cam_qos.c replacement. mmdvfs/PM QoS do not exist on this kernel, so
 * bandwidth requests are accepted and dropped, and the clock queries report
 * the single rate the ISP mux runs at (the vendor EP_PMQOS behaviour).
 */
#ifndef __MTK_CAM_ISP_QOS_H__
#define __MTK_CAM_ISP_QOS_H__

enum E_QOS_OP {
	E_QOS_UNKNOWN,
	E_BW_ADD,
	E_BW_UPDATE,
	E_BW_CLR,
	E_BW_REMOVE,
	E_CLK_ADD,
	E_CLK_UPDATE,
	E_CLK_CLR,
	E_CLK_REMOVE,
	E_CLK_SUPPORTED,	/*supported clk*/
	E_CLK_CUR,		/*current clk*/
	E_MAX,
};

/* MHz, top_cam mux rate used by the stock EP_PMQOS path */
#define ISP_QOS_CLK_MHZ	624

static inline int isp_qos_common(enum E_QOS_OP cmd, unsigned int *pvalue)
{
	switch (cmd) {
	case E_CLK_SUPPORTED:
		pvalue[0] = ISP_QOS_CLK_MHZ;
		return 1;
	case E_CLK_CUR:
		pvalue[0] = ISP_QOS_CLK_MHZ;
		pvalue[1] = ISP_QOS_CLK_MHZ;
		return 0;
	case E_QOS_UNKNOWN:
	case E_MAX:
		return -1;
	default:
		return 0;
	}
}

static inline int ISP_SetPMQOS(enum E_QOS_OP cmd,
			       enum ISP_IRQ_TYPE_ENUM module,
			       unsigned int *pvalue)
{
	return isp_qos_common(cmd, pvalue);
}

static inline int SV_SetPMQOS(enum E_QOS_OP cmd,
			      enum ISP_IRQ_TYPE_ENUM module,
			      unsigned int *pvalue)
{
	return isp_qos_common(cmd, pvalue);
}

#endif
