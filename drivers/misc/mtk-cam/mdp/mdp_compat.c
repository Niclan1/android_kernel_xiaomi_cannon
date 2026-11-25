// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/mtk_mdp compat for the stock MT6853 libdpframework / P2 HAL.
 *
 * Implements the 4.14 "meta" CMDQ ABI (CMDQ_IOCTL_ASYNC_EXEC/WAIT and the
 * readback slots) on top of the mainline GCE mailbox: user-space sends a list
 * of op_meta {engine, offset, value}; we turn it into a GCE packet using
 * absolute-address (SPR) instructions, translate dma-buf fds to IOVAs through
 * the camera/MDP IOMMU domain (larb2 ports) and map MTK event enums to GCE
 * event ids via the 4.14 DT properties on mdp_rdma0.
 *
 * Behind mtk_cam.hw=1 mtk_cam.mdp=1. Every wait is bounded; a timed-out GCE
 * thread is aborted (channel freed and re-requested) so nothing can hang.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/mailbox_controller.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "../mtk_cam.h"
#include "cmdq_subsys_common.h"
#include "mdp_event_common.h"
#include "mdp_event_table.h"
#include "mdp_subsys_table.h"

#define MDP_TAG "mtk_mdp_compat: "

/* ---- 4.14 ABI (mdp_def.h / mdp_def_ex.h / mdp_driver.h), keep exact ---- */

#define cmdqU32Ptr_t unsigned long long

enum CMDQ_META_OP {
	CMDQ_MOP_WRITE = 0,
	CMDQ_MOP_READ,
	CMDQ_MOP_POLL,
	CMDQ_MOP_WAIT,
	CMDQ_MOP_WAIT_NO_CLEAR,
	CMDQ_MOP_CLEAR,
	CMDQ_MOP_SET,
	CMDQ_MOP_ACQUIRE,
	CMDQ_MOP_WRITE_FD,
	CMDQ_MOP_WRITE_FROM_REG,
	CMDQ_MOP_WRITE_SEC,
	CMDQ_MOP_READBACK,
	CMDQ_MOP_WRITE_RDMA,
	CMDQ_MOP_WRITE_FD_RDMA,
	CMDQ_MOP_NOP,
};

struct op_meta {
	uint8_t op;
	uint16_t engine;
	union {
		uint16_t offset;
		uint16_t event;
		struct {
			uint8_t pipe_idx;
			uint8_t cpr_idx;
		};
	};
	union {
		uint32_t value;
		uint32_t readback_id;
		uint32_t fd;
		uint32_t sec_handle;
		struct {
			uint16_t from_offset;
			uint16_t from_engine;
		};
	};
	union {
		uint32_t mask;
		uint32_t fd_offset;
		uint32_t sec_index;
	};
};

struct hw_meta {
	uint16_t offset;
	uint16_t engine;
};

struct cmdqMetaBuf {
	uint64_t va;
	uint64_t size;
};

#define CMDQ_ISP_META_CNT	8

struct cmdqSecIspMeta {
	struct cmdqMetaBuf ispBufs[CMDQ_ISP_META_CNT];
	uint64_t CqSecHandle;
	uint32_t CqSecSize;
	uint32_t CqDesOft;
	uint32_t CqVirtOft;
	uint64_t TpipeSecHandle;
	uint32_t TpipeSecSize;
	uint32_t TpipeOft;
	uint64_t BpciHandle;
	uint64_t LsciHandle;
	uint64_t LceiHandle;
	uint64_t DepiHandle;
	uint64_t DmgiHandle;
};

struct cmdqSecDataStruct {
	bool is_secure;
	uint32_t addrMetadataCount;
	cmdqU32Ptr_t addrMetadatas;
	uint32_t addrMetadataMaxCount;
	uint64_t enginesNeedDAPC;
	uint64_t enginesNeedPortSecurity;
	int32_t waitCookie;
	bool resetExecCnt;
	struct cmdqSecIspMeta ispMeta;
	uint64_t extension;
};

struct mdp_submit {
	cmdqU32Ptr_t metas;
	uint32_t meta_count;
	uint32_t priority;
	uint64_t engine_flag;
	uint32_t prop_size;
	uint64_t prop_addr;
	uint64_t readback_ext;
	uint32_t read_count_v1;
	cmdqU32Ptr_t hw_metas_read_v1;
	uint64_t job_id;
	struct cmdqSecDataStruct secData;
};

struct mdp_read_v1 {
	uint32_t count;
	cmdqU32Ptr_t ret_values;
};

struct mdp_read_readback {
	uint32_t count;
	cmdqU32Ptr_t ids;
	cmdqU32Ptr_t ret_values;
};

struct mdp_wait {
	uint64_t job_id;
	struct mdp_read_v1 read_v1_result;
	struct mdp_read_readback read_result;
};

struct mdp_readback {
	uint32_t count;
	uint32_t start_id;
};

#define CMDQ_SUBSYS_GRPNAME_MAX		(30)
struct SubsysStruct {
	uint32_t msb;
	int32_t subsysID;
	uint32_t mask;
	char grpName[CMDQ_SUBSYS_GRPNAME_MAX];
};

enum CMDQ_MDP_PA_BASE_ENUM {
	CMDQ_MDP_PA_BASE_MM_MUTEX,
	CMDQ_MAX_MDP_PA_BASE_COUNT,
};

struct cmdqDTSDataStruct {
	int32_t eventTable[CMDQ_SYNC_TOKEN_MAX];
	struct SubsysStruct subsys[CMDQ_SUBSYS_MAX_COUNT];
	uint32_t MDPBaseAddress[CMDQ_MAX_MDP_PA_BASE_COUNT];
};

/* mt6853 mdp_engine.h: CMDQ_MAX_ENGINE_COUNT follows the dummy engine 63 */
#define CMDQ_MAX_ENGINE_COUNT	64
#define CMDQ_ENG_INORDER	35

struct cmdqUsageInfoStruct {
	uint32_t count[CMDQ_MAX_ENGINE_COUNT];
};

#define CMDQ_CAP_WFE		0

#define CMDQ_IOCTL_MAGIC_NUMBER 'x'
#define CMDQ_IOCTL_QUERY_USAGE  _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 4, \
	struct cmdqUsageInfoStruct)
#define CMDQ_IOCTL_QUERY_CAP_BITS _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 10, int)
#define CMDQ_IOCTL_QUERY_DTS _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 11, \
	struct cmdqDTSDataStruct)
#define CMDQ_IOCTL_NOTIFY_ENGINE _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 12, uint64_t)
#define CMDQ_IOCTL_ASYNC_EXEC _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 20, \
	struct mdp_submit)
#define CMDQ_IOCTL_ASYNC_WAIT _IOR(CMDQ_IOCTL_MAGIC_NUMBER, 21, \
	struct mdp_wait)
#define CMDQ_IOCTL_ALLOC_READBACK_SLOTS _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 22, \
	struct mdp_readback)
#define CMDQ_IOCTL_FREE_READBACK_SLOTS _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 23, \
	struct mdp_readback)
#define CMDQ_IOCTL_READ_READBACK_SLOTS _IOW(CMDQ_IOCTL_MAGIC_NUMBER, 24, \
	struct mdp_read_readback)

/* ---- mt6853 mdp_base.h (meta.engine is an ENGBASE index) ---- */

enum {
	ENGBASE_MMSYS_CONFIG, ENGBASE_MDP_RDMA0, ENGBASE_MDP_RDMA1,
	ENGBASE_MDP_AAL0, ENGBASE_MDP_AAL1, ENGBASE_MDP_HDR0, ENGBASE_MDP_RSZ0,
	ENGBASE_MDP_RSZ1, ENGBASE_MDP_WROT0, ENGBASE_MDP_WROT1,
	ENGBASE_MDP_TDSHP0, ENGBASE_MDP_TDSHP1, ENGBASE_MDP_COLOR0,
	ENGBASE_MMSYS_MUTEX, ENGBASE_IMGSYS, ENGBASE_ISP_DIP_A0,
	ENGBASE_ISP_DIP_A1, ENGBASE_ISP_DIP_A7, ENGBASE_ISP_MSFDL,
	ENGBASE_ISP_MSS_B, ENGBASE_ISP_MFB_B, ENGBASE_ISP_WPE_A,
	ENGBASE_ISP_WPE_B, ENGBASE_COUNT
};

static const u32 mdp_base[ENGBASE_COUNT] = {
	[ENGBASE_MMSYS_CONFIG] = 0x1f000000,
	[ENGBASE_MDP_RDMA0] = 0x1f003000,
	[ENGBASE_MDP_RDMA1] = 0x1f004000,
	[ENGBASE_MDP_AAL0] = 0x1f005000,
	[ENGBASE_MDP_AAL1] = 0x1f006000,
	[ENGBASE_MDP_HDR0] = 0x1f007000,
	[ENGBASE_MDP_RSZ0] = 0x1f008000,
	[ENGBASE_MDP_RSZ1] = 0x1f009000,
	[ENGBASE_MDP_WROT0] = 0x1f00a000,
	[ENGBASE_MDP_WROT1] = 0x1f00b000,
	[ENGBASE_MDP_TDSHP0] = 0x1f00c000,
	[ENGBASE_MDP_TDSHP1] = 0x1f00d000,
	[ENGBASE_MDP_COLOR0] = 0x1f00e000,
	[ENGBASE_MMSYS_MUTEX] = 0x1f001000,
	[ENGBASE_IMGSYS] = 0x15020000,
	[ENGBASE_ISP_DIP_A0] = 0x15021000,
	[ENGBASE_ISP_DIP_A1] = 0x15022000,
	[ENGBASE_ISP_DIP_A7] = 0x15028000,
	[ENGBASE_ISP_MSFDL] = 0x15810000,
	[ENGBASE_ISP_MSS_B] = 0x15812000,
	[ENGBASE_ISP_MFB_B] = 0x15820000,
	[ENGBASE_ISP_WPE_A] = 0x15011000,
	[ENGBASE_ISP_WPE_B] = 0x15811000,
};

/*
 * Engine-flag bits whose registers live in imgsys (ISP_IMGI..CAMIN2, DPE..,
 * ISP secure ports): those need the DIP driver's power/clock reference.
 */
#define MDP_ISP_ENG_MASK	(GENMASK_ULL(7, 0) | GENMASK_ULL(34, 20))

/* ---- driver state ---- */

#define MDP_MAX_CHAN		4
#define MDP_MAX_BUFS		10
#define MDP_MAX_JOB_SLOTS	16
#define MDP_MAX_METAS		8192
#define MDP_MAX_READ_V1		4096
#define MDP_MAX_JOBS		64
#define MDP_INSTR_PER_META	4

#define SLOT_ID_SHIFT		16
#define SLOT_OFFSET_MASK	0xffff
#define MDP_MAX_SLOTS		1024
#define MAX_COUNT_IN_RB_SLOT	0x1000

static unsigned int timeout_ms = 2000;
module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "GCE job timeout before the thread is aborted");

struct mdp_chan {
	struct cmdq_client *clt;
	u8 shift_pa;
	u64 engines;	/* union of engine flags of unreaped jobs */
	u32 jobs;
};

struct mdp_buf {
	int fd;
	struct dma_buf *buf;
	struct dma_buf_attachment *att;
	struct sg_table *sgt;
	dma_addr_t iova;
};

struct mdp_job {
	struct list_head node;
	u64 id;
	struct file *owner;
	int ch;
	u64 engines;
	bool submitted;
	bool waiting;
	bool dip_held;
	int status;
	struct completion done;
	struct cmdq_pkt pkt;
	int nbufs;
	struct mdp_buf bufs[MDP_MAX_BUFS];
	u32 read_count;
	u32 *read_va;
	dma_addr_t read_pa;
	int nslots;
	u16 slots[MDP_MAX_JOB_SLOTS];
};

struct mdp_slot {
	void *va;
	dma_addr_t pa;
	u32 count;
	struct file *owner;
	u32 refs;
	bool free_pending;
};

struct mdp_compat {
	struct device *dev;
	struct device *gce_dev;
	struct mdp_chan ch[MDP_MAX_CHAN];
	int nch;
	struct clk_bulk_data *clks;
	int nclks;
	struct mutex lock;
	struct list_head jobs;
	u32 njobs;
	u64 next_id;
	u32 power_users;
	struct mdp_slot *slots;
	struct cmdqDTSDataStruct *dts;
	struct miscdevice misc;
	struct miscdevice misc_limit;
};

static struct mdp_compat *mdpc;

/* ---- channels ---- */

static void mdp_cmdq_cb(struct mbox_client *cl, void *mssg)
{
	struct cmdq_cb_data *data = mssg;
	struct mdp_job *job = container_of(data->pkt, struct mdp_job, pkt);

	job->status = data->sta;
	complete(&job->done);
}

static int mdp_chan_open(struct mdp_compat *mc, int i)
{
	struct cmdq_client *clt = cmdq_mbox_create(mc->dev, i);

	if (IS_ERR(clt)) {
		mc->ch[i].clt = NULL;
		return PTR_ERR(clt);
	}
	clt->client.rx_callback = mdp_cmdq_cb;
	mc->ch[i].clt = clt;
	mc->ch[i].shift_pa = cmdq_get_shift_pa(clt->chan);
	return 0;
}

/*
 * Stop a GCE thread: freeing the channel suspends the thread and completes
 * every queued packet with -ECONNABORTED. Called with mc->lock held.
 */
static void mdp_chan_abort(struct mdp_compat *mc, int i)
{
	int ret;

	if (!mc->ch[i].clt)
		return;
	dev_err(mc->dev, "aborting GCE channel %d\n", i);
	cmdq_mbox_destroy(mc->ch[i].clt);
	mc->ch[i].clt = NULL;
	ret = mdp_chan_open(mc, i);
	if (ret)
		dev_err(mc->dev, "channel %d lost: %d\n", i, ret);
}

static int mdp_chan_pick(struct mdp_compat *mc, u64 engines)
{
	int i, best = -1;

	for (i = 0; i < mc->nch; i++)
		if (mc->ch[i].clt && mc->ch[i].jobs &&
		    (mc->ch[i].engines & engines))
			return i;
	for (i = 0; i < mc->nch; i++) {
		if (!mc->ch[i].clt)
			continue;
		if (best < 0 || mc->ch[i].jobs < mc->ch[best].jobs)
			best = i;
	}
	return best;
}

/* ---- power ---- */

static int mdp_power_get(struct mdp_compat *mc)
{
	int ret;

	if (mc->power_users++)
		return 0;
	ret = pm_runtime_resume_and_get(mc->dev);
	if (ret)
		goto err;
	ret = clk_bulk_prepare_enable(mc->nclks, mc->clks);
	if (ret) {
		pm_runtime_put_sync(mc->dev);
		goto err;
	}
	return 0;
err:
	mc->power_users--;
	dev_err(mc->dev, "power on failed: %d\n", ret);
	return ret;
}

static void mdp_power_put(struct mdp_compat *mc)
{
	if (WARN_ON(!mc->power_users) || --mc->power_users)
		return;
	clk_bulk_disable_unprepare(mc->nclks, mc->clks);
	pm_runtime_put_sync(mc->dev);
}

/* ---- jobs ---- */

static void mdp_pkt_free(struct mdp_compat *mc, struct cmdq_pkt *pkt)
{
	if (!pkt->va_base)
		return;
	dma_unmap_single(mc->gce_dev, pkt->pa_base, pkt->buf_size,
			 DMA_TO_DEVICE);
	kfree(pkt->va_base);
	pkt->va_base = NULL;
}

static void mdp_slot_release(struct mdp_compat *mc, u32 id)
{
	struct mdp_slot *s = &mc->slots[id];

	if (s->va)
		dma_free_coherent(mc->gce_dev, s->count * sizeof(u32), s->va,
				  s->pa);
	memset(s, 0, sizeof(*s));
}

/* Called with mc->lock held, the job must be off the hardware. */
static void mdp_job_free(struct mdp_compat *mc, struct mdp_job *job)
{
	int i;

	for (i = 0; i < job->nbufs; i++) {
		struct mdp_buf *b = &job->bufs[i];

		dma_buf_unmap_attachment_unlocked(b->att, b->sgt,
						  DMA_BIDIRECTIONAL);
		dma_buf_detach(b->buf, b->att);
		dma_buf_put(b->buf);
	}
	for (i = 0; i < job->nslots; i++) {
		struct mdp_slot *s = &mc->slots[job->slots[i]];

		if (s->refs && !--s->refs && s->free_pending)
			mdp_slot_release(mc, job->slots[i]);
	}
	if (job->read_va)
		dma_free_coherent(mc->gce_dev, job->read_count * sizeof(u32),
				  job->read_va, job->read_pa);
	mdp_pkt_free(mc, &job->pkt);
	if (job->submitted) {
		mc->ch[job->ch].jobs--;
		if (!mc->ch[job->ch].jobs)
			mc->ch[job->ch].engines = 0;
		if (job->dip_held)
			mtk_cam_dip_release();
		mdp_power_put(mc);
	}
	if (!list_empty(&job->node)) {
		list_del(&job->node);
		mc->njobs--;
	}
	kfree(job);
}

static dma_addr_t mdp_job_fd_iova(struct mdp_compat *mc, struct mdp_job *job,
				  int fd)
{
	struct mdp_buf *b;
	int i;

	for (i = 0; i < job->nbufs; i++)
		if (job->bufs[i].fd == fd)
			return job->bufs[i].iova;
	if (job->nbufs >= MDP_MAX_BUFS)
		return 0;

	b = &job->bufs[job->nbufs];
	b->buf = dma_buf_get(fd);
	if (IS_ERR(b->buf))
		return 0;
	b->att = dma_buf_attach(b->buf, mc->dev);
	if (IS_ERR(b->att))
		goto put;
	b->sgt = dma_buf_map_attachment_unlocked(b->att, DMA_BIDIRECTIONAL);
	if (IS_ERR(b->sgt))
		goto detach;
	b->fd = fd;
	b->iova = sg_dma_address(b->sgt->sgl);
	job->nbufs++;
	return b->iova;
detach:
	dma_buf_detach(b->buf, b->att);
put:
	dma_buf_put(b->buf);
	return 0;
}

static u32 mdp_reg(u16 engine, u16 offset)
{
	if (engine >= ENGBASE_COUNT || offset > 0x1000)
		return 0;
	return mdp_base[engine] + (offset & ~0x3);
}

static bool mdp_reg_is_isp(u16 engine)
{
	return engine >= ENGBASE_IMGSYS;
}

/* imgsys registers are only reachable while the DIP driver powers them */
static int mdp_job_isp_ref(struct mdp_job *job, u16 engine)
{
	int ret;

	if (!mdp_reg_is_isp(engine) || job->dip_held)
		return 0;
	ret = mtk_cam_dip_hold();
	if (ret) {
		pr_err_ratelimited(MDP_TAG "imgsys access but DIP unavailable (mtk_cam.dip=1?): %d\n",
				   ret);
		return -EPERM;
	}
	job->dip_held = true;
	return 0;
}

static int mdp_event(struct mdp_compat *mc, u16 event)
{
	s32 v;

	if (event >= CMDQ_SYNC_TOKEN_MAX)
		return -EINVAL;
	v = mc->dts->eventTable[event];
	if (v < 0 || v >= CMDQ_MAX_EVENT)
		return -EINVAL;
	return v;
}

static int mdp_write(struct cmdq_pkt *pkt, u32 addr, u32 value, u32 mask)
{
	int ret = cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX0, CMDQ_ADDR_HIGH(addr));

	if (ret)
		return ret;
	if (mask == GENMASK(31, 0))
		return cmdq_pkt_write_s_value(pkt, CMDQ_THR_SPR_IDX0,
					      CMDQ_ADDR_LOW(addr), value);
	return cmdq_pkt_write_s_mask_value(pkt, CMDQ_THR_SPR_IDX0,
					   CMDQ_ADDR_LOW(addr), value, mask);
}

static dma_addr_t mdp_slot_addr(struct mdp_compat *mc, u32 read_id)
{
	u32 id = read_id >> SLOT_ID_SHIFT, off = read_id & SLOT_OFFSET_MASK;

	if (id >= MDP_MAX_SLOTS || !mc->slots[id].va ||
	    mc->slots[id].free_pending || off >= mc->slots[id].count)
		return 0;
	return mc->slots[id].pa + off * sizeof(u32);
}

static int mdp_job_ref_slot(struct mdp_compat *mc, struct mdp_job *job,
			    u32 read_id)
{
	u16 id = read_id >> SLOT_ID_SHIFT;
	int i;

	for (i = 0; i < job->nslots; i++)
		if (job->slots[i] == id)
			return 0;
	if (job->nslots >= MDP_MAX_JOB_SLOTS)
		return -ENOSPC;
	job->slots[job->nslots++] = id;
	mc->slots[id].refs++;
	return 0;
}

/* Called with mc->lock held. */
static int mdp_translate(struct mdp_compat *mc, struct mdp_job *job,
			 const struct op_meta *m)
{
	struct cmdq_pkt *pkt = &job->pkt;
	u32 reg = 0, from;
	dma_addr_t addr;
	int ev, ret;

	switch (m->op) {
	case CMDQ_MOP_WRITE:
	case CMDQ_MOP_WRITE_FD:
	case CMDQ_MOP_READ:
	case CMDQ_MOP_POLL:
	case CMDQ_MOP_WRITE_FROM_REG:
		reg = mdp_reg(m->engine, m->offset);
		if (!reg)
			return -EINVAL;
		ret = mdp_job_isp_ref(job, m->engine);
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	switch (m->op) {
	case CMDQ_MOP_WRITE:
		return mdp_write(pkt, reg, m->value, m->mask);
	case CMDQ_MOP_WRITE_FD:
		addr = mdp_job_fd_iova(mc, job, m->fd);
		if (!addr)
			return -EINVAL;
		return mdp_write(pkt, reg, (u32)(addr + m->fd_offset),
				 GENMASK(31, 0));
	case CMDQ_MOP_READ:
		addr = mdp_slot_addr(mc, m->readback_id);
		if (!addr)
			return -EINVAL;
		ret = mdp_job_ref_slot(mc, job, m->readback_id);
		if (ret)
			return ret;
		return cmdq_pkt_mem_move(pkt, reg, addr);
	case CMDQ_MOP_POLL:
		return cmdq_pkt_poll_addr(pkt, reg, m->value, m->mask);
	case CMDQ_MOP_WRITE_FROM_REG:
		from = mdp_reg(m->from_engine, m->from_offset);
		if (!from)
			return -EINVAL;
		ret = mdp_job_isp_ref(job, m->from_engine);
		if (ret)
			return ret;
		return cmdq_pkt_mem_move(pkt, from, reg);
	case CMDQ_MOP_WAIT:
	case CMDQ_MOP_WAIT_NO_CLEAR:
	case CMDQ_MOP_CLEAR:
	case CMDQ_MOP_SET:
	case CMDQ_MOP_ACQUIRE:
		ev = mdp_event(mc, m->event);
		if (ev < 0)
			return ev;
		if (m->op == CMDQ_MOP_WAIT)
			return cmdq_pkt_wfe(pkt, ev, true);
		if (m->op == CMDQ_MOP_WAIT_NO_CLEAR)
			return cmdq_pkt_wfe(pkt, ev, false);
		if (m->op == CMDQ_MOP_CLEAR)
			return cmdq_pkt_clear_event(pkt, ev);
		if (m->op == CMDQ_MOP_SET)
			return cmdq_pkt_set_event(pkt, ev);
		return cmdq_pkt_acquire_event(pkt, ev);
	case CMDQ_MOP_READBACK:
		/* AAL/HDR histogram readback: PQ is off, leave the slot as is */
		pr_info_once(MDP_TAG "READBACK meta ignored (PQ histograms)\n");
		return 0;
	case CMDQ_MOP_NOP:
		return 0;
	default:
		pr_err_ratelimited(MDP_TAG "unsupported meta op %u\n", m->op);
		return -EINVAL;
	}
}

static int mdp_build(struct mdp_compat *mc, struct mdp_job *job,
		     const struct mdp_submit *s)
{
	const u32 per_page = PAGE_SIZE / sizeof(struct op_meta);
	const u8 __user *src = u64_to_user_ptr(s->metas);
	struct op_meta *metas;
	u32 done = 0, i, n;
	int ret = 0;

	metas = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!metas)
		return -ENOMEM;

	while (done < s->meta_count) {
		n = min_t(u32, s->meta_count - done, per_page);
		if (copy_from_user(metas, src + done * sizeof(*metas),
				   n * sizeof(*metas))) {
			ret = -EFAULT;
			break;
		}
		for (i = 0; i < n; i++) {
			ret = mdp_translate(mc, job, &metas[i]);
			if (ret) {
				pr_err_ratelimited(MDP_TAG "meta[%u] op %u eng %u off %#x: %d\n",
						   done + i, metas[i].op,
						   metas[i].engine,
						   metas[i].offset, ret);
				goto out;
			}
		}
		done += n;
	}

	if (job->read_count) {
		struct hw_meta *hm;

		hm = kcalloc(job->read_count, sizeof(*hm), GFP_KERNEL);
		if (!hm) {
			ret = -ENOMEM;
			goto out;
		}
		if (copy_from_user(hm, u64_to_user_ptr(s->hw_metas_read_v1),
				   job->read_count * sizeof(*hm))) {
			kfree(hm);
			ret = -EFAULT;
			goto out;
		}
		for (i = 0; i < job->read_count && !ret; i++) {
			u32 reg = mdp_reg(hm[i].engine, hm[i].offset);

			ret = reg ? mdp_job_isp_ref(job, hm[i].engine) : -EINVAL;
			if (!ret)
				ret = cmdq_pkt_mem_move(&job->pkt, reg,
						job->read_pa + i * sizeof(u32));
		}
		kfree(hm);
		if (ret)
			goto out;
	}

	ret = cmdq_pkt_eoc(&job->pkt);
	if (!ret)
		ret = cmdq_pkt_jump_rel(&job->pkt, CMDQ_INST_SIZE,
					mc->ch[job->ch].shift_pa);
out:
	kfree(metas);
	return ret;
}

static int mdp_ioctl_exec(struct mdp_compat *mc, struct file *f,
			  void __user *arg)
{
	struct mdp_submit s;
	struct mdp_job *job;
	size_t size;
	int ret;

	if (copy_from_user(&s, arg, sizeof(s)))
		return -EFAULT;
	if (s.secData.is_secure)
		return -EOPNOTSUPP;
	if (!s.meta_count || s.meta_count > MDP_MAX_METAS ||
	    s.read_count_v1 > MDP_MAX_READ_V1)
		return -EINVAL;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	INIT_LIST_HEAD(&job->node);
	init_completion(&job->done);
	job->owner = f;
	job->engines = s.engine_flag & ~BIT_ULL(CMDQ_ENG_INORDER);
	job->read_count = s.read_count_v1;

	mutex_lock(&mc->lock);
	if (mc->njobs >= MDP_MAX_JOBS) {
		ret = -EBUSY;
		goto err;
	}
	job->ch = mdp_chan_pick(mc, job->engines);
	if (job->ch < 0) {
		ret = -ENODEV;
		goto err;
	}

	size = ((size_t)s.meta_count * MDP_INSTR_PER_META +
		(size_t)s.read_count_v1 * MDP_INSTR_PER_META + 8) *
		CMDQ_INST_SIZE;
	ret = cmdq_pkt_create(mc->ch[job->ch].clt, &job->pkt, size);
	if (ret) {
		job->pkt.va_base = NULL;
		goto err;
	}
	if (job->read_count) {
		job->read_va = dma_alloc_coherent(mc->gce_dev,
				job->read_count * sizeof(u32), &job->read_pa,
				GFP_KERNEL);
		if (!job->read_va) {
			ret = -ENOMEM;
			goto err;
		}
	}

	ret = mdp_power_get(mc);
	if (ret)
		goto err;
	/* from here mdp_job_free() drops power, DIP and channel accounting */
	job->submitted = true;
	mc->ch[job->ch].jobs++;
	mc->ch[job->ch].engines |= job->engines;

	if (job->engines & MDP_ISP_ENG_MASK) {
		ret = mdp_job_isp_ref(job, ENGBASE_IMGSYS);
		if (ret)
			goto err;
	}
	ret = mdp_build(mc, job, &s);
	if (ret)
		goto err;

	job->id = ++mc->next_id;
	list_add_tail(&job->node, &mc->jobs);
	mc->njobs++;

	dma_sync_single_for_device(mc->gce_dev, job->pkt.pa_base,
				   job->pkt.cmd_buf_size, DMA_TO_DEVICE);
	ret = mbox_send_message(mc->ch[job->ch].clt->chan, &job->pkt);
	if (ret < 0) {
		dev_err(mc->dev, "mbox_send_message: %d\n", ret);
		goto err;
	}
	mbox_client_txdone(mc->ch[job->ch].clt->chan, 0);
	mutex_unlock(&mc->lock);

	s.job_id = job->id;
	if (copy_to_user(arg, &s, sizeof(s)))
		return -EFAULT;	/* the job stays queued until close */
	return 0;
err:
	mdp_job_free(mc, job);
	mutex_unlock(&mc->lock);
	return ret;
}

/* Wait for one job (bounded); aborts its GCE thread on timeout. */
static int mdp_job_wait(struct mdp_compat *mc, struct mdp_job *job,
			bool interruptible)
{
	unsigned long t = msecs_to_jiffies(timeout_ms);
	long left;

	if (interruptible)
		left = wait_for_completion_interruptible_timeout(&job->done, t);
	else
		left = wait_for_completion_timeout(&job->done, t);
	if (left < 0)
		return left;
	if (left)
		return job->status ? -EIO : 0;

	mutex_lock(&mc->lock);
	if (!completion_done(&job->done))
		mdp_chan_abort(mc, job->ch);
	mutex_unlock(&mc->lock);
	if (!wait_for_completion_timeout(&job->done, HZ))
		dev_err(mc->dev, "job %llu not released by GCE abort\n",
			job->id);
	return -ETIMEDOUT;
}

static int mdp_read_slots(struct mdp_compat *mc, struct mdp_read_readback *r)
{
	u32 *ids, *vals, i;
	int ret = 0;

	if (!r->count)
		return 0;
	if (r->count > MDP_MAX_READ_V1 || !r->ids || !r->ret_values)
		return -EINVAL;
	ids = kcalloc(r->count, sizeof(u32), GFP_KERNEL);
	vals = kcalloc(r->count, sizeof(u32), GFP_KERNEL);
	if (!ids || !vals) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(ids, u64_to_user_ptr(r->ids),
			   r->count * sizeof(u32))) {
		ret = -EFAULT;
		goto out;
	}
	mutex_lock(&mc->lock);
	for (i = 0; i < r->count; i++) {
		u32 id = ids[i] >> SLOT_ID_SHIFT, off = ids[i] & SLOT_OFFSET_MASK;
		struct mdp_slot *s = id < MDP_MAX_SLOTS ? &mc->slots[id] : NULL;

		if (s && s->va && off < s->count)
			vals[i] = READ_ONCE(((u32 *)s->va)[off]);
	}
	mutex_unlock(&mc->lock);
	if (copy_to_user(u64_to_user_ptr(r->ret_values), vals,
			 r->count * sizeof(u32)))
		ret = -EFAULT;
out:
	kfree(ids);
	kfree(vals);
	return ret;
}

static int mdp_ioctl_wait(struct mdp_compat *mc, struct file *f,
			  void __user *arg)
{
	struct mdp_wait w;
	struct mdp_job *job = NULL, *it;
	int ret;

	if (copy_from_user(&w, arg, sizeof(w)))
		return -EFAULT;

	mutex_lock(&mc->lock);
	list_for_each_entry(it, &mc->jobs, node) {
		if (it->id == w.job_id && it->owner == f && !it->waiting) {
			job = it;
			job->waiting = true;
			break;
		}
	}
	mutex_unlock(&mc->lock);
	if (!job)
		return -EFAULT;

	ret = mdp_job_wait(mc, job, true);
	if (ret == -ERESTARTSYS) {
		mutex_lock(&mc->lock);
		job->waiting = false;
		mutex_unlock(&mc->lock);
		return ret;
	}
	if (ret)
		dev_err(mc->dev, "job %llu (engines %#llx) failed: %d\n",
			job->id, job->engines, ret);

	if (!ret && w.read_v1_result.count != job->read_count)
		ret = -ENOMEM;
	if (!ret && job->read_count && w.read_v1_result.ret_values &&
	    copy_to_user(u64_to_user_ptr(w.read_v1_result.ret_values),
			 job->read_va, job->read_count * sizeof(u32)))
		ret = -EFAULT;
	if (!ret)
		ret = mdp_read_slots(mc, &w.read_result);

	mutex_lock(&mc->lock);
	mdp_job_free(mc, job);
	mutex_unlock(&mc->lock);
	return ret;
}

static int mdp_ioctl_alloc_slot(struct mdp_compat *mc, struct file *f,
				void __user *arg)
{
	struct mdp_readback rb;
	struct mdp_slot *s;
	u32 i;

	if (copy_from_user(&rb, arg, sizeof(rb)))
		return -EFAULT;
	if (!rb.count || rb.count > MAX_COUNT_IN_RB_SLOT)
		return -EINVAL;

	mutex_lock(&mc->lock);
	for (i = 0; i < MDP_MAX_SLOTS; i++)
		if (!mc->slots[i].va)
			break;
	if (i == MDP_MAX_SLOTS) {
		mutex_unlock(&mc->lock);
		return -ENOMEM;
	}
	s = &mc->slots[i];
	s->va = dma_alloc_coherent(mc->gce_dev, rb.count * sizeof(u32), &s->pa,
				   GFP_KERNEL);
	if (!s->va) {
		mutex_unlock(&mc->lock);
		return -ENOMEM;
	}
	s->count = rb.count;
	s->owner = f;
	mutex_unlock(&mc->lock);

	rb.start_id = i << SLOT_ID_SHIFT;
	if (copy_to_user(arg, &rb, sizeof(rb)))
		return -EFAULT;
	return 0;
}

/* Called with mc->lock held. */
static void mdp_slot_free(struct mdp_compat *mc, u32 id)
{
	struct mdp_slot *s = &mc->slots[id];

	if (s->refs)
		s->free_pending = true;
	else
		mdp_slot_release(mc, id);
}

static int mdp_ioctl_free_slot(struct mdp_compat *mc, struct file *f,
			       void __user *arg)
{
	struct mdp_readback rb;
	u32 id;
	int ret = -EINVAL;

	if (copy_from_user(&rb, arg, sizeof(rb)))
		return -EFAULT;
	id = rb.start_id >> SLOT_ID_SHIFT;
	if (id >= MDP_MAX_SLOTS)
		return -EINVAL;
	mutex_lock(&mc->lock);
	if (mc->slots[id].va && !mc->slots[id].free_pending &&
	    mc->slots[id].owner == f) {
		mdp_slot_free(mc, id);
		ret = 0;
	}
	mutex_unlock(&mc->lock);
	return ret;
}

static long mdp_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct mdp_compat *mc = mdpc;
	void __user *uarg = (void __user *)arg;
	struct mdp_read_readback r;
	int cap = BIT(CMDQ_CAP_WFE);

	switch (cmd) {
	case CMDQ_IOCTL_QUERY_USAGE: {
		struct cmdqUsageInfoStruct u = {};

		return copy_to_user(uarg, &u, sizeof(u)) ? -EFAULT : 0;
	}
	case CMDQ_IOCTL_QUERY_CAP_BITS:
		return copy_to_user(uarg, &cap, sizeof(cap)) ? -EFAULT : 0;
	case CMDQ_IOCTL_QUERY_DTS:
		return copy_to_user(uarg, mc->dts, sizeof(*mc->dts)) ?
			-EFAULT : 0;
	case CMDQ_IOCTL_NOTIFY_ENGINE:
		return 0;
	case CMDQ_IOCTL_ASYNC_EXEC:
		return mdp_ioctl_exec(mc, f, uarg);
	case CMDQ_IOCTL_ASYNC_WAIT:
		return mdp_ioctl_wait(mc, f, uarg);
	case CMDQ_IOCTL_ALLOC_READBACK_SLOTS:
		return mdp_ioctl_alloc_slot(mc, f, uarg);
	case CMDQ_IOCTL_FREE_READBACK_SLOTS:
		return mdp_ioctl_free_slot(mc, f, uarg);
	case CMDQ_IOCTL_READ_READBACK_SLOTS:
		if (copy_from_user(&r, uarg, sizeof(r)))
			return -EFAULT;
		return mdp_read_slots(mc, &r);
	default:
		pr_info_ratelimited(MDP_TAG "unsupported ioctl %#x\n", cmd);
		return -ENOTTY;
	}
}

static int mdp_release(struct inode *inode, struct file *f)
{
	struct mdp_compat *mc = mdpc;
	struct mdp_job *job;
	u32 i;

	for (;;) {
		mutex_lock(&mc->lock);
		job = NULL;
		list_for_each_entry(job, &mc->jobs, node)
			if (job->owner == f && !job->waiting)
				break;
		if (list_entry_is_head(job, &mc->jobs, node))
			job = NULL;
		if (job)
			job->waiting = true;
		mutex_unlock(&mc->lock);
		if (!job)
			break;
		mdp_job_wait(mc, job, false);
		mutex_lock(&mc->lock);
		mdp_job_free(mc, job);
		mutex_unlock(&mc->lock);
	}

	mutex_lock(&mc->lock);
	for (i = 0; i < MDP_MAX_SLOTS; i++)
		if (mc->slots[i].va && mc->slots[i].owner == f)
			mdp_slot_free(mc, i);
	mutex_unlock(&mc->lock);
	return 0;
}

static const struct file_operations mdp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mdp_ioctl,
	.release = mdp_release,
};

/* ---- probe ---- */

static void mdp_init_dts(struct mdp_compat *mc, struct device_node *evnp)
{
	struct cmdqDTSDataStruct *d = mc->dts;
	u32 i, v, sub[3];

	for (i = 0; i < CMDQ_SYNC_TOKEN_MAX; i++)
		d->eventTable[i] = i <= CMDQ_MAX_HW_EVENT_COUNT ?
			CMDQ_SYNC_TOKEN_INVALID - 1 - i : i;
	for (i = 0; i < ARRAY_SIZE(mdp_event_names); i++) {
		u16 ev = mdp_event_names[i].event;

		if (ev >= CMDQ_MAX_HW_EVENT_COUNT)
			continue;
		if (!of_property_read_u32(evnp, mdp_event_names[i].dts_name, &v))
			d->eventTable[ev] = v;
	}
	for (i = 0; i < CMDQ_SUBSYS_MAX_COUNT; i++) {
		d->subsys[i].subsysID = -1;
		if (!mdp_subsys_names[i].name ||
		    of_property_read_u32_array(evnp, mdp_subsys_names[i].name,
					       sub, 3))
			continue;
		d->subsys[i].msb = sub[0] & sub[2];
		d->subsys[i].subsysID = sub[1];
		d->subsys[i].mask = sub[2];
		strscpy(d->subsys[i].grpName, mdp_subsys_names[i].group,
			sizeof(d->subsys[i].grpName));
	}
	d->MDPBaseAddress[CMDQ_MDP_PA_BASE_MM_MUTEX] =
		mdp_base[ENGBASE_MMSYS_MUTEX];
}

/*
 * The 4.14 GCE driver set the "default_tokens" (GPR/resource locks) at boot;
 * user space acquires/waits on them. Doing it with a packet also proves the
 * GCE thread actually runs before /dev/mtk_mdp is exposed.
 */
static int mdp_init_tokens(struct mdp_compat *mc)
{
	struct device_node *gce;
	struct mdp_job *job;
	u16 tok[16];
	int n, i, ret;

	gce = of_parse_phandle(mc->dev->of_node, "mboxes", 0);
	if (!gce)
		return -ENODEV;
	n = of_property_count_u16_elems(gce, "default_tokens");
	if (n <= 0 || n > ARRAY_SIZE(tok) ||
	    of_property_read_u16_array(gce, "default_tokens", tok, n))
		n = 0;
	of_node_put(gce);

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	INIT_LIST_HEAD(&job->node);
	init_completion(&job->done);
	job->ch = 0;

	mutex_lock(&mc->lock);
	ret = cmdq_pkt_create(mc->ch[0].clt, &job->pkt, 64 * CMDQ_INST_SIZE);
	if (ret) {
		job->pkt.va_base = NULL;
		goto out;
	}
	for (i = 0; i < n && !ret; i++)
		ret = cmdq_pkt_set_event(&job->pkt, tok[i]);
	if (!ret)
		ret = cmdq_pkt_eoc(&job->pkt);
	if (!ret)
		ret = cmdq_pkt_jump_rel(&job->pkt, CMDQ_INST_SIZE,
					mc->ch[0].shift_pa);
	if (ret)
		goto out;
	dma_sync_single_for_device(mc->gce_dev, job->pkt.pa_base,
				   job->pkt.cmd_buf_size, DMA_TO_DEVICE);
	ret = mbox_send_message(mc->ch[0].clt->chan, &job->pkt);
	if (ret < 0)
		goto out;
	mbox_client_txdone(mc->ch[0].clt->chan, 0);
	mutex_unlock(&mc->lock);

	if (!wait_for_completion_timeout(&job->done, msecs_to_jiffies(500))) {
		mutex_lock(&mc->lock);
		mdp_chan_abort(mc, 0);
		wait_for_completion_timeout(&job->done, HZ);
		ret = -ETIMEDOUT;
	} else {
		mutex_lock(&mc->lock);
		ret = job->status;
	}
	dev_info(mc->dev, "GCE self-test (%d default tokens): %d\n", n, ret);
out:
	mdp_job_free(mc, job);
	mutex_unlock(&mc->lock);
	return ret;
}

static int mdp_compat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *evnp;
	struct mdp_compat *mc;
	int i, ret;

	if (!mtk_cam_mdp_enabled())
		return -ENODEV;

	mc = devm_kzalloc(dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;
	mc->dev = dev;
	mutex_init(&mc->lock);
	INIT_LIST_HEAD(&mc->jobs);

	mc->dts = devm_kzalloc(dev, sizeof(*mc->dts), GFP_KERNEL);
	mc->slots = devm_kcalloc(dev, MDP_MAX_SLOTS, sizeof(*mc->slots),
				 GFP_KERNEL);
	if (!mc->dts || !mc->slots)
		return -ENOMEM;

	evnp = of_parse_phandle(dev->of_node, "mediatek,event-node", 0);
	if (!evnp)
		return dev_err_probe(dev, -EINVAL, "no mediatek,event-node\n");
	mdp_init_dts(mc, evnp);
	of_node_put(evnp);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;

	mc->nclks = devm_clk_bulk_get_all(dev, &mc->clks);
	if (mc->nclks < 0)
		return dev_err_probe(dev, mc->nclks, "clocks\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	mc->nch = of_count_phandle_with_args(dev->of_node, "mboxes",
					     "#mbox-cells");
	if (mc->nch <= 0)
		return dev_err_probe(dev, -EINVAL, "no mboxes\n");
	mc->nch = min(mc->nch, MDP_MAX_CHAN);
	for (i = 0; i < mc->nch; i++) {
		ret = mdp_chan_open(mc, i);
		if (ret) {
			dev_err_probe(dev, ret, "GCE channel %d\n", i);
			goto err_chan;
		}
	}
	mc->gce_dev = mc->ch[0].clt->chan->mbox->dev;

	ret = mdp_init_tokens(mc);
	if (ret) {
		dev_err(dev, "GCE does not execute packets (%d), /dev/mtk_mdp not created\n",
			ret);
		ret = -ENODEV;
		goto err_chan;
	}

	mdpc = mc;
	mc->misc.minor = MISC_DYNAMIC_MINOR;
	mc->misc.name = "mtk_mdp";
	mc->misc.fops = &mdp_fops;
	ret = misc_register(&mc->misc);
	if (ret)
		goto err_reg;
	mc->misc_limit.minor = MISC_DYNAMIC_MINOR;
	mc->misc_limit.name = "mtk_mdp_limit";
	mc->misc_limit.fops = &mdp_fops;
	if (misc_register(&mc->misc_limit))
		mc->misc_limit.name = NULL;

	dev_info(dev, "/dev/mtk_mdp ready (%d GCE threads, %d clocks)\n",
		 mc->nch, mc->nclks);
	return 0;

err_reg:
	mdpc = NULL;
err_chan:
	for (i = 0; i < mc->nch; i++)
		if (mc->ch[i].clt)
			cmdq_mbox_destroy(mc->ch[i].clt);
	return ret;
}

/* Nothing may stay on a GCE thread across system suspend. */
static int mdp_compat_suspend(struct device *dev)
{
	struct mdp_compat *mc = mdpc;
	int i;

	if (!mc)
		return 0;
	mutex_lock(&mc->lock);
	for (i = 0; i < mc->nch; i++)
		if (mc->ch[i].jobs)
			mdp_chan_abort(mc, i);
	mutex_unlock(&mc->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mdp_compat_pm, mdp_compat_suspend, NULL);

static const struct of_device_id mdp_compat_of_ids[] = {
	{ .compatible = "mediatek,mt6853-mdp-compat" },
	{}
};

static struct platform_driver mdp_compat_driver = {
	.probe = mdp_compat_probe,
	.driver = {
		.name = "mtk-mdp-compat",
		.of_match_table = mdp_compat_of_ids,
		.pm = pm_sleep_ptr(&mdp_compat_pm),
		.suppress_bind_attrs = true,
	},
};

static int __init mdp_compat_init(void)
{
	if (!mtk_cam_mdp_enabled()) {
		pr_info(MDP_TAG "disabled (boot with mtk_cam.hw=1 mtk_cam.mdp=1)\n");
		return 0;
	}
	return platform_driver_register(&mdp_compat_driver);
}
module_init(mdp_compat_init);

MODULE_IMPORT_NS("DMA_BUF");
