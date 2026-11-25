/*
 * Copyright (c) 2016 MediaTek Inc.
 * Author: PoChun Lin <pochun.lin@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <media/v4l2-mem2mem.h>
#include "mtk_vcu_controls.h"
#include "mtk_vcu.h"
#include "venc_vcu_if.h"
#include "mtk_vcodec_intr.h"
#include "mtk_vcodec_enc_pm.h"
#include "mtk_vcodec_enc.h"
#include "venc_drv_if.h"

static LIST_HEAD(venc_instances);
static DEFINE_MUTEX(venc_instances_lock);

static void vcu_enc_register_inst(struct venc_vcu_inst *vcu)
{
	mutex_lock(&venc_instances_lock);
	INIT_LIST_HEAD(&vcu->list);
	list_add_tail(&vcu->list, &venc_instances);
	vcu->registered = true;
	mutex_unlock(&venc_instances_lock);
}

static void vcu_enc_unregister_inst(struct venc_vcu_inst *vcu)
{
	mutex_lock(&venc_instances_lock);
	list_del_init(&vcu->list);
	vcu->registered = false;
	mutex_unlock(&venc_instances_lock);
}

static int handle_enc_init_msg(struct venc_vcu_inst *vcu, void *data)
{
	struct venc_vcu_ipi_msg_init *msg = data;

	vcu->inst_addr = msg->vcu_inst_addr;
	vcu->vsi = vcu_mapping_dm_addr_range(vcu->dev, msg->vcu_inst_addr,
					    sizeof(struct venc_vsi));
	return vcu->vsi ? 0 : -EINVAL;
}

static int handle_query_cap_ack_msg(struct venc_vcu_inst *vcu,
	struct venc_vcu_ipi_query_cap_ack *msg)
{
	void *data;
	size_t size;

	if (!vcu->query_out || msg->id != vcu->query_id ||
	    msg->ap_data_addr != (u64)(uintptr_t)vcu->query_out)
		return -EINVAL;
	switch (msg->id) {
	case VENC_GET_PARAM_CAPABILITY_SUPPORTED_FORMATS:
		size = sizeof(struct mtk_video_fmt) * MTK_MAX_ENC_CODECS_SUPPORT;
		break;
	case VENC_GET_PARAM_CAPABILITY_FRAME_SIZES:
		size = sizeof(struct mtk_codec_framesizes) * MTK_MAX_ENC_CODECS_SUPPORT;
		break;
	default:
		return -EINVAL;
	}
	data = vcu_mapping_dm_addr_range(vcu->dev, msg->vcu_data_addr, size);
	if (!data)
		return -EINVAL;
	memcpy(vcu->query_out, data, size);
	return 0;
}

static void handle_enc_waitisr_msg(struct venc_vcu_inst *vcu,
	void *data, uint32_t timeout)
{
	struct venc_vcu_ipi_msg_waitisr *msg = data;
	struct mtk_vcodec_ctx *ctx = vcu->ctx;

	msg->irq_status = ctx->irq_status;
	msg->timeout = timeout;
}

static int vcu_enc_handle_message(struct venc_vcu_inst *vcu,
	void *data, unsigned int len, void *priv)
{
	struct venc_vcu_ipi_msg_common *msg = data;
	struct mtk_vcodec_ctx *ctx;
	int ret = 0;
	unsigned long flags;
	int lock = -1;

	BUILD_BUG_ON(sizeof(struct venc_ap_ipi_msg_init) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_ap_ipi_query_cap) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_ap_ipi_msg_set_param) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_ap_ipi_msg_enc) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_ap_ipi_msg_deinit) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(
		sizeof(struct venc_vcu_ipi_query_cap_ack) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_vcu_ipi_msg_common) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_vcu_ipi_msg_init) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(
		sizeof(struct venc_vcu_ipi_msg_set_param) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_vcu_ipi_msg_enc) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_vcu_ipi_msg_deinit) > SHARE_BUF_SIZE);
	BUILD_BUG_ON(sizeof(struct venc_vcu_ipi_msg_waitisr) > SHARE_BUF_SIZE);

	if (msg->msg_id < VCU_IPIMSG_VENC_SEND_BASE && vcu != priv)
		return 1;
	if (vcu->daemon_pid != current->tgid)
		return -EINVAL;

	mtk_vcodec_debug(vcu, "msg_id %x inst %p status %d",
					 msg->msg_id, vcu, msg->status);

	if (vcu->abort)
		return -EINVAL;

	ctx = vcu->ctx;
	switch (msg->msg_id) {
	case VCU_IPIMSG_ENC_INIT_DONE:
		if (len < sizeof(struct venc_vcu_ipi_msg_init))
			return -EINVAL;
		if (!msg->status && handle_enc_init_msg(vcu, data))
			msg->status = -EINVAL;
		break;
	case VCU_IPIMSG_ENC_SET_PARAM_DONE:
		/* Prevent slowmotion with GCE mode on,
		 * user thread enter freezing while holding mutex (enc lock)
		 */

		break;
	case VCU_IPIMSG_ENC_DEINIT_DONE:
		break;
	case VCU_IPIMSG_ENC_POWER_ON:
		vcu_get_gce_lock(vcu->dev, VCU_VENC);
		while (lock != 0) {
			lock = venc_lock(ctx, 0, true);
			if (lock != 0) {
				vcu_put_gce_lock(vcu->dev, VCU_VENC);
				usleep_range(1000, 2000);
				vcu_get_gce_lock(vcu->dev, VCU_VENC);
			}
		}
		ret = venc_encode_prepare(ctx, 0, &flags);
		if (ret) {
			msg->status = ret;
			venc_unlock(ctx, 0);
		}
		vcu_put_gce_lock(vcu->dev, VCU_VENC);
		ret = 1;
		break;
	case VCU_IPIMSG_ENC_POWER_OFF:
		vcu_get_gce_lock(vcu->dev, VCU_VENC);
		venc_encode_unprepare(ctx, 0, &flags);
		venc_unlock(ctx, 0);
		vcu_put_gce_lock(vcu->dev, VCU_VENC);
		ret = 1;
		break;
	case VCU_IPIMSG_ENC_QUERY_CAP_ACK:
		if (len < sizeof(struct venc_vcu_ipi_query_cap_ack))
			return -EINVAL;
		if (!msg->status && handle_query_cap_ack_msg(vcu, data))
			msg->status = -EINVAL;
		break;
	case VCU_IPIMSG_ENC_WAIT_ISR:
		if (len < sizeof(struct venc_vcu_ipi_msg_waitisr))
			return -EINVAL;
		if (!ctx->dev->pm.powered) {
			mtk_vcodec_err(vcu, "wait isr without power on");
			msg->status = -EIO;
			ret = 1;
			break;
		}
		enable_irq(ctx->dev->enc_irq);
		if (-1 == mtk_vcodec_wait_for_done_ctx(ctx, 0,
			MTK_INST_IRQ_RECEIVED,
			WAIT_INTR_TIMEOUT_MS)) {
			handle_enc_waitisr_msg(vcu, data, 1);
			mtk_vcodec_debug(vcu,
				"irq_status %x <-", ctx->irq_status);
		} else
			handle_enc_waitisr_msg(vcu, data, 0);
		disable_irq(ctx->dev->enc_irq);
		ret = 1;
		break;
	case VCU_IPIMSG_ENC_PUT_BUFFER:
		mtk_enc_put_buf(ctx);
		ret = 1;
		break;
	case VCU_IPIMSG_ENC_ENCODE_ACK:
		break;
	default:
		mtk_vcodec_err(vcu, "unknown msg id %x", msg->msg_id);
		break;
	}

	/* deinit done timeout case handling do not touch vdec_vcu_inst
	 * or memory used after freed
	 */
	if (msg->msg_id != VCU_IPIMSG_ENC_DEINIT_DONE) {
		vcu->signaled = 1;
		vcu->failure = (msg->status != VENC_IPI_MSG_STATUS_OK);
	}

	mtk_vcodec_debug_leave(vcu);
	return ret;
}

int vcu_enc_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct venc_vcu_ipi_msg_common *msg = data;
	struct venc_vcu_inst *vcu;
	int ret = -EINVAL;

	if (len < sizeof(*msg))
		return ret;
	/* Resolve echoed cookies only against registered, live instances. */
	mutex_lock(&venc_instances_lock);
	list_for_each_entry(vcu, &venc_instances, list) {
		if (msg->venc_inst != (u64)(uintptr_t)vcu)
			continue;
		ret = vcu_enc_handle_message(vcu, data, len, priv);
		break;
	}
	mutex_unlock(&venc_instances_lock);
	return ret;
}

static int vcu_enc_send_msg(struct venc_vcu_inst *vcu, void *msg,
							int len)
{
	int status;

	mtk_vcodec_debug_enter(vcu);

	if (!vcu->dev) {
		mtk_vcodec_err(vcu, "inst dev is NULL");
		return -EINVAL;
	}

	if (vcu->abort)
		return -EIO;

	if (vcu->daemon_pid < 0 ||
	    vcu->daemon_pid != vcu_get_daemon_pid(vcu->dev)) {
		vcu->abort = 1;
		return -EIO;
	}

	status = vcu_ipi_send(vcu->dev, vcu->id, msg, len, vcu);
	if (status) {
		mtk_vcodec_err(vcu, "vcu_ipi_send msg_id %x len %d fail %d",
					   *(uint32_t *)msg, len, status);
		if (status == -EIO)
			vcu->abort = 1;
		return status;
	}
	if (vcu->failure)
		return -EINVAL;

	mtk_vcodec_debug_leave(vcu);

	return 0;
}


static void vcu_enc_set_pid(struct venc_vcu_inst *vcu)
{

	vcu->daemon_pid = vcu_get_daemon_pid(vcu->dev);
}

static int vcu_enc_set_ctx(struct venc_vcu_inst *vcu,
	struct venc_frm_buf *frm_buf,
	struct mtk_vcodec_mem *bs_buf)
{
	int err = 0;
	struct vb2_buffer *src_vb = NULL;
	struct vb2_buffer *dst_vb = NULL;
	struct mtk_video_enc_buf *temp;

	if (frm_buf != NULL) {
		temp = container_of(frm_buf, struct mtk_video_enc_buf, frm_buf);
		src_vb = &temp->vb.vb2_buf;
	}
	if (bs_buf != NULL) {
		temp = container_of(bs_buf, struct mtk_video_enc_buf, bs_buf);
		dst_vb = &temp->vb.vb2_buf;
	}
	vcu_set_codec_ctx(vcu->dev,
		(void *)vcu->ctx,
		src_vb, dst_vb,
		VCU_VENC);

	return err;
}

static int vcu_enc_clear_ctx(struct venc_vcu_inst *vcu)
{
	int err = 0;

	vcu_clear_codec_ctx(vcu->dev,
		(void *)vcu->ctx, VCU_VENC);

	return err;
}

int vcu_enc_init(struct venc_vcu_inst *vcu)
{
	int status;
	struct venc_ap_ipi_msg_init out;

	mtk_vcodec_debug_enter(vcu);

	if (sizeof(out) > SHARE_BUF_SIZE) {
		mtk_vcodec_err(vcu, "venc_ap_ipi_msg_init cannot be large than %d",
					   SHARE_BUF_SIZE);
		return -EINVAL;
	}

	init_waitqueue_head(&vcu->wq_hd);
	vcu->signaled = 0;
	vcu->failure = 0;
	vcu_get_ctx_ipi_binding_lock(vcu->dev, &vcu->ctx_ipi_binding, VCU_VENC);

	status = vcu_ipi_register(vcu->dev, vcu->id, vcu->handler,
							  NULL, vcu);
	if (status) {
		mtk_vcodec_err(vcu, "vcu_ipi_register fail %d", status);
		return -EINVAL;
	}

	memset(&out, 0, sizeof(out));
	out.msg_id = AP_IPIMSG_ENC_INIT;
	out.venc_inst = (unsigned long)vcu;

	vcu_enc_set_pid(vcu);
	vcu_enc_register_inst(vcu);
	status = vcu_enc_send_msg(vcu, &out, sizeof(out));

	if (status) {
		vcu_enc_unregister_inst(vcu);
		mtk_vcodec_err(vcu, "AP_IPIMSG_ENC_INIT fail");
		return -EINVAL;
	}
	mtk_vcodec_debug_leave(vcu);

	return 0;
}

int vcu_enc_query_cap(struct venc_vcu_inst *vcu, unsigned int id, void *out)
{
	struct venc_ap_ipi_query_cap msg;
	bool temporary = !vcu->registered;
	int err = 0;

	if (sizeof(msg) > SHARE_BUF_SIZE) {
		mtk_vcodec_err(vcu, "venc_ap_ipi_query_cap cannot be large than %d",
					   SHARE_BUF_SIZE);
		return -EINVAL;
	}

	mtk_vcodec_debug(vcu, "+ id=%X", AP_IPIMSG_ENC_QUERY_CAP);
	vcu->dev = vcu_get_plat_device(vcu->ctx->dev->plat_dev);
	if (vcu->dev  == NULL) {
		mtk_vcodec_err(vcu, "vcu device in not ready");
		return -EPROBE_DEFER;
	}

	vcu->id = (vcu->id == IPI_VCU_INIT) ? IPI_VENC_COMMON : vcu->id;
	vcu->handler = vcu_enc_ipi_handler;

	err = vcu_ipi_register(vcu->dev,
		vcu->id, vcu->handler, NULL, vcu);
	if (err != 0) {
		mtk_vcodec_err(vcu, "vcu_ipi_register fail status=%d", err);
		return err;
	}

	memset(&msg, 0, sizeof(msg));
	msg.msg_id = AP_IPIMSG_ENC_QUERY_CAP;
	msg.id = id;
	msg.ap_inst_addr = (uintptr_t)vcu;
	msg.ap_data_addr = (uintptr_t)out;

	vcu_enc_set_pid(vcu);
	vcu->query_out = out;
	vcu->query_id = id;
	if (temporary)
		vcu_enc_register_inst(vcu);
	err = vcu_enc_send_msg(vcu, &msg, sizeof(msg));
	if (temporary)
		vcu_enc_unregister_inst(vcu);
	vcu->query_out = NULL;
	mtk_vcodec_debug(vcu, "- id=%X ret=%d", msg.msg_id, err);

	return err;
}

int vcu_enc_set_param(struct venc_vcu_inst *vcu,
					  enum venc_set_param_type id,
					  struct venc_enc_param *enc_param)
{
	struct venc_ap_ipi_msg_set_param out;

	mtk_vcodec_debug(vcu, "id %d ->", id);
	if (sizeof(out) > SHARE_BUF_SIZE) {
		mtk_vcodec_err(vcu, "venc_ap_ipi_msg_set_param cannot be large than %d",
					   SHARE_BUF_SIZE);
		return -EINVAL;
	}

	memset(&out, 0, sizeof(out));
	out.msg_id = AP_IPIMSG_ENC_SET_PARAM;
	out.vcu_inst_addr = vcu->inst_addr;
	out.param_id = id;
	switch (id) {
	case VENC_SET_PARAM_ENC:
		out.data_item = 0;
		break;
	case VENC_SET_PARAM_FORCE_INTRA:
		out.data_item = 0;
		break;
	case VENC_SET_PARAM_ADJUST_BITRATE:
		out.data_item = 1;
		out.data[0] = enc_param->bitrate;
		break;
	case VENC_SET_PARAM_ADJUST_FRAMERATE:
		out.data_item = 1;
		out.data[0] = enc_param->frm_rate;
		break;
	case VENC_SET_PARAM_GOP_SIZE:
		out.data_item = 1;
		out.data[0] = enc_param->gop_size;
		break;
	case VENC_SET_PARAM_INTRA_PERIOD:
		out.data_item = 1;
		out.data[0] = enc_param->intra_period;
		break;
	case VENC_SET_PARAM_SKIP_FRAME:
		out.data_item = 0;
		break;
	case VENC_SET_PARAM_PREPEND_HEADER:
		out.data_item = 0;
		break;
	case VENC_SET_PARAM_SCENARIO:
		out.data_item = 1;
		out.data[0] = enc_param->scenario;
		break;
	case VENC_SET_PARAM_NONREFP:
		out.data_item = 1;
		out.data[0] = enc_param->nonrefp;
		break;
	case VENC_SET_PARAM_NONREFPFREQ:
		out.data_item = 1;
		out.data[0] = enc_param->nonrefpfreq;
		break;
	case VENC_SET_PARAM_DETECTED_FRAMERATE:
		out.data_item = 1;
		out.data[0] = enc_param->detectframerate;
		break;
	case VENC_SET_PARAM_RFS_ON:
		out.data_item = 1;
		out.data[0] = enc_param->rfs;
		break;
	case VENC_SET_PARAM_PREPEND_SPSPPS_TO_IDR:
		out.data_item = 1;
		out.data[0] = enc_param->prependheader;
		break;
	case VENC_SET_PARAM_OPERATION_RATE:
		out.data_item = 1;
		out.data[0] = enc_param->operationrate;
		break;
	case VENC_SET_PARAM_BITRATE_MODE:
		out.data_item = 1;
		out.data[0] = enc_param->bitratemode;
		break;
	case VENC_SET_PARAM_ROI_ON:
		out.data_item = 1;
		out.data[0] = enc_param->roion;
		break;
	case VENC_SET_PARAM_HEIF_GRID_SIZE:
		out.data_item = 1;
		out.data[0] = enc_param->heif_grid_size;
		break;
	case VENC_SET_PARAM_COLOR_DESC:
		out.data_item = 0; // passed via vsi
		break;
	case VENC_SET_PARAM_SEC_MODE:
		out.data_item = 1;
		out.data[0] = enc_param->svp_mode;
		break;
	case VENC_SET_PARAM_TSVC:
		out.data_item = 1;
		out.data[0] = enc_param->tsvc;
		break;
	case VENC_SET_PARAM_ADJUST_MAX_QP:
		out.data_item = 1;
		out.data[0] = enc_param->max_qp;
		break;
	case VENC_SET_PARAM_ADJUST_MIN_QP:
		out.data_item = 1;
		out.data[0] = enc_param->min_qp;
		break;
	case VENC_SET_PARAM_ADJUST_I_P_QP_DELTA:
		out.data_item = 1;
		out.data[0] = enc_param->i_p_qp_delta;
		break;
	case VENC_SET_PARAM_ADJUST_FRAME_LEVEL_QP:
		out.data_item = 1;
		out.data[0] = enc_param->frame_level_qp;
		break;
	case VENC_SET_PARAM_MAX_REFP_NUM:
		out.data_item = 1;
		out.data[0] = enc_param->maxrefpnum;
		break;
	case VENC_SET_PARAM_REFP_DISTANCE:
		out.data_item = 1;
		out.data[0] = enc_param->refpdistance;
		break;
	case VENC_SET_PARAM_REFP_FRMNUM:
		out.data_item = 1;
		out.data[0] = enc_param->refpfrmnum;
		break;
	case VENC_SET_PARAM_ENABLE_DUMMY_NAL:
		out.data_item = 1;
		out.data[0] = enc_param->dummynal;
		break;
	default:
		mtk_vcodec_err(vcu, "id %d not supported", id);
		return -EINVAL;
	}

	if (vcu_enc_send_msg(vcu, &out, sizeof(out))) {
		mtk_vcodec_err(vcu,
			"AP_IPIMSG_ENC_SET_PARAM %d fail", id);
		return -EINVAL;
	}

	mtk_vcodec_debug(vcu, "id %d <-", id);

	return 0;
}

int vcu_enc_encode(struct venc_vcu_inst *vcu, unsigned int bs_mode,
				   struct venc_frm_buf *frm_buf,
				   struct mtk_vcodec_mem *bs_buf,
				   unsigned int *bs_size)
{

	struct venc_ap_ipi_msg_enc out;
	struct venc_vsi *vsi = (struct venc_vsi *)vcu->vsi;
	unsigned int i, ret;

	mtk_vcodec_debug(vcu, "bs_mode %d ->", bs_mode);

	if (sizeof(out) > SHARE_BUF_SIZE) {
		mtk_vcodec_err(vcu, "venc_ap_ipi_msg_enc cannot be large than %d",
					   SHARE_BUF_SIZE);
		return -EINVAL;
	}

	memset(&out, 0, sizeof(out));
	out.msg_id = AP_IPIMSG_ENC_ENCODE;
	out.vcu_inst_addr = vcu->inst_addr;
	out.bs_mode = bs_mode;
	if (frm_buf) {
		out.fb_num_planes = frm_buf->num_planes;
		for (i = 0; i < frm_buf->num_planes; i++) {
			out.input_addr[i] =
				frm_buf->fb_addr[i].dma_addr;
			out.input_size[i] =
				frm_buf->fb_addr[i].size;
			out.data_offset[i] =
				frm_buf->fb_addr[i].data_offset;
		}
		if (frm_buf->has_meta) {
			vsi->meta_fd = 0;
			vsi->meta_size = sizeof(struct mtk_hdr_dynamic_info);
			vsi->meta_addr = frm_buf->meta_addr;
		} else {
			vsi->meta_fd = 0;
			vsi->meta_size = 0;
			vsi->meta_addr = 0;
		}

		if (frm_buf->qpmap != 0) {
			out.qpmap = frm_buf->qpmap;
		}

		mtk_vcodec_debug(vcu, " num_planes = %d input (dmabuf:%lx fd:%x), metafd %x metasize %d %llx",
			frm_buf->num_planes,
			(unsigned long)frm_buf->fb_addr[0].dmabuf,
			out.input_fd[0], vsi->meta_fd, vsi->meta_size,
			vsi->meta_addr);
	}

	if (bs_buf) {
		out.bs_addr = bs_buf->dma_addr;
		out.bs_size = bs_buf->size;

		mtk_vcodec_debug(vcu, " output (dma:%lx fd:%x)",
			(unsigned long)bs_buf->dmabuf,
			out.bs_fd);
	}

	mutex_lock(vcu->ctx_ipi_binding);
	vcu_enc_set_ctx(vcu, frm_buf, bs_buf);
	ret = vcu_enc_send_msg(vcu, &out, sizeof(out));
	mutex_unlock(vcu->ctx_ipi_binding);

	if (ret) {
		mtk_vcodec_err(vcu, "AP_IPIMSG_ENC_ENCODE %d fail %d",
					   bs_mode, ret);
		return ret;
	}

	if (frm_buf) {
		if (frm_buf->has_meta) {
			dma_buf_put(frm_buf->meta_dma);
		}
	}

	mtk_vcodec_debug(vcu, "bs_mode %d size %d key_frm %d <-",
		bs_mode, vcu->bs_size, vcu->is_key_frm);

	return 0;
}

int vcu_enc_deinit(struct venc_vcu_inst *vcu)
{
	struct venc_ap_ipi_msg_deinit out;
	int ret = 0;

	mtk_vcodec_debug_enter(vcu);

	if (sizeof(out) > SHARE_BUF_SIZE) {
		mtk_vcodec_err(vcu, "venc_ap_ipi_msg_deint cannot be large than %d",
					   SHARE_BUF_SIZE);
		return -EINVAL;
	}

	memset(&out, 0, sizeof(out));
	out.msg_id = AP_IPIMSG_ENC_DEINIT;
	out.vcu_inst_addr = vcu->inst_addr;

	mutex_lock(vcu->ctx_ipi_binding);
	ret = vcu_enc_send_msg(vcu, &out, sizeof(out));
	vcu_enc_clear_ctx(vcu);
	mutex_unlock(vcu->ctx_ipi_binding);
	vcu_enc_unregister_inst(vcu);

	mtk_vcodec_debug_leave(vcu);

	return ret;
}
