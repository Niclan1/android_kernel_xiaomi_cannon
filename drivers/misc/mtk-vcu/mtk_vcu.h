/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Andrew-CT Chen <andrew-ct.chen@mediatek.com>
 *
 * In-kernel interface of the MediaTek VCU bridge. The VCU "processor" is the
 * userspace daemon vpud; codec drivers exchange IPI messages with it through
 * /dev/vcu.
 */

#ifndef MTK_VCU_H
#define MTK_VCU_H

#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>

struct vb2_buffer;

typedef int (*ipi_handler_t)(void *data, unsigned int len, void *priv);

enum ipi_id {
	IPI_VCU_INIT = 0,
	IPI_VDEC_COMMON,
	IPI_VDEC_H264,
	IPI_VDEC_H265,
	IPI_VDEC_HEIF,
	IPI_VDEC_VP8,
	IPI_VDEC_VP9,
	IPI_VDEC_MPEG4,
	IPI_VDEC_H263,
	IPI_VDEC_MPEG12,
	IPI_VDEC_WMV,
	IPI_VDEC_RV30,
	IPI_VDEC_RV40,
	IPI_VDEC_AV1,
	IPI_VENC_COMMON,
	IPI_VENC_H264,
	IPI_VENC_H265,
	IPI_VENC_HEIF,
	IPI_VENC_VP8,
	IPI_VENC_MPEG4,
	IPI_VENC_HYBRID_H264,
	IPI_VENC_H263,
	IPI_MDP,
	IPI_MDP_1,
	IPI_MDP_2,
	IPI_MDP_3,
	IPI_CAMERA,
	IPI_MAX = 50,
};

enum vcu_codec_type {
	VCU_VDEC = 0,
	VCU_VENC,
	VCU_CODEC_MAX
};

/*
 * Hooks a codec front end may install to bracket GCE jobs with its own power,
 * clock and bandwidth handling. Every hook is optional.
 */
struct vcu_codec_ops {
	int (*encode_prepare)(void *ctx, unsigned int core_id,
			       unsigned long *flags);
	void (*encode_unprepare)(void *ctx, unsigned int core_id,
				 unsigned long *flags);
	int (*venc_lock)(void *ctx, int core_id, bool sec);
	void (*venc_unlock)(void *ctx, int core_id);
	void (*pmqos_gce_begin)(void *ctx, unsigned int core_id, int job_cnt);
	void (*pmqos_gce_end)(void *ctx, unsigned int core_id, int job_cnt);
	void (*vdec_check_release_lock)(void *ctx);
	void (*venc_gce_error)(void *ctx, int status);
};

int vcu_ipi_register(struct platform_device *pdev, enum ipi_id id,
		     ipi_handler_t handler, const char *name, void *priv);
int vcu_ipi_send(struct platform_device *pdev, enum ipi_id id, void *buf,
		 unsigned int len, void *priv);
struct platform_device *vcu_get_plat_device(struct platform_device *pdev);
unsigned int vcu_get_vdec_hw_capa(struct platform_device *pdev);
unsigned int vcu_get_venc_hw_capa(struct platform_device *pdev);
int vcu_load_firmware(struct platform_device *pdev);
int vcu_compare_version(struct platform_device *pdev,
			const char *expected_version);
void *vcu_mapping_dm_addr_range(struct platform_device *pdev,
			       uintptr_t address, size_t size);
void *vcu_mapping_dm_addr(struct platform_device *pdev,
			  uintptr_t dtcm_dmem_addr);
void vcu_get_gce_lock(struct platform_device *pdev, unsigned long codec_type);
void vcu_put_gce_lock(struct platform_device *pdev, unsigned long codec_type);
int vcu_check_vpud_alive(void);
int vcu_get_daemon_pid(struct platform_device *pdev);
int vcu_get_ctx_ipi_binding_lock(struct platform_device *pdev,
				 struct mutex **mutex, unsigned long type);
int vcu_set_codec_ctx(struct platform_device *pdev, void *codec_ctx,
		      struct vb2_buffer *src_vb, struct vb2_buffer *dst_vb,
		      unsigned long type);
int vcu_clear_codec_ctx(struct platform_device *pdev, void *codec_ctx,
			unsigned long type);
void vcu_set_codec_ops(struct platform_device *pdev,
		       const struct vcu_codec_ops *ops);
int vcu_set_log(const char *val);
/*
 * Power hooks for the encoder register window: held while vpud has it
 * mmapped through /dev/vcu.
 */
void vcu_set_reg_power(struct platform_device *pdev,
		       int (*get)(void *priv), void (*put)(void *priv),
		       void *priv);
bool vcu_trace_on(void);
bool vcu_iommu_trace_on(void);
__printf(1, 2) void vcu_trace_log(const char *fmt, ...);

#endif /* MTK_VCU_H */
