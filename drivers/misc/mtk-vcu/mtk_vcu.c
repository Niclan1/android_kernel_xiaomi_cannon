// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek VCU (Video Communication/Controller Unit) bridge.
 *
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Andrew-CT Chen <andrew-ct.chen@mediatek.com>
 *
 * Port of the MT6853 4.14 vendor driver (drivers/media/platform/mtk-vcu) to
 * Linux 6.18. It exposes /dev/vcu to the vendor video daemon (vpud), relays
 * IPI messages between codec drivers and the daemon, hands out DMA working
 * buffers, maps the codec register windows listed in the DT node and runs the
 * daemon's GCE command lists on the mainline mtk-cmdq mailbox.
 *
 * Dropped against the vendor driver: secure video path, MTK ION/M4U private
 * hooks, AEE reporting and the signal_generate tracepoint.
 *
 * The driver is inert unless booted with mtk_vcu.enable=1.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <media/videobuf2-core.h>

#include <linux/of_reserved_mem.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/clock.h>
#include <linux/io.h>
#include <linux/rwsem.h>

#include "mtk_vcu.h"
#include "mtk_vcu_controls.h"

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Create /dev/vcu for the vendor video daemon (default off)");

static bool gce = true;
module_param(gce, bool, 0644);
MODULE_PARM_DESC(gce, "Execute daemon GCE command lists (default on when enabled)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Verbose VCU logging");

static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "Log venc power, mmap, ioctl and IPI traffic at emerg level");

static bool venc_always_on;
module_param(venc_always_on, bool, 0644);
MODULE_PARM_DESC(venc_always_on, "Hold venc power, larb and clocks while vpud is up");

static bool stub_unknown_ioctl = true;
module_param(stub_unknown_ioctl, bool, 0644);
MODULE_PARM_DESC(stub_unknown_ioctl, "Return 0 for unknown /dev/vcu ioctls (mmdvfs/qos etc.) instead of -ENOTTY");

static bool vdec;
module_param(vdec, bool, 0644);
MODULE_PARM_DESC(vdec, "Allow vpud to use the VDEC (reg0 mmap, vdec GCE); off by default");

static bool enc_iommu_trace;
module_param(enc_iommu_trace, bool, 0644);
MODULE_PARM_DESC(enc_iommu_trace, "Log venc vb2 buffer alloc/attach/map with iova");

bool vcu_iommu_trace_on(void)
{
	return enc_iommu_trace;
}
EXPORT_SYMBOL_GPL(vcu_iommu_trace_on);

/*
 * Trace ring kept in RAM (/proc/venc_trace) and mirrored into a no-map
 * reserved region that is read back after a warm reset (/proc/venc_last).
 */
#define VT_LINES	64
#define VT_LEN		128
#define VT_MAGIC	0x56545243

struct vt_ring {
	u32 magic;
	u32 head;
	u32 count;
	u32 pad;
	char line[VT_LINES][VT_LEN];
};

static struct vt_ring vt_ram;
static struct vt_ring *vt_pmem;
static char *vt_last;
static size_t vt_last_len;
static DEFINE_SPINLOCK(vt_lock);

void vcu_trace_log(const char *fmt, ...)
{
	char buf[VT_LEN];
	unsigned long flags;
	u64 ts = local_clock();
	va_list ap;
	int n;
	u32 h;

	n = scnprintf(buf, sizeof(buf), "[%5llu.%06llu] ", ts / 1000000000ULL,
		      (ts % 1000000000ULL) / 1000);
	va_start(ap, fmt);
	vscnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	printk(KERN_EMERG "%s", buf + n);
	strim(buf);

	spin_lock_irqsave(&vt_lock, flags);
	h = vt_ram.head % VT_LINES;
	strscpy_pad(vt_ram.line[h], buf, VT_LEN);
	vt_ram.head = h + 1;
	if (vt_ram.count < VT_LINES)
		vt_ram.count++;
	if (vt_pmem) {
		memcpy(vt_pmem->line[h], vt_ram.line[h], VT_LEN);
		WRITE_ONCE(vt_pmem->head, vt_ram.head);
		WRITE_ONCE(vt_pmem->count, vt_ram.count);
		wmb();
	}
	spin_unlock_irqrestore(&vt_lock, flags);
}
EXPORT_SYMBOL_GPL(vcu_trace_log);

static void vt_dump(struct seq_file *m, const struct vt_ring *r)
{
	u32 i, cnt = min_t(u32, r->count, VT_LINES);
	u32 start = (r->head % VT_LINES + VT_LINES - cnt) % VT_LINES;

	for (i = 0; i < cnt; i++)
		seq_printf(m, "%.*s\n", VT_LEN, r->line[(start + i) % VT_LINES]);
}

static int vt_trace_show(struct seq_file *m, void *v)
{
	struct vt_ring *copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	unsigned long flags;

	if (!copy)
		return -ENOMEM;
	spin_lock_irqsave(&vt_lock, flags);
	memcpy(copy, &vt_ram, sizeof(*copy));
	spin_unlock_irqrestore(&vt_lock, flags);
	vt_dump(m, copy);
	kfree(copy);
	return 0;
}

static int vt_last_show(struct seq_file *m, void *v)
{
	if (vt_last)
		seq_write(m, vt_last, vt_last_len);
	else
		seq_puts(m, "no previous trace\n");
	return 0;
}

static void vt_init(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;
	struct vt_ring *r;

	proc_create_single("venc_trace", 0444, NULL, vt_trace_show);
	proc_create_single("venc_last", 0444, NULL, vt_last_show);

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	rmem = np ? of_reserved_mem_lookup(np) : NULL;
	of_node_put(np);
	if (!rmem || rmem->size < sizeof(*r)) {
		dev_info(dev, "[VCU] no persistent trace region\n");
		return;
	}
	r = memremap(rmem->base, sizeof(*r), MEMREMAP_WC);
	if (!r) {
		dev_info(dev, "[VCU] trace region map failed\n");
		return;
	}
	if (r->magic == VT_MAGIC && r->count && r->count <= VT_LINES) {
		struct vt_ring *old = kmalloc(sizeof(*old), GFP_KERNEL);
		u32 i, cnt, start;
		size_t len = 0, cap = VT_LINES * (VT_LEN + 1);

		vt_last = kmalloc(cap, GFP_KERNEL);
		if (old && vt_last) {
			memcpy_fromio(old, (void __iomem *)r, sizeof(*old));
			cnt = old->count;
			start = (old->head % VT_LINES + VT_LINES - cnt) % VT_LINES;
			for (i = 0; i < cnt; i++)
				len += scnprintf(vt_last + len, cap - len, "%.*s\n",
						 VT_LEN - 1,
						 old->line[(start + i) % VT_LINES]);
			vt_last_len = len;
			pr_emerg("[VCU] previous boot left %u trace lines in /proc/venc_last\n",
				 cnt);
		} else {
			kfree(vt_last);
			vt_last = NULL;
		}
		kfree(old);
	}
	memset(r, 0, sizeof(*r));
	spin_lock_irq(&vt_lock);
	memcpy(r->line, vt_ram.line, sizeof(r->line));
	r->head = vt_ram.head;
	r->count = vt_ram.count;
	r->magic = VT_MAGIC;
	wmb();
	vt_pmem = r;
	spin_unlock_irq(&vt_lock);
	dev_info(dev, "[VCU] persistent trace at %pa\n", &rmem->base);
}

#define vcu_trace(fmt, ...) do {					\
		if (trace)						\
			vcu_trace_log("[VCU] " fmt, ##__VA_ARGS__);		\
	} while (0)

bool vcu_trace_on(void)
{
	return trace;
}
EXPORT_SYMBOL_GPL(vcu_trace_on);

static u32 vcu_msg_id(const void *buf)
{
	u32 id;

	memcpy(&id, buf, sizeof(id));
	return id;
}

#define vcu_dbg(fmt, ...) do {						\
		if (debug)						\
			pr_info("[VCU] " fmt, ##__VA_ARGS__);		\
	} while (0)

#define IPI_TIMEOUT_MS		5000U
#define VCU_FW_VER_LEN		16
#define VCODEC_INST_MAX		64
#define GCE_EVENT_MAX		64
#define GCE_THNUM_MAX		2
#define VCU_GCE_CLEAR_TIMEOUT	msecs_to_jiffies(2000)
#define VCU_GCE_STALL_TIMEOUT	msecs_to_jiffies(3000)
#define VCU_MAP_HW_REG_NUM	4

#define VCU_SHMEM_SIZE		0x80000UL
#define MAP_PA_BASE_1GB		0x40000000UL
#define MAP_SHMEM_ALLOC_BASE	0x80000000UL
#define MAP_SHMEM_ALLOC_END	(MAP_SHMEM_ALLOC_BASE + VCU_SHMEM_SIZE)
#define MAP_SHMEM_COMMIT_BASE	0x88000000UL
#define MAP_SHMEM_COMMIT_END	(MAP_SHMEM_COMMIT_BASE + VCU_SHMEM_SIZE)
#define MAP_SHMEM_MM_BASE	0x200000000ULL
#define MAP_SHMEM_MM_CACHEABLE_BASE 0x400000000ULL
#define MAP_SHMEM_PA_BASE	0x800000000ULL

#define CODEC_MAX_BUFFER	512U
#define CODEC_ALLOCATE_MAX_BUFFER_SIZE 0x8000000UL
#define CODEC_MSK(addr)		(((u64)(addr) >> PAGE_SHIFT) & 0xFFFF)

/* worst case expansion of one daemon GCE command into GCE instructions */
#define GCE_INST_PER_CMD	4

struct vcu_run_msg {
	u32 signaled;
	char fw_ver[VCU_FW_VER_LEN];
	u32 dec_capability;
	u32 enc_capability;
};

struct vcu_run {
	u32 signaled;
	char fw_ver[VCU_FW_VER_LEN];
	unsigned int dec_capability;
	unsigned int enc_capability;
};

struct vcu_ipi_desc {
	ipi_handler_t handler;
	const char *name;
	void *priv;
};

struct map_hw_reg {
	phys_addr_t base;
	resource_size_t len;
};

struct gce_ctx_info {
	bool used;
	bool failed;
	unsigned int type;
	u64 generation;
	void *v4l2_ctx;
	u64 user_hdl;
	atomic_t flush_done;
	atomic_t flush_pending;
};

struct mtk_vcu {
	struct device *dev;
	struct miscdevice misc;
	struct vcu_run run;
	struct vcu_ipi_desc ipi_desc[IPI_MAX];
	const struct vcu_codec_ops *codec_ops;

	struct mutex vcu_mutex[VCU_CODEC_MAX];
	struct mutex vcu_gce_mutex[VCU_CODEC_MAX];
	struct mutex gce_cmds_mutex[VCU_CODEC_MAX];
	struct mutex ctx_ipi_binding[VCU_CODEC_MAX];
	struct mutex vcu_share;

	struct map_hw_reg map_base[VCU_MAP_HW_REG_NUM];
	int (*reg_get)(void *priv);
	void (*reg_put)(void *priv);
	void *reg_priv;
	bool venc_held;
	atomic_t reg_maps;
	wait_queue_head_t reg_map_wq;

	wait_queue_head_t ack_wq[VCU_CODEC_MAX];
	bool ipi_id_ack[IPI_MAX];
	wait_queue_head_t get_wq[VCU_CODEC_MAX];
	atomic_t ipi_got[VCU_CODEC_MAX];
	atomic_t ipi_done[VCU_CODEC_MAX];
	struct share_obj user_obj[VCU_CODEC_MAX];

	struct log_test_nofuse *vdec_log_info;
	wait_queue_head_t vdec_log_get_wq;
	atomic_t vdec_log_got;

	/* data extended memory shared with the daemon */
	void *dmem_va;
	unsigned long dmem_vma;

	struct cmdq_client *clt_vdec[GCE_THNUM_MAX];
	struct cmdq_client *clt_venc[GCE_THNUM_MAX];
	struct device *gce_dev;
	struct workqueue_struct *gce_wq_work;
	int gce_th_num[VCU_CODEC_MAX];
	int gce_codec_eid[GCE_EVENT_MAX];
	struct gce_cmds *gce_cmds[VCU_CODEC_MAX];
	wait_queue_head_t gce_wq[VCU_CODEC_MAX];
	struct rw_semaphore gce_chan_sem[VCU_CODEC_MAX];
	unsigned long lost_channels[VCU_CODEC_MAX];
	struct gce_ctx_info gce_info[VCODEC_INST_MAX];
	struct gce_ctx_info *venc_owner[GCE_THNUM_MAX];
	atomic_t gce_job_cnt[VCU_CODEC_MAX][GCE_THNUM_MAX];
	unsigned long gce_progress[VCU_CODEC_MAX][GCE_THNUM_MAX];
	struct delayed_work gce_wdt;
	unsigned long flags[VCU_CODEC_MAX];

	void *curr_ctx[VCU_CODEC_MAX];
	struct vb2_buffer *curr_src_vb[VCU_CODEC_MAX];
	struct vb2_buffer *curr_dst_vb[VCU_CODEC_MAX];

	struct task_struct *vcud_task;
	int daemon_pid;
	int open_cnt;
	bool abort;
	struct semaphore vpud_killed;
	int vpud_is_going_down;
};

struct vcu_buf {
	struct list_head node;
	void *kva;
	dma_addr_t iova;
	size_t size;
	struct dma_buf *dbuf;
	atomic_t ref_cnt;
};

struct vcu_pa_page {
	struct list_head node;
	void *kva;
	dma_addr_t pa;
	atomic_t ref_cnt;
};

struct vcu_queue {
	struct mtk_vcu *vcu;
	struct mutex lock;
	struct list_head bufs;
	unsigned int num_bufs;
	struct list_head pages;
	u64 map_buf_pa;
};

struct vcu_gce_job {
	struct cmdq_pkt pkt;
	struct cmdq_client *clt;
	struct gce_cmdq_obj obj;
	struct mtk_vcu *vcu;
	int inst;
	u64 generation;
	int sta;
	bool pm_ref;
	struct work_struct work;
	struct {
		u64 addr;
		u64 data;
		u32 mask;
		u8 cmd;
	} wait;
};

static struct mtk_vcu *vcu_ptr;

static const struct {
	int id;
	const char *name;
} vcu_gce_events[] = {
	{ VDEC_EVENT_0, "vdec_pic_start" },
	{ VDEC_EVENT_1, "vdec_decode_done" },
	{ VDEC_EVENT_2, "vdec_pause" },
	{ VDEC_EVENT_3, "vdec_dec_error" },
	{ VDEC_EVENT_4, "vdec_mc_busy_overflow_timeout" },
	{ VDEC_EVENT_5, "vdec_all_dram_req_done" },
	{ VDEC_EVENT_6, "vdec_ini_fetch_rdy" },
	{ VDEC_EVENT_7, "vdec_process_flag" },
	{ VDEC_EVENT_8, "vdec_search_start_code_done" },
	{ VDEC_EVENT_9, "vdec_ref_reorder_done" },
	{ VDEC_EVENT_10, "vdec_wp_tble_done" },
	{ VDEC_EVENT_11, "vdec_count_sram_clr_done" },
	{ VDEC_EVENT_15, "vdec_gce_cnt_op_threshold" },
	{ VDEC_LAT_EVENT_0, "vdec_lat_pic_start" },
	{ VDEC_LAT_EVENT_1, "vdec_lat_decode_done" },
	{ VDEC_LAT_EVENT_2, "vdec_lat_pause" },
	{ VDEC_LAT_EVENT_3, "vdec_lat_dec_error" },
	{ VDEC_LAT_EVENT_4, "vdec_lat_mc_busy_overflow_timeout" },
	{ VDEC_LAT_EVENT_5, "vdec_lat_all_dram_req_done" },
	{ VDEC_LAT_EVENT_6, "vdec_lat_ini_fetch_rdy" },
	{ VDEC_LAT_EVENT_7, "vdec_lat_process_flag" },
	{ VDEC_LAT_EVENT_8, "vdec_lat_search_start_code_done" },
	{ VDEC_LAT_EVENT_9, "vdec_lat_ref_reorder_done" },
	{ VDEC_LAT_EVENT_10, "vdec_lat_wp_tble_done" },
	{ VDEC_LAT_EVENT_11, "vdec_lat_count_sram_clr_done" },
	{ VDEC_LAT_EVENT_15, "vdec_lat_gce_cnt_op_threshold" },
	{ VENC_EOF, "venc_eof" },
	{ VENC_CMDQ_PAUSE_DONE, "venc_cmdq_pause_done" },
	{ VENC_MB_DONE, "venc_mb_done" },
	{ VENC_128BYTE_CNT_DONE, "venc_128B_cnt_done" },
	{ VENC_EOF_C1, "venc_eof_c1" },
	{ VENC_WP_2ND_DONE, "venc_wp_2nd_done" },
	{ VENC_WP_3ND_DONE, "venc_wp_3nd_done" },
	{ VENC_SPS_DONE, "venc_sps_done" },
	{ VENC_PPS_DONE, "venc_pps_done" },
};

static inline unsigned int ipi_id_to_inst_id(int id)
{
	/* VENC uses instance 1, everything else instance 0 */
	if (id < IPI_VENC_COMMON && id >= IPI_VCU_INIT)
		return VCU_VDEC;
	return VCU_VENC;
}

static inline bool vcu_running(struct mtk_vcu *vcu)
{
	return vcu->run.signaled != 0;
}

/* ------------------------------------------------------------------ */
/* IPI                                                                 */
/* ------------------------------------------------------------------ */

int vcu_ipi_register(struct platform_device *pdev, enum ipi_id id,
		     ipi_handler_t handler, const char *name, void *priv)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);
	unsigned int i;

	if (!vcu)
		return -EPROBE_DEFER;

	if (id < IPI_VCU_INIT || id >= IPI_MAX || !handler)
		return -EINVAL;

	i = ipi_id_to_inst_id(id);
	mutex_lock(&vcu->vcu_mutex[i]);
	vcu->ipi_desc[id].name = name;
	vcu->ipi_desc[id].handler = handler;
	vcu->ipi_desc[id].priv = priv;
	mutex_unlock(&vcu->vcu_mutex[i]);

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_ipi_register);

static void vcu_wait_vpud_killed(struct mtk_vcu *vcu)
{
	if (vcu->open_cnt > 0) {
		dev_info(vcu->dev, "wait for vpud killed\n");
		if (down_interruptible(&vcu->vpud_killed))
			dev_info(vcu->dev, "wait for vpud killed interrupted\n");
	}
}

int vcu_ipi_send(struct platform_device *pdev, enum ipi_id id, void *buf,
		 unsigned int len, void *priv)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);
	unsigned int i;
	long ret;

	if (!vcu || id <= IPI_VCU_INIT || id >= IPI_MAX ||
	    len > SHARE_BUF_SIZE || !buf)
		return -EINVAL;

	if (!vcu_running(vcu)) {
		dev_err(&pdev->dev, "[VCU] %s: VCU is not running\n", __func__);
		return -EPERM;
	}

	i = ipi_id_to_inst_id(id);
	vcu_trace("ipi send %s id %d msg 0x%x len %u\n",
		  i == VCU_VENC ? "enc" : "dec", id, vcu_msg_id(buf), len);

	mutex_lock(&vcu->vcu_mutex[i]);
	if (vcu->abort) {
		vcu_wait_vpud_killed(vcu);
		mutex_unlock(&vcu->vcu_mutex[i]);
		return -EIO;
	}
	vcu->ipi_id_ack[id] = false;
	vcu->ipi_desc[id].priv = priv;

	memcpy(vcu->user_obj[i].share_buf, buf, len);
	vcu->user_obj[i].len = len;
	vcu->user_obj[i].id = (int)id;
	atomic_set(&vcu->ipi_got[i], 1);
	atomic_set(&vcu->ipi_done[i], 0);
	wake_up(&vcu->get_wq[i]);

	ret = wait_event_timeout(vcu->ack_wq[i], vcu->ipi_id_ack[id],
				 msecs_to_jiffies(IPI_TIMEOUT_MS));
	vcu->ipi_id_ack[id] = false;

	if (vcu->abort || ret == 0) {
		dev_info(&pdev->dev, "vcu ipi %d ack time out\n", id);
		if (!vcu->abort && vcu->vcud_task)
			send_sig(SIGKILL, vcu->vcud_task, 0);
		vcu_wait_vpud_killed(vcu);
		mutex_unlock(&vcu->vcu_mutex[i]);
		return -EIO;
	}
	mutex_unlock(&vcu->vcu_mutex[i]);

	/* wait until the daemon receiver thread came back for the next msg */
	while (atomic_read(&vcu->ipi_done[i]) == 0 && !vcu->abort)
		cond_resched();

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_ipi_send);

static int vcu_ipi_get(struct mtk_vcu *vcu, void __user *uarg)
{
	struct share_obj obj;
	unsigned int i;
	int ret;

	if (copy_from_user(&obj, uarg, sizeof(obj)))
		return -EFAULT;

	i = ipi_id_to_inst_id(obj.id);

	atomic_set(&vcu->ipi_done[i], 1);
	ret = wait_event_freezable(vcu->get_wq[i],
				   atomic_read(&vcu->ipi_got[i]));
	if (ret)
		return ret;

	if (copy_to_user(uarg, &vcu->user_obj[i], sizeof(struct share_obj)))
		ret = -EFAULT;
	atomic_set(&vcu->ipi_got[i], 0);

	return ret;
}

static int vcu_log_get(struct mtk_vcu *vcu, void __user *uarg)
{
	int ret;

	if (!vcu->vdec_log_info)
		return -EINVAL;

	ret = wait_event_freezable(vcu->vdec_log_get_wq,
				   atomic_read(&vcu->vdec_log_got));
	if (ret)
		return ret;

	if (copy_to_user(uarg, vcu->vdec_log_info,
			 sizeof(struct log_test_nofuse)))
		ret = -EFAULT;
	atomic_set(&vcu->vdec_log_got, 0);

	return ret;
}

static int vcu_ipi_handler(struct mtk_vcu *vcu, struct share_obj *rcv_obj)
{
	struct vcu_ipi_desc *ipi_desc = vcu->ipi_desc;
	unsigned int i = ipi_id_to_inst_id(rcv_obj->id);
	int non_ack;

	if (rcv_obj->len > SHARE_BUF_SIZE)
		rcv_obj->len = SHARE_BUF_SIZE;

	if (!ipi_desc[rcv_obj->id].handler) {
		dev_err(vcu->dev, "[VCU] No such ipi id = %d\n", rcv_obj->id);
		return -EINVAL;
	}

	vcu_trace("ipi recv %s id %d msg 0x%x len %u\n",
		  i == VCU_VENC ? "enc" : "dec", rcv_obj->id,
		  vcu_msg_id(rcv_obj->share_buf), rcv_obj->len);
	non_ack = ipi_desc[rcv_obj->id].handler(rcv_obj->share_buf,
						rcv_obj->len,
						ipi_desc[rcv_obj->id].priv);
	vcu_trace("ipi recv id %d done %d\n", rcv_obj->id, non_ack);
	if (rcv_obj->id > IPI_VCU_INIT && non_ack == 0) {
		vcu->ipi_id_ack[rcv_obj->id] = true;
		wake_up(&vcu->ack_wq[i]);
	}

	return 0;
}

static bool vcu_gce_busy(struct mtk_vcu *vcu)
{
	int i, j;

	for (i = 0; i < VCU_CODEC_MAX; i++)
		for (j = 0; j < GCE_THNUM_MAX; j++)
			if (atomic_read(&vcu->gce_job_cnt[i][j]) > 0)
				return true;
	return false;
}

static void vcu_venc_daemon_hold(struct mtk_vcu *vcu, bool running)
{
	if (running && venc_always_on && !vcu->venc_held && vcu->reg_get) {
		int ret = vcu->reg_get(vcu->reg_priv);

		vcu->venc_held = !ret;
		vcu_trace_log("[VCU] vpud up, venc power hold %s (%d)\n",
			      ret ? "failed" : "on", ret);
	} else if (!running && vcu->venc_held) {
		vcu->reg_put(vcu->reg_priv);
		vcu->venc_held = false;
	}
}

static int vcu_init_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_vcu *vcu = priv;
	struct vcu_run_msg *run = data;
	int i, wait_cnt = 0;

	if (len < sizeof(*run))
		return -EINVAL;

	/* daemon going down: wake every waiter and fail pending IPIs */
	if (vcu->run.signaled == 1U && run->signaled == 0U) {
		vcu->abort = true;
		for (i = 0; i < IPI_MAX; i++)
			vcu->ipi_id_ack[i] = true;

		while (vcu_gce_busy(vcu) && ++wait_cnt <= 5)
			usleep_range(10000, 20000);

		for (i = 0; i < VCU_CODEC_MAX; i++) {
			atomic_set(&vcu->ipi_got[i], 1);
			atomic_set(&vcu->ipi_done[i], 0);
			memset(&vcu->user_obj[i], 0, sizeof(struct share_obj));
			wake_up(&vcu->get_wq[i]);
			wake_up(&vcu->ack_wq[i]);
		}
		atomic_set(&vcu->vdec_log_got, 1);
		wake_up(&vcu->vdec_log_get_wq);
		vcu->vcud_task = NULL;
		WRITE_ONCE(vcu->daemon_pid, -1);
		vcu->run.signaled = 0;
		mutex_lock(&vcu->vcu_share);
		vcu_venc_daemon_hold(vcu, false);
		mutex_unlock(&vcu->vcu_share);
		dev_info(vcu->dev, "[VCU] vpud killing\n");
		return 0;
	}

	vcu->run.signaled = run->signaled;
	strscpy_pad(vcu->run.fw_ver, run->fw_ver, VCU_FW_VER_LEN);
	vcu->run.dec_capability = vdec ? run->dec_capability : 0;
	vcu->run.enc_capability = run->enc_capability;
	if (!vdec && run->dec_capability)
		vcu_trace_log("[VCU] vdec disabled, ignoring dec cap 0x%x\n",
			 run->dec_capability);

	dev_info(vcu->dev, "[VCU] vpud up: fw %s dec cap 0x%x enc cap 0x%x\n",
		 vcu->run.fw_ver, vcu->run.dec_capability,
		 vcu->run.enc_capability);

	mutex_lock(&vcu->vcu_share);
	vcu_venc_daemon_hold(vcu, run->signaled);
	mutex_unlock(&vcu->vcu_share);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Exported helpers for codec front ends                               */
/* ------------------------------------------------------------------ */

unsigned int vcu_get_vdec_hw_capa(struct platform_device *pdev)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	return vcu ? vcu->run.dec_capability : 0;
}
EXPORT_SYMBOL_GPL(vcu_get_vdec_hw_capa);

unsigned int vcu_get_venc_hw_capa(struct platform_device *pdev)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	return vcu ? vcu->run.enc_capability : 0;
}
EXPORT_SYMBOL_GPL(vcu_get_venc_hw_capa);

void *vcu_mapping_dm_addr_range(struct platform_device *pdev,
			  uintptr_t dtcm_dmem_addr, size_t size)
{
	struct mtk_vcu *vcu;
	uintptr_t d_off;

	if (IS_ERR_OR_NULL(pdev))
		return NULL;
	vcu = platform_get_drvdata(pdev);
	if (!vcu || !vcu->dmem_va || !vcu->dmem_vma)
		return NULL;

	d_off = dtcm_dmem_addr - vcu->dmem_vma;
	if (dtcm_dmem_addr == 0UL || dtcm_dmem_addr < vcu->dmem_vma ||
	    d_off >= VCU_SHMEM_SIZE || !size || size > VCU_SHMEM_SIZE - d_off) {
		dev_info(&pdev->dev, "[VCU] %s: invalid vma 0x%lx\n", __func__,
			 (unsigned long)dtcm_dmem_addr);
		return NULL;
	}

	return vcu->dmem_va + d_off;
}
EXPORT_SYMBOL_GPL(vcu_mapping_dm_addr_range);

void *vcu_mapping_dm_addr(struct platform_device *pdev, uintptr_t address)
{
	return vcu_mapping_dm_addr_range(pdev, address, 1);
}
EXPORT_SYMBOL_GPL(vcu_mapping_dm_addr);

struct platform_device *vcu_get_plat_device(struct platform_device *pdev)
{
	struct device_node *vcu_node;
	struct platform_device *vcu_pdev;

	vcu_node = of_parse_phandle(pdev->dev.of_node, "mediatek,vcu", 0);
	if (!vcu_node) {
		dev_err(&pdev->dev, "[VCU] can't get vcu node\n");
		return NULL;
	}

	vcu_pdev = of_find_device_by_node(vcu_node);
	of_node_put(vcu_node);
	if (!vcu_pdev || !platform_get_drvdata(vcu_pdev))
		return NULL;

	return vcu_pdev;
}
EXPORT_SYMBOL_GPL(vcu_get_plat_device);

int vcu_load_firmware(struct platform_device *pdev)
{
	return pdev ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(vcu_load_firmware);

int vcu_compare_version(struct platform_device *pdev,
			const char *expected_version)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);
	int cur[4] = { 0 }, exp[4] = { 0 };
	int cur_num, exp_num, k;

	cur_num = sscanf(vcu->run.fw_ver, "%d.%d.%d-rc%d",
			 &cur[0], &cur[1], &cur[2], &cur[3]);
	exp_num = sscanf(expected_version, "%d.%d.%d-rc%d",
			 &exp[0], &exp[1], &exp[2], &exp[3]);
	if (cur_num < 3 || exp_num < 3)
		return -1;

	for (k = 0; k < 3; k++) {
		if (cur[k] != exp[k])
			return cur[k] < exp[k] ? -1 : 1;
	}
	if (cur_num != exp_num)
		return cur_num < exp_num ? -1 : 1;
	if (exp_num > 3 && cur[3] != exp[3])
		return cur[3] < exp[3] ? -1 : 1;

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_compare_version);

void vcu_get_gce_lock(struct platform_device *pdev, unsigned long codec_type)
{
	struct mtk_vcu *vcu = pdev ? platform_get_drvdata(pdev) : NULL;

	if (vcu && codec_type < VCU_CODEC_MAX)
		mutex_lock(&vcu->vcu_gce_mutex[codec_type]);
}
EXPORT_SYMBOL_GPL(vcu_get_gce_lock);

void vcu_put_gce_lock(struct platform_device *pdev, unsigned long codec_type)
{
	struct mtk_vcu *vcu = pdev ? platform_get_drvdata(pdev) : NULL;

	if (vcu && codec_type < VCU_CODEC_MAX)
		mutex_unlock(&vcu->vcu_gce_mutex[codec_type]);
}
EXPORT_SYMBOL_GPL(vcu_put_gce_lock);

int vcu_check_vpud_alive(void)
{
	return (vcu_ptr && vcu_ptr->vcud_task &&
		!vcu_ptr->vpud_is_going_down) ? 1 : 0;
}
EXPORT_SYMBOL_GPL(vcu_check_vpud_alive);

int vcu_get_daemon_pid(struct platform_device *pdev)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	if (!vcu || !vcu_running(vcu) || vcu->vpud_is_going_down)
		return -1;
	return READ_ONCE(vcu->daemon_pid);
}
EXPORT_SYMBOL_GPL(vcu_get_daemon_pid);

int vcu_get_ctx_ipi_binding_lock(struct platform_device *pdev,
				 struct mutex **mutex, unsigned long type)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	if (!vcu || type >= VCU_CODEC_MAX)
		return -EINVAL;
	*mutex = &vcu->ctx_ipi_binding[type];

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_get_ctx_ipi_binding_lock);

int vcu_set_codec_ctx(struct platform_device *pdev, void *codec_ctx,
		      struct vb2_buffer *src_vb, struct vb2_buffer *dst_vb,
		      unsigned long type)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	if (!vcu || type >= VCU_CODEC_MAX)
		return -EINVAL;

	mutex_lock(&vcu->vcu_share);
	vcu->curr_ctx[type] = codec_ctx;
	vcu->curr_src_vb[type] = src_vb;
	vcu->curr_dst_vb[type] = dst_vb;
	mutex_unlock(&vcu->vcu_share);

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_set_codec_ctx);

static void vcu_gce_clear_inst(struct gce_ctx_info *info)
{
	WRITE_ONCE(info->generation, info->generation + 1);
	info->used = false;
	WRITE_ONCE(info->failed, false);
	info->v4l2_ctx = NULL;
	info->user_hdl = 0;
	atomic_set(&info->flush_done, 0);
	atomic_set(&info->flush_pending, 0);
}

/*
 * The mainline mailbox has no GCE timeout, so a packet stuck in a poll or
 * an event wait never calls back. Cycling the channel runs the cmdq
 * shutdown, which completes every busy task with -ECONNABORTED.
 */
static void vcu_gce_abort(struct mtk_vcu *vcu, unsigned int type,
			  unsigned long cores)
{
	int base = type == VCU_VENC ? vcu->gce_th_num[VCU_VDEC] : 0;
	struct cmdq_client *cl;
	struct mbox_chan *chan;
	int core;

	down_write(&vcu->gce_chan_sem[type]);
	for (core = 0; core < vcu->gce_th_num[type]; core++) {
		cl = type == VCU_VENC ? vcu->clt_venc[core] : vcu->clt_vdec[core];
		if (IS_ERR_OR_NULL(cl) || !(cores & BIT(core)) ||
		    test_bit(core, &vcu->lost_channels[type]) ||
		    !atomic_read(&vcu->gce_job_cnt[type][core]))
			continue;
		dev_info(vcu->dev, "[VCU] GCE type %u core %d stuck, resetting\n",
			 type, core);
		mbox_free_channel(cl->chan);
		chan = mbox_request_channel(&cl->client, base + core);
		if (IS_ERR(chan)) {
			set_bit(core, &vcu->lost_channels[type]);
			dev_err(vcu->dev, "[VCU] GCE channel %d lost: %ld\n",
				base + core, PTR_ERR(chan));
			continue;
		}
		cl->chan = chan;
	}
	up_write(&vcu->gce_chan_sem[type]);
	flush_workqueue(vcu->gce_wq_work);
}

/* Reset a GCE thread that has jobs queued but completed none for a while. */
static void vcu_gce_wdt_work(struct work_struct *work)
{
	struct mtk_vcu *vcu = container_of(to_delayed_work(work),
					   struct mtk_vcu, gce_wdt);
	unsigned long stuck;
	unsigned int type;
	bool busy = false;
	int core;

	for (type = 0; type < VCU_CODEC_MAX; type++) {
		stuck = 0;
		for (core = 0; core < vcu->gce_th_num[type]; core++) {
			if (!atomic_read(&vcu->gce_job_cnt[type][core]))
				continue;
			if (time_after(jiffies,
				       READ_ONCE(vcu->gce_progress[type][core]) +
				       VCU_GCE_STALL_TIMEOUT))
				stuck |= BIT(core);
			else
				busy = true;
		}
		if (stuck)
			vcu_gce_abort(vcu, type, stuck);
	}
	if (busy)
		schedule_delayed_work(&vcu->gce_wdt, HZ);
}

static void vcu_venc_release_core(struct mtk_vcu *vcu, int core)
{
	const struct vcu_codec_ops *ops = vcu->codec_ops;
	void *ctx = vcu->venc_owner[core]->v4l2_ctx;

	ops->encode_unprepare(ctx, core, &vcu->flags[VCU_VENC]);
	ops->venc_unlock(ctx, core);
	vcu->venc_owner[core] = NULL;
}

int vcu_clear_codec_ctx(struct platform_device *pdev, void *codec_ctx,
			unsigned long type)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);
	int i, core, aborts = 0;

	if (!vcu || type >= VCU_CODEC_MAX)
		return -EINVAL;

again:
	mutex_lock(&vcu->vcu_gce_mutex[type]);
	mutex_lock(&vcu->vcu_share);
	for (i = 0; i < VCODEC_INST_MAX; i++) {
		if (!vcu->gce_info[i].used || vcu->gce_info[i].type != type ||
		    vcu->gce_info[i].v4l2_ctx != codec_ctx)
			continue;
		if (atomic_read(&vcu->gce_info[i].flush_pending) && aborts < 2) {
			mutex_unlock(&vcu->vcu_share);
			mutex_unlock(&vcu->vcu_gce_mutex[type]);
			/* The completion must finish its last codec hook before free. */
			if (!wait_event_timeout(vcu->gce_wq[type],
				!atomic_read(&vcu->gce_info[i].flush_pending),
				VCU_GCE_CLEAR_TIMEOUT)) {
				aborts++;
				vcu_gce_abort(vcu, type, ~0UL);
			}
			goto again;
		}
		if (atomic_read(&vcu->gce_info[i].flush_pending)) {
			dev_err(vcu->dev, "[VCU] dropping GCE inst %d, %d jobs lost\n",
				i, atomic_read(&vcu->gce_info[i].flush_pending));
			if (type == VCU_VENC) {
				for (core = 0; core < vcu->gce_th_num[type]; core++) {
					if (vcu->venc_owner[core] != &vcu->gce_info[i])
						continue;
					atomic_set(&vcu->gce_job_cnt[type][core], 0);
					vcu_venc_release_core(vcu, core);
				}
			}
		}
		vcu_gce_clear_inst(&vcu->gce_info[i]);
	}
	if (vcu->curr_ctx[type] == codec_ctx) {
		vcu->curr_ctx[type] = NULL;
		vcu->curr_src_vb[type] = NULL;
		vcu->curr_dst_vb[type] = NULL;
	}
	mutex_unlock(&vcu->vcu_share);
	mutex_unlock(&vcu->vcu_gce_mutex[type]);
	wake_up_all(&vcu->gce_wq[type]);
	return 0;
}
EXPORT_SYMBOL_GPL(vcu_clear_codec_ctx);

void vcu_set_codec_ops(struct platform_device *pdev,
		       const struct vcu_codec_ops *ops)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	if (vcu)
		vcu->codec_ops = ops;
}
EXPORT_SYMBOL_GPL(vcu_set_codec_ops);

void vcu_set_reg_power(struct platform_device *pdev,
		       int (*get)(void *priv), void (*put)(void *priv),
		       void *priv)
{
	struct mtk_vcu *vcu = platform_get_drvdata(pdev);

	if (!vcu)
		return;
	mutex_lock(&vcu->vcu_share);
	if (!get)
		vcu_venc_daemon_hold(vcu, false);
	vcu->reg_get = get;
	vcu->reg_put = put;
	vcu->reg_priv = priv;
	mutex_unlock(&vcu->vcu_share);
	if (!get)
		wait_event(vcu->reg_map_wq, !atomic_read(&vcu->reg_maps));
}
EXPORT_SYMBOL_GPL(vcu_set_reg_power);

int vcu_set_log(const char *val)
{
	struct mtk_vcu *vcu = vcu_ptr;

	if (!vcu || !vcu->vdec_log_info || !val)
		return -EFAULT;

	strscpy(vcu->vdec_log_info->log_info, val, LOG_INFO_SIZE);

	if (strstr(vcu->vdec_log_info->log_info, "vcu_log 1")) {
		debug = true;
		return 0;
	}
	if (strstr(vcu->vdec_log_info->log_info, "vcu_log 0")) {
		debug = false;
		return 0;
	}

	atomic_set(&vcu->vdec_log_got, 1);
	wake_up(&vcu->vdec_log_get_wq);

	return 0;
}
EXPORT_SYMBOL_GPL(vcu_set_log);

static int vcu_test_info_set(const char *val, const struct kernel_param *kp)
{
	return vcu_set_log(val);
}

static const struct kernel_param_ops vcu_test_info_ops = {
	.set = vcu_test_info_set,
};
module_param_cb(test_info, &vcu_test_info_ops, NULL, 0200);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static struct vcu_queue *vcu_queue_create(struct mtk_vcu *vcu)
{
	struct vcu_queue *q = kzalloc(sizeof(*q), GFP_KERNEL);

	if (!q)
		return NULL;
	q->vcu = vcu;
	mutex_init(&q->lock);
	INIT_LIST_HEAD(&q->bufs);
	INIT_LIST_HEAD(&q->pages);

	return q;
}

static void vcu_queue_release(struct vcu_queue *q)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_buf *b, *tb;
	struct vcu_pa_page *p, *tp;

	mutex_lock(&q->lock);
	list_for_each_entry_safe(b, tb, &q->bufs, node) {
		if (b->dbuf)
			dma_buf_put(b->dbuf);
		else
			dma_free_coherent(vcu->dev, b->size, b->kva, b->iova);
		list_del(&b->node);
		kfree(b);
	}
	list_for_each_entry_safe(p, tp, &q->pages, node) {
		dma_free_coherent(vcu->gce_dev, PAGE_SIZE, p->kva, p->pa);
		list_del(&p->node);
		kfree(p);
	}
	q->num_bufs = 0;
	mutex_unlock(&q->lock);
	mutex_destroy(&q->lock);
	kfree(q);
}

static int vcu_mva_alloc(struct vcu_queue *q, struct mem_obj *mem)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_buf *b;

	if (mem->len == 0U || mem->len > CODEC_ALLOCATE_MAX_BUFFER_SIZE)
		return -EINVAL;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->size = PAGE_ALIGN(mem->len);
	b->kva = dma_alloc_coherent(vcu->dev, b->size, &b->iova, GFP_KERNEL);
	if (!b->kva) {
		kfree(b);
		return -ENOMEM;
	}
	atomic_set(&b->ref_cnt, 1);

	mutex_lock(&q->lock);
	if (q->num_bufs >= CODEC_MAX_BUFFER) {
		mutex_unlock(&q->lock);
		dma_free_coherent(vcu->dev, b->size, b->kva, b->iova);
		kfree(b);
		return -ENOMEM;
	}
	list_add_tail(&b->node, &q->bufs);
	q->num_bufs++;
	mutex_unlock(&q->lock);

	mem->iova = b->iova;
	mem->va = CODEC_MSK(b->kva);
	mem->pa = 0;
	vcu_dbg("MVA alloc iova %pad len %u\n", &b->iova, mem->len);

	return 0;
}

static int vcu_mva_free(struct vcu_queue *q, struct mem_obj *mem)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_buf *b, *found = NULL;

	mutex_lock(&q->lock);
	list_for_each_entry(b, &q->bufs, node) {
		if (b->dbuf)
			continue;
		if (mem->iova == (u64)b->iova &&
		    mem->va == CODEC_MSK(b->kva) &&
		    PAGE_ALIGN(mem->len) == b->size &&
		    atomic_read(&b->ref_cnt) == 1) {
			found = b;
			list_del(&b->node);
			q->num_bufs--;
			break;
		}
	}
	mutex_unlock(&q->lock);

	if (!found) {
		pr_info("[VCU] can not free va %llx iova %llx len %u\n",
			mem->va, mem->iova, mem->len);
		return -EINVAL;
	}
	dma_free_coherent(vcu->dev, found->size, found->kva, found->iova);
	kfree(found);

	return 0;
}

static int vcu_pa_alloc(struct vcu_queue *q, struct mem_obj *mem)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_pa_page *p;

	if (!vcu->gce_dev)
		return -ENODEV;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->kva = dma_alloc_coherent(vcu->gce_dev, PAGE_SIZE, &p->pa, GFP_KERNEL);
	if (!p->kva) {
		kfree(p);
		return -ENOMEM;
	}
	atomic_set(&p->ref_cnt, 1);

	mutex_lock(&q->lock);
	list_add_tail(&p->node, &q->pages);
	mutex_unlock(&q->lock);

	mem->pa = p->pa;
	mem->va = CODEC_MSK(p->kva);
	mem->iova = 0;

	return 0;
}

static int vcu_pa_free(struct vcu_queue *q, struct mem_obj *mem)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_pa_page *p, *found = NULL;

	mutex_lock(&q->lock);
	list_for_each_entry(p, &q->pages, node) {
		if (mem->pa == (u64)p->pa && mem->va == CODEC_MSK(p->kva) &&
		    atomic_read(&p->ref_cnt) == 1) {
			found = p;
			list_del(&p->node);
			break;
		}
	}
	mutex_unlock(&q->lock);

	if (!found)
		return -EINVAL;
	dma_free_coherent(vcu->gce_dev, PAGE_SIZE, found->kva, found->pa);
	kfree(found);

	return 0;
}

static struct vcu_pa_page *vcu_find_pa_page(struct vcu_queue *q, u64 addr,
					    u64 len)
{
	struct vcu_pa_page *p;

	list_for_each_entry(p, &q->pages, node)
		if (addr >= (u64)p->pa && addr + len <= (u64)p->pa + PAGE_SIZE)
			return p;

	return NULL;
}

static bool vcu_reg_in_range(struct mtk_vcu *vcu, u64 addr, u64 len)
{
	int i;

	if (addr >= MAP_PA_BASE_1GB)
		return false;

	for (i = 0; i < VCU_MAP_HW_REG_NUM; i++) {
		if (!vcu->map_base[i].len)
			continue;
		if (addr >= vcu->map_base[i].base &&
		    addr + len <= vcu->map_base[i].base + vcu->map_base[i].len)
			return true;
	}

	return false;
}

/*
 * The codec engines only take 32-bit addresses; the larb bank select supplies
 * bits 33:32. vpud therefore hands back iovas truncated to 32 bits, so restore
 * the bank of the candidate buffer before comparing.
 */
static inline u64 vcu_iova_rebase(u64 iova, u64 base)
{
	if (iova >> 32)
		return iova;
	return iova | (base & GENMASK_ULL(63, 32));
}

/*
 * Look up a working buffer by iova. Buffers of the current codec job (vb2
 * src/dst planes imported as dma-buf) are added to the queue on first use.
 */
static struct vcu_buf *vcu_find_buf(struct vcu_queue *q, u64 iova, u64 len,
				    bool lookup_vb)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vb2_buffer *vbs[2] = { NULL, NULL };
	struct vcu_buf *b;
	unsigned int k, plane;

	list_for_each_entry(b, &q->bufs, node) {
		u64 a = vcu_iova_rebase(iova, b->iova);

		if (a >= (u64)b->iova && a + len <= (u64)b->iova + b->size)
			return b;
	}

	if (!lookup_vb)
		return NULL;

	if (!strcmp(current->comm, "vdec_srv")) {
		vbs[0] = vcu->curr_src_vb[VCU_VDEC];
		vbs[1] = vcu->curr_dst_vb[VCU_VDEC];
	} else if (!strcmp(current->comm, "venc_srv")) {
		vbs[0] = vcu->curr_src_vb[VCU_VENC];
		vbs[1] = vcu->curr_dst_vb[VCU_VENC];
	}

	for (k = 0; k < ARRAY_SIZE(vbs); k++) {
		if (!vbs[k])
			continue;
		for (plane = 0; plane < vbs[k]->num_planes; plane++) {
			dma_addr_t *addr = vb2_plane_cookie(vbs[k], plane);
			struct dma_buf *dbuf = vbs[k]->planes[plane].dbuf;

			if (!addr || !dbuf ||
			    *addr != vcu_iova_rebase(iova, *addr))
				continue;
			if (q->num_bufs >= CODEC_MAX_BUFFER)
				return NULL;
			b = kzalloc(sizeof(*b), GFP_KERNEL);
			if (!b)
				return NULL;
			get_dma_buf(dbuf);
			b->dbuf = dbuf;
			b->iova = *addr;
			b->size = vbs[k]->planes[plane].length;
			atomic_set(&b->ref_cnt, 1);
			list_add_tail(&b->node, &q->bufs);
			q->num_bufs++;
			return b;
		}
	}

	return NULL;
}

static int vcu_cache_sync(struct vcu_queue *q, u64 iova, u64 len, bool flush)
{
	struct vcu_buf *b;
	int ret = 0;

	mutex_lock(&q->lock);
	b = vcu_find_buf(q, iova, len, false);
	if (!b) {
		ret = -EINVAL;
	} else if (b->dbuf) {
		/* working buffers are coherent; only imported io buffers sync */
		if (flush)
			ret = dma_buf_end_cpu_access(b->dbuf, DMA_TO_DEVICE);
		else
			ret = dma_buf_begin_cpu_access(b->dbuf, DMA_FROM_DEVICE);
	}
	mutex_unlock(&q->lock);

	if (ret)
		pr_info("[VCU] cache %s fail iova %llx len %llu\n",
			flush ? "flush" : "invalidate", iova, len);

	return ret;
}

static void vcu_cache_flush_all(struct vcu_queue *q)
{
	struct vcu_buf *b;

	mutex_lock(&q->lock);
	list_for_each_entry(b, &q->bufs, node)
		if (b->dbuf)
			dma_buf_end_cpu_access(b->dbuf, DMA_TO_DEVICE);
	mutex_unlock(&q->lock);
}

/* ------------------------------------------------------------------ */
/* GCE                                                                 */
/* ------------------------------------------------------------------ */

/* Caller holds vcu_share across lookup and generation capture. */
static int vcu_gce_get_inst_id(struct mtk_vcu *vcu, unsigned int type, u64 gce_handle)
{
	int i;

	for (i = 0; i < VCODEC_INST_MAX; i++) {
		if (vcu->gce_info[i].used && vcu->gce_info[i].type == type &&
		    vcu->gce_info[i].user_hdl == gce_handle)
			return i;
	}

	return -1;
}

static int vcu_gce_set_inst_id(struct mtk_vcu *vcu, unsigned int type, u64 gce_handle)
{
	int i;

	for (i = 0; i < VCODEC_INST_MAX; i++) {
		if (!vcu->gce_info[i].used) {
			vcu_gce_clear_inst(&vcu->gce_info[i]);
			vcu->gce_info[i].used = true;
			vcu->gce_info[i].type = type;
			vcu->gce_info[i].v4l2_ctx = vcu->curr_ctx[type];
			vcu->gce_info[i].user_hdl = gce_handle;
			return i;
		}
	}
	pr_info("[VCU] no free GCE instance slot for hndl %llx\n", gce_handle);

	return -1;
}

static int vcu_set_gce_cmd(struct cmdq_pkt *pkt, struct mtk_vcu *vcu,
			   struct vcu_queue *q, u8 cmd, u64 addr, u64 data,
			   u32 mask)
{
	const u16 high = CMDQ_THR_SPR_IDX0;
	int ret = 0;

	switch (cmd) {
	case CMD_READ:
		if (!vcu_reg_in_range(vcu, addr, 4))
			goto bad;
		ret = cmdq_pkt_assign(pkt, high, CMDQ_ADDR_HIGH(addr));
		if (!ret)
			ret = cmdq_pkt_read_s(pkt, high, CMDQ_ADDR_LOW(addr),
					      CMDQ_THR_SPR_IDX1);
		break;
	case CMD_WRITE:
		if (!vcu_reg_in_range(vcu, addr, 4))
			goto bad;
		ret = cmdq_pkt_assign(pkt, high, CMDQ_ADDR_HIGH(addr));
		if (ret)
			break;
		if (mask == GENMASK(31, 0))
			ret = cmdq_pkt_write_s_value(pkt, high,
						     CMDQ_ADDR_LOW(addr),
						     (u32)data);
		else
			ret = cmdq_pkt_write_s_mask_value(pkt, high,
							  CMDQ_ADDR_LOW(addr),
							  (u32)data, mask);
		break;
	case CMD_POLL_REG:
		if (!vcu_reg_in_range(vcu, addr, 4))
			goto bad;
		ret = cmdq_pkt_poll_addr(pkt, addr, (u32)data, mask);
		break;
	case CMD_POLL_ADDR:
		/* the mainline mailbox has no polling timeout */
		if (!vcu_reg_in_range(vcu, addr, 4) &&
		    !vcu_find_pa_page(q, addr, 4))
			goto bad;
		ret = cmdq_pkt_poll_addr(pkt, addr, (u32)data, mask);
		break;
	case CMD_WAIT_EVENT:
		if (data >= GCE_EVENT_MAX || vcu->gce_codec_eid[data] < 0)
			goto bad;
		ret = cmdq_pkt_wfe(pkt, vcu->gce_codec_eid[data], true);
		break;
	case CMD_MEM_MV:
		if ((!vcu_reg_in_range(vcu, addr, 4) &&
		     !vcu_find_pa_page(q, addr, 4)) ||
		    !vcu_find_pa_page(q, data, 4))
			goto bad;
		ret = cmdq_pkt_mem_move(pkt, addr, data);
		break;
	case CMD_SEC_WRITE:
	default:
		goto bad;
	}

	return ret;
bad:
	pr_info("[VCU] rejected GCE cmd %u addr 0x%llx data 0x%llx mask 0x%x\n",
		cmd, addr, data, mask);
	return -EINVAL;
}

static void vcu_gce_job_done(struct work_struct *work)
{
	struct vcu_gce_job *job = container_of(work, struct vcu_gce_job, work);
	struct mtk_vcu *vcu = job->vcu;
	const struct vcu_codec_ops *ops = vcu->codec_ops;
	unsigned int i = job->obj.codec_type;
	unsigned int core_id = job->obj.core_id;
	void *ctx = NULL;
	int j;

	if (job->sta)
		dev_info(vcu->dev,
			 "[VCU] GCE job hndl %llx status %d, last wait op %u addr 0x%llx data 0x%llx mask 0x%x\n",
			 job->obj.gce_handle, job->sta, job->wait.cmd,
			 job->wait.addr, job->wait.data, job->wait.mask);

	mutex_lock(&vcu->vcu_gce_mutex[i]);
	mutex_lock(&vcu->vcu_share);
	j = job->inst;
	if (!vcu->gce_info[j].used ||
	    vcu->gce_info[j].generation != job->generation)
		j = -1;
	if (j >= 0) {
		ctx = vcu->gce_info[j].v4l2_ctx;
		if (job->sta && i == VCU_VENC && ctx && ops && ops->venc_gce_error) {
			WRITE_ONCE(vcu->gce_info[j].failed, true);
			ops->venc_gce_error(ctx, job->sta);
		}
	}

	if (j >= 0 || i == VCU_VDEC) {
		WRITE_ONCE(vcu->gce_progress[i][core_id], jiffies);
		if (i == VCU_VENC && ops->pmqos_gce_end)
			ops->pmqos_gce_end(ctx, core_id,
					   atomic_read(&vcu->gce_job_cnt[i][core_id]));
		if (atomic_dec_and_test(&vcu->gce_job_cnt[i][core_id]) && i == VCU_VENC)
			vcu_venc_release_core(vcu, core_id);
	}
	if (j >= 0) {
		atomic_inc(&vcu->gce_info[j].flush_done);
		atomic_dec(&vcu->gce_info[j].flush_pending);
	}
	mutex_unlock(&vcu->vcu_share);
	mutex_unlock(&vcu->vcu_gce_mutex[i]);

	wake_up(&vcu->gce_wq[i]);

	if (job->pm_ref) {
		pm_runtime_mark_last_busy(job->clt->chan->mbox->dev);
		pm_runtime_put_autosuspend(job->clt->chan->mbox->dev);
	}
	cmdq_pkt_destroy(job->clt, &job->pkt);
	kfree(job);
}

static void vcu_gce_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct cmdq_cb_data *data = mssg;
	struct vcu_gce_job *job = container_of(data->pkt, struct vcu_gce_job,
					       pkt);

	job->sta = data->sta;
	queue_work(job->vcu->gce_wq_work, &job->work);
}

static int vcu_gce_cmd_flush(struct mtk_vcu *vcu, struct vcu_queue *q,
			     void __user *uarg)
{
	const struct vcu_codec_ops *ops = vcu->codec_ops;
	struct gce_cmdq_obj obj;
	struct cmdq_client *cl;
	struct vcu_gce_job *job;
	struct gce_cmds *cmds;
	unsigned int i, core_id, n, k;
	u64 generation;
	void *ctx;
	int j, ret;

	if (!gce) {
		vcu_trace_log("[VCU] gce off, rejecting flush pid %d (%s)\n",
			      current->pid, current->comm);
		return -EPERM;
	}

	if (copy_from_user(&obj, uarg, sizeof(obj)))
		return -EFAULT;

	if (obj.codec_type >= VCU_CODEC_MAX || !obj.cmds_user_ptr)
		return -EINVAL;
	if (obj.secure) {
		pr_info("[VCU] secure GCE flush not supported\n");
		return -EOPNOTSUPP;
	}
	i = obj.codec_type;
	core_id = obj.core_id;
	if (i == VCU_VDEC && !vdec) {
		vcu_trace_log("[VCU] vdec disabled, rejecting vdec gce flush pid %d\n",
			 current->pid);
		return -EPERM;
	}
	if (core_id >= vcu->gce_th_num[i])
		return -EINVAL;
	cl = (i == VCU_VDEC) ? vcu->clt_vdec[core_id] : vcu->clt_venc[core_id];
	if (IS_ERR_OR_NULL(cl))
		return -ENODEV;

	mutex_lock(&vcu->vcu_share);
	j = vcu_gce_get_inst_id(vcu, i, obj.gce_handle);
	if (j < 0 && (i != VCU_VENC || vcu->curr_ctx[i]))
		j = vcu_gce_set_inst_id(vcu, i, obj.gce_handle);
	if (j < 0) {
		mutex_unlock(&vcu->vcu_share);
		return -EINVAL;
	}
	ctx = vcu->gce_info[j].v4l2_ctx;
	generation = vcu->gce_info[j].generation;
	mutex_unlock(&vcu->vcu_share);

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	job->clt = cl;
	job->obj = obj;
	job->vcu = vcu;
	INIT_WORK(&job->work, vcu_gce_job_done);

	mutex_lock(&vcu->gce_cmds_mutex[i]);
	cmds = vcu->gce_cmds[i];
	if (copy_from_user(cmds, u64_to_user_ptr(obj.cmds_user_ptr),
			   sizeof(*cmds))) {
		ret = -EFAULT;
		goto err_job;
	}
	if (cmds->cmd_cnt > VCODEC_CMDQ_CMD_MAX) {
		ret = -EINVAL;
		goto err_job;
	}
	n = cmds->cmd_cnt;
	vcu_trace("gce flush codec %u core %u cmds %u\n", i, core_id, n);
	for (k = 0; k < n && k < 8; k++)
		vcu_trace("gce cmd%u op %u addr 0x%llx data 0x%llx mask 0x%x\n",
			  k, cmds->cmd[k], cmds->addr[k], cmds->data[k],
			  cmds->mask[k]);

	ret = cmdq_pkt_create(cl, &job->pkt,
			      (n * GCE_INST_PER_CMD + 4) * CMDQ_INST_SIZE);
	if (ret)
		goto err_job;

	mutex_lock(&q->lock);
	for (k = 0; k < n && !ret; k++) {
		ret = vcu_set_gce_cmd(&job->pkt, vcu, q, cmds->cmd[k],
				      cmds->addr[k], cmds->data[k],
				      cmds->mask[k]);
		if (cmds->cmd[k] == CMD_POLL_REG ||
		    cmds->cmd[k] == CMD_POLL_ADDR ||
		    cmds->cmd[k] == CMD_WAIT_EVENT) {
			job->wait.cmd = cmds->cmd[k];
			job->wait.addr = cmds->addr[k];
			job->wait.data = cmds->data[k];
			job->wait.mask = cmds->mask[k];
		}
	}
	mutex_unlock(&q->lock);
	if (!ret)
		ret = cmdq_pkt_eoc(&job->pkt);
	if (ret)
		goto err_pkt;
	mutex_unlock(&vcu->gce_cmds_mutex[i]);

	for (;;) {
		down_read(&vcu->gce_chan_sem[i]);
		mutex_lock(&vcu->vcu_gce_mutex[i]);
		mutex_lock(&vcu->vcu_share);
		if (test_bit(core_id, &vcu->lost_channels[i]) || vcu->gce_info[j].failed) {
			ret = -EIO;
			goto err_submit;
		}
		if (!vcu->gce_info[j].used ||
		    vcu->gce_info[j].generation != generation ||
		    (i == VCU_VENC && (!ops || !ops->encode_prepare || !ops->encode_unprepare ||
				       !ops->venc_lock || !ops->venc_unlock || !ctx))) {
			ret = -EINVAL;
			goto err_submit;
		}
		if (i != VCU_VENC || vcu->venc_owner[core_id] == &vcu->gce_info[j] ||
		    (!vcu->venc_owner[core_id] && !ops->venc_lock(ctx, core_id, false)))
			break;
		mutex_unlock(&vcu->vcu_share);
		mutex_unlock(&vcu->vcu_gce_mutex[i]);
		up_read(&vcu->gce_chan_sem[i]);
		usleep_range(1000, 2000);
	}
	if (i == VCU_VENC && !vcu->venc_owner[core_id]) {
		ret = ops->encode_prepare(ctx, core_id, &vcu->flags[i]);
		if (ret) {
			ops->venc_unlock(ctx, core_id);
			goto err_submit;
		}
		vcu->venc_owner[core_id] = &vcu->gce_info[j];
	}
	if (i == VCU_VENC && ctx && ops && ops->pmqos_gce_begin)
		ops->pmqos_gce_begin(ctx, core_id,
				     atomic_read(&vcu->gce_job_cnt[i][core_id]));
	if (!atomic_read(&vcu->gce_job_cnt[i][core_id]))
		WRITE_ONCE(vcu->gce_progress[i][core_id], jiffies);
	atomic_inc(&vcu->gce_job_cnt[i][core_id]);
	job->inst = j;
	job->generation = generation;
	atomic_inc(&vcu->gce_info[j].flush_pending);
	mutex_unlock(&vcu->vcu_share);
	mutex_unlock(&vcu->vcu_gce_mutex[i]);

	dma_sync_single_for_device(cl->chan->mbox->dev, job->pkt.pa_base,
				   job->pkt.cmd_buf_size, DMA_TO_DEVICE);
	vcu_dbg("GCE flush type %u core %u hndl %llx order %u size %zu\n",
		i, core_id, obj.gce_handle, obj.flush_order,
		job->pkt.cmd_buf_size);

	/* keep the GCE clocked until the job callback runs */
	ret = pm_runtime_resume_and_get(cl->chan->mbox->dev);
	if (!ret) {
		job->pm_ref = true;
		ret = mbox_send_message(cl->chan, &job->pkt);
	}
	if (ret >= 0)
		mbox_client_txdone(cl->chan, 0);
	up_read(&vcu->gce_chan_sem[i]);
	if (ret < 0) {
		pr_info("[VCU] GCE send fail %d\n", ret);
		job->sta = ret;
		vcu_gce_job_done(&job->work);
		return ret;
	}
	schedule_delayed_work(&vcu->gce_wdt, HZ);

	return 0;

err_submit:
	mutex_unlock(&vcu->vcu_share);
	mutex_unlock(&vcu->vcu_gce_mutex[i]);
	up_read(&vcu->gce_chan_sem[i]);
	cmdq_pkt_destroy(cl, &job->pkt);
	kfree(job);
	return ret;
err_pkt:
	cmdq_pkt_destroy(cl, &job->pkt);
err_job:
	mutex_unlock(&vcu->gce_cmds_mutex[i]);
	kfree(job);
	return ret;
}

static int vcu_wait_gce_callback(struct mtk_vcu *vcu, void __user *uarg)
{
	struct gce_obj obj;
	unsigned int i;
	struct gce_ctx_info *info;
	u64 generation;
	int j, ret;

	if (copy_from_user(&obj, uarg, sizeof(obj)))
		return -EFAULT;

	i = obj.codec_type;
	if (i >= VCU_CODEC_MAX)
		return -EINVAL;
	mutex_lock(&vcu->vcu_share);
	j = vcu_gce_get_inst_id(vcu, i, obj.gce_handle);
	if (j < 0) {
		mutex_unlock(&vcu->vcu_share);
		return -EINVAL;
	}
	info = &vcu->gce_info[j];
	generation = info->generation;
	for (;;) {
		if (info->generation != generation) {
			ret = -ECANCELED;
			break;
		}
		if (atomic_dec_if_positive(&info->flush_done) >= 0) {
			ret = 0;
			break;
		}
		if (info->failed && !atomic_read(&info->flush_pending)) {
			ret = -EIO;
			break;
		}
		mutex_unlock(&vcu->vcu_share);
		ret = wait_event_interruptible(vcu->gce_wq[i],
			READ_ONCE(info->generation) != generation ||
			atomic_read(&info->flush_done) > 0 ||
			(READ_ONCE(info->failed) && !atomic_read(&info->flush_pending)));
		if (ret)
			return ret;
		mutex_lock(&vcu->vcu_share);
	}
	mutex_unlock(&vcu->vcu_share);
	return ret;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int mtk_vcu_open(struct inode *inode, struct file *file)
{
	struct mtk_vcu *vcu = vcu_ptr;
	struct vcu_queue *q;

	if (!vcu)
		return -ENODEV;

	q = vcu_queue_create(vcu);
	if (!q)
		return -ENOMEM;

	mutex_lock(&vcu->vcu_share);
	if (!strcmp(current->comm, "vpud")) {
		vcu->vcud_task = current->group_leader;
		WRITE_ONCE(vcu->daemon_pid, current->tgid);
	}
	vcu->open_cnt++;
	sema_init(&vcu->vpud_killed, 0);
	vcu->abort = false;
	vcu->vpud_is_going_down = 0;
	mutex_unlock(&vcu->vcu_share);

	file->private_data = q;
	pr_info("[VCU] %s name: %s pid %d open_cnt %d\n", __func__,
		current->comm, current->tgid, vcu->open_cnt);

	return 0;
}

static int mtk_vcu_release(struct inode *inode, struct file *file)
{
	struct vcu_queue *q = file->private_data;
	struct mtk_vcu *vcu = q->vcu;
	int i;

	vcu_queue_release(q);

	mutex_lock(&vcu->vcu_share);
	vcu->open_cnt--;
	pr_info("[VCU] %s name: %s pid %d open_cnt %d\n", __func__,
		current->comm, current->tgid, vcu->open_cnt);
	if (vcu->open_cnt == 0) {
		vcu_venc_daemon_hold(vcu, false);
		vcu->abort = true;
		vcu->vcud_task = NULL;
		WRITE_ONCE(vcu->daemon_pid, -1);
		vcu->run.signaled = 0;
		vcu->vpud_is_going_down = 0;
		for (i = 0; i < VCODEC_INST_MAX; i++)
			if (!vcu->gce_info[i].v4l2_ctx)
				vcu_gce_clear_inst(&vcu->gce_info[i]);
		/* release the vdec and venc workers */
		up(&vcu->vpud_killed);
		up(&vcu->vpud_killed);
		for (i = 0; i < VCU_CODEC_MAX; i++) {
			wake_up(&vcu->ack_wq[i]);
			wake_up_all(&vcu->gce_wq[i]);
		}
	}
	mutex_unlock(&vcu->vcu_share);

	if (vcu->open_cnt == 0 && vcu->curr_ctx[VCU_VDEC] && vcu->codec_ops &&
	    vcu->codec_ops->vdec_check_release_lock)
		vcu->codec_ops->vdec_check_release_lock(vcu->curr_ctx[VCU_VDEC]);

	return 0;
}

static void vcu_buf_vm_open(struct vm_area_struct *vma)
{
	struct vcu_buf *b = vma->vm_private_data;

	atomic_inc(&b->ref_cnt);
}

static void vcu_buf_vm_close(struct vm_area_struct *vma)
{
	struct vcu_buf *b = vma->vm_private_data;

	atomic_dec(&b->ref_cnt);
}

static const struct vm_operations_struct vcu_buf_vm_ops = {
	.open = vcu_buf_vm_open,
	.close = vcu_buf_vm_close,
};

static void vcu_page_vm_open(struct vm_area_struct *vma)
{
	struct vcu_pa_page *p = vma->vm_private_data;

	atomic_inc(&p->ref_cnt);
}

static void vcu_page_vm_close(struct vm_area_struct *vma)
{
	struct vcu_pa_page *p = vma->vm_private_data;

	atomic_dec(&p->ref_cnt);
}

static const struct vm_operations_struct vcu_page_vm_ops = {
	.open = vcu_page_vm_open,
	.close = vcu_page_vm_close,
};

/* the venc register windows keep the block powered while mapped */
struct vcu_reg_mapping {
	struct kref refs;
	struct mtk_vcu *vcu;
	void (*put)(void *priv);
	void *priv;
};

static void vcu_reg_mapping_release(struct kref *refs)
{
	struct vcu_reg_mapping *mapping =
		container_of(refs, struct vcu_reg_mapping, refs);
	struct mtk_vcu *vcu = mapping->vcu;

	mapping->put(mapping->priv);
	if (atomic_dec_and_test(&vcu->reg_maps))
		wake_up_all(&vcu->reg_map_wq);
	kfree(mapping);
}

static void vcu_reg_vm_open(struct vm_area_struct *vma)
{
	struct vcu_reg_mapping *mapping = vma->vm_private_data;

	kref_get(&mapping->refs);
}

static void vcu_reg_vm_close(struct vm_area_struct *vma)
{
	struct vcu_reg_mapping *mapping = vma->vm_private_data;

	kref_put(&mapping->refs, vcu_reg_mapping_release);
}

static const struct vm_operations_struct vcu_reg_vm_ops = {
	.open = vcu_reg_vm_open,
	.close = vcu_reg_vm_close,
};

static int vcu_reg_index(struct mtk_vcu *vcu, u64 addr, u64 len)
{
	int i;

	for (i = 0; i < VCU_MAP_HW_REG_NUM; i++) {
		if (!vcu->map_base[i].len)
			continue;
		if (addr >= vcu->map_base[i].base &&
		    addr + len <= vcu->map_base[i].base + vcu->map_base[i].len)
			return i;
	}
	return -1;
}

static int vcu_mmap_reg(struct mtk_vcu *vcu, struct vm_area_struct *vma,
			u64 pa_start, unsigned long length)
{
	int idx = vcu_reg_index(vcu, pa_start, length);
	struct vcu_reg_mapping *mapping = NULL;
	int ret;

	vcu_trace("mmap hw reg%d pa 0x%llx len 0x%lx\n", idx, pa_start, length);
	if (idx < 0)
		return -EINVAL;
	/* the VDEC window has no power handling; touching it unpowered hangs the bus */
	if (idx == 0 && !vdec) {
		vcu_trace_log("[VCU] vdec disabled, rejecting reg0 mmap pa 0x%llx pid %d (%s)\n",
			 pa_start, current->pid, current->comm);
		return -EPERM;
	}

	/* reg0 is VDEC, the others are the venc core windows */
	if (idx > 0) {
		mapping = kmalloc(sizeof(*mapping), GFP_KERNEL);
		if (!mapping)
			return -ENOMEM;
		mutex_lock(&vcu->vcu_share);
		ret = vcu->reg_get && vcu->reg_put ? vcu->reg_get(vcu->reg_priv) : -ENODEV;
		if (!ret) {
			mapping->put = vcu->reg_put;
			mapping->priv = vcu->reg_priv;
			atomic_inc(&vcu->reg_maps);
		}
		mutex_unlock(&vcu->vcu_share);
		if (ret) {
			kfree(mapping);
			vcu_trace_log("[VCU] venc power on for mmap failed %d\n", ret);
			return ret;
		}
		mapping->vcu = vcu;
		kref_init(&mapping->refs);
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	ret = io_remap_pfn_range(vma, vma->vm_start, pa_start >> PAGE_SHIFT,
				 length, vma->vm_page_prot);
	if (!mapping)
		return ret;
	if (ret) {
		kref_put(&mapping->refs, vcu_reg_mapping_release);
		return ret;
	}
	vma->vm_ops = &vcu_reg_vm_ops;
	vma->vm_private_data = mapping;
	return 0;
}

static int vcu_mmap_mm(struct vcu_queue *q, struct vm_area_struct *vma,
		       u64 iova, unsigned long length)
{
	struct mtk_vcu *vcu = q->vcu;
	struct vcu_buf *b;
	unsigned long off;
	int ret;

	mutex_lock(&q->lock);
	b = vcu_find_buf(q, iova, length, true);
	if (!b) {
		mutex_unlock(&q->lock);
		dev_info(vcu->dev, "[VCU] mmap: no buffer at iova 0x%llx\n", iova);
		return -EINVAL;
	}
	off = vcu_iova_rebase(iova, b->iova) - b->iova;

	if (b->dbuf) {
		ret = dma_buf_mmap(b->dbuf, vma, off >> PAGE_SHIFT);
		mutex_unlock(&q->lock);
		return ret;
	}

	/* dma_mmap_coherent() takes vm_pgoff as the offset into the buffer */
	vma->vm_pgoff = off >> PAGE_SHIFT;
	ret = dma_mmap_coherent(vcu->dev, vma, b->kva, b->iova, b->size);
	if (!ret) {
		vma->vm_ops = &vcu_buf_vm_ops;
		vma->vm_private_data = b;
		atomic_inc(&b->ref_cnt);
	}
	mutex_unlock(&q->lock);

	return ret;
}

static int mtk_vcu_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vcu_queue *q = file->private_data;
	struct mtk_vcu *vcu = q->vcu;
	unsigned long length = vma->vm_end - vma->vm_start;
	u64 pa_start = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 pa_end = pa_start + length;
	int ret;

	vcu_dbg("mmap start 0x%lx len 0x%lx off 0x%llx\n", vma->vm_start,
		length, pa_start);
	vcu_trace("mmap va 0x%lx len 0x%lx off 0x%llx gce_pa %d\n",
		  vma->vm_start, length, pa_start,
		  q->map_buf_pa >= MAP_SHMEM_PA_BASE);

	/* a PA_ALLOCATION is followed by the mmap of that GCE page */
	if (q->map_buf_pa >= MAP_SHMEM_PA_BASE) {
		struct vcu_pa_page *p;

		q->map_buf_pa = 0;
		mutex_lock(&q->lock);
		p = vcu_find_pa_page(q, pa_start, length);
		if (!p || pa_start != (u64)p->pa) {
			mutex_unlock(&q->lock);
			return -EINVAL;
		}
		vma->vm_pgoff = 0;
		ret = dma_mmap_coherent(vcu->gce_dev, vma, p->kva, p->pa,
					PAGE_SIZE);
		if (!ret) {
			vma->vm_ops = &vcu_page_vm_ops;
			vma->vm_private_data = p;
			atomic_inc(&p->ref_cnt);
		}
		mutex_unlock(&q->lock);
		return ret;
	}

	if (pa_start < MAP_PA_BASE_1GB)
		return vcu_mmap_reg(vcu, vma, pa_start, length);

	if ((pa_start >= MAP_SHMEM_ALLOC_BASE && pa_end <= MAP_SHMEM_ALLOC_END) ||
	    (pa_start >= MAP_SHMEM_COMMIT_BASE && pa_end <= MAP_SHMEM_COMMIT_END)) {
		u64 off = pa_start & (VCU_SHMEM_SIZE - 1);

		ret = remap_pfn_range(vma, vma->vm_start,
				      (virt_to_phys(vcu->dmem_va) + off) >> PAGE_SHIFT,
				      length, vma->vm_page_prot);
		if (!ret && pa_start >= MAP_SHMEM_COMMIT_BASE)
			vcu->dmem_vma = vma->vm_start - off;
		return ret;
	}

	if (pa_start >= MAP_SHMEM_MM_BASE && pa_start < MAP_SHMEM_PA_BASE) {
		u64 iova = pa_start - ((pa_start >= MAP_SHMEM_MM_CACHEABLE_BASE) ?
				       MAP_SHMEM_MM_CACHEABLE_BASE :
				       MAP_SHMEM_MM_BASE);

		return vcu_mmap_mm(q, vma, iova, length);
	}

	dev_info(vcu->dev, "[VCU] invalid mmap offset 0x%llx\n", pa_start);
	return -EINVAL;
}

static long mtk_vcu_unlocked_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct vcu_queue *q = file->private_data;
	struct mtk_vcu *vcu = q->vcu;
	void __user *uarg = (void __user *)arg;
	struct share_obj share;
	struct mem_obj mem;
	long ret;

	{
		static DECLARE_BITMAP(seen, 256);

		if (!test_and_set_bit(_IOC_NR(cmd) & 0xff, seen))
			vcu_trace_log("[VCU] first ioctl 0x%x nr %u size %u pid %d (%s)\n",
				 cmd, _IOC_NR(cmd), _IOC_SIZE(cmd),
				 current->pid, current->comm);
	}
	if (cmd != VCU_GET_OBJECT && cmd != VCU_GET_LOG_OBJECT)
		vcu_trace("ioctl 0x%x pid %d\n", cmd, current->pid);
	switch (cmd) {
	case VCU_SET_OBJECT:
		if (copy_from_user(&share, uarg, sizeof(share)))
			return -EFAULT;
		if (share.id >= (int)IPI_MAX || share.id < (int)IPI_VCU_INIT)
			return -EINVAL;
		ret = vcu_ipi_handler(vcu, &share);
		if (copy_to_user(uarg, &share, sizeof(share)))
			return -EFAULT;
		return ret;
	case VCU_GET_OBJECT:
		return vcu_ipi_get(vcu, uarg);
	case VCU_GET_LOG_OBJECT:
		return vcu_log_get(vcu, uarg);
	case VCU_MVA_ALLOCATION:
	case VCU_PA_ALLOCATION:
		if (copy_from_user(&mem, uarg, sizeof(mem)))
			return -EFAULT;
		ret = (cmd == VCU_MVA_ALLOCATION) ? vcu_mva_alloc(q, &mem) :
						    vcu_pa_alloc(q, &mem);
		if (ret) {
			mem.va = (u64)-1;
			mem.pa = (u64)-1;
			mem.iova = (u64)-1;
			if (copy_to_user(uarg, &mem, sizeof(mem)))
				return -EFAULT;
			pr_info("[VCU] allocation %u len %u failed %ld\n",
				_IOC_NR(cmd), mem.len, ret);
			return ret;
		}
		if (copy_to_user(uarg, &mem, sizeof(mem)))
			return -EFAULT;
		if (cmd == VCU_PA_ALLOCATION)
			q->map_buf_pa = mem.pa + MAP_SHMEM_PA_BASE;
		return 0;
	case VCU_MVA_FREE:
	case VCU_PA_FREE:
		if (copy_from_user(&mem, uarg, sizeof(mem)))
			return -EFAULT;
		if (mem.iova == 0 && mem.va == 0)
			return -EINVAL;
		ret = (cmd == VCU_MVA_FREE) ? vcu_mva_free(q, &mem) :
					      vcu_pa_free(q, &mem);
		if (ret)
			return -EINVAL;
		mem.va = 0;
		mem.iova = 0;
		mem.pa = 0;
		if (copy_to_user(uarg, &mem, sizeof(mem)))
			return -EFAULT;
		return 0;
	case VCU_CACHE_FLUSH_ALL:
		vcu_cache_flush_all(q);
		return 0;
	case VCU_CACHE_FLUSH_BUFF:
	case VCU_CACHE_INVALIDATE_BUFF:
		if (copy_from_user(&mem, uarg, sizeof(mem)))
			return -EFAULT;
		if (vcu_cache_sync(q, mem.iova, mem.len,
				   cmd == VCU_CACHE_FLUSH_BUFF))
			return -EINVAL;
		if (copy_to_user(uarg, &mem, sizeof(mem)))
			return -EFAULT;
		return 0;
	case VCU_GCE_SET_CMD_FLUSH:
		return vcu_gce_cmd_flush(vcu, q, uarg);
	case VCU_GCE_WAIT_CALLBACK:
		return vcu_wait_gce_callback(vcu, uarg);
	case VCU_SET_MMAP_TYPE:
		return 0;
	default:
		vcu_trace_log("[VCU] unknown ioctl 0x%x nr %u pid %d (%s)%s\n", cmd,
			 _IOC_NR(cmd), current->pid, current->comm,
			 stub_unknown_ioctl ? ", stubbed" : "");
		return stub_unknown_ioctl ? 0 : -ENOTTY;
	}
}

static const struct file_operations vcu_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mtk_vcu_unlocked_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= mtk_vcu_open,
	.release	= mtk_vcu_release,
	.mmap		= mtk_vcu_mmap,
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static void vcu_parse_gce_events(struct mtk_vcu *vcu)
{
	struct device_node *np = vcu->dev->of_node;
	unsigned int k;
	int idx;
	u32 eid;

	for (k = 0; k < GCE_EVENT_MAX; k++)
		vcu->gce_codec_eid[k] = -1;

	/* vendor binding: gce-events = <&gce id>, ... matched by name */
	for (k = 0; k < ARRAY_SIZE(vcu_gce_events); k++) {
		idx = of_property_match_string(np, "gce-event-names",
					       vcu_gce_events[k].name);
		if (idx < 0)
			continue;
		if (of_property_read_u32_index(np, "gce-events", idx * 2 + 1,
					       &eid))
			continue;
		vcu->gce_codec_eid[vcu_gce_events[k].id] = eid;
	}
}

static void vcu_free_gce(struct mtk_vcu *vcu)
{
	int i;

	if (vcu->gce_wq_work)
		cancel_delayed_work_sync(&vcu->gce_wdt);
	for (i = 0; i < GCE_THNUM_MAX; i++) {
		if (!IS_ERR_OR_NULL(vcu->clt_vdec[i]))
			cmdq_mbox_destroy(vcu->clt_vdec[i]);
		if (!IS_ERR_OR_NULL(vcu->clt_venc[i]))
			cmdq_mbox_destroy(vcu->clt_venc[i]);
		vcu->clt_vdec[i] = NULL;
		vcu->clt_venc[i] = NULL;
	}
	if (vcu->gce_wq_work)
		destroy_workqueue(vcu->gce_wq_work);
	vcu->gce_wq_work = NULL;
}

static int vcu_setup_gce(struct mtk_vcu *vcu)
{
	struct device *dev = vcu->dev;
	u32 num;
	int i;

	vcu->gce_th_num[VCU_VDEC] = 1;
	vcu->gce_th_num[VCU_VENC] = 1;
	if (!of_property_read_u32(dev->of_node, "mediatek,dec_gce_th_num", &num) &&
	    num >= 1 && num <= GCE_THNUM_MAX)
		vcu->gce_th_num[VCU_VDEC] = num;
	if (!of_property_read_u32(dev->of_node, "mediatek,enc_gce_th_num", &num) &&
	    num >= 1 && num <= GCE_THNUM_MAX)
		vcu->gce_th_num[VCU_VENC] = num;

	INIT_DELAYED_WORK(&vcu->gce_wdt, vcu_gce_wdt_work);
	vcu->gce_wq_work = alloc_ordered_workqueue("mtk-vcu-gce", WQ_MEM_RECLAIM);
	if (!vcu->gce_wq_work)
		return -ENOMEM;

	for (i = 0; i < vcu->gce_th_num[VCU_VDEC]; i++) {
		vcu->clt_vdec[i] = cmdq_mbox_create(dev, i);
		if (IS_ERR(vcu->clt_vdec[i]))
			goto err;
		vcu->clt_vdec[i]->client.rx_callback = vcu_gce_rx_callback;
	}
	for (i = 0; i < vcu->gce_th_num[VCU_VENC]; i++) {
		vcu->clt_venc[i] = cmdq_mbox_create(dev,
					i + vcu->gce_th_num[VCU_VDEC]);
		if (IS_ERR(vcu->clt_venc[i]))
			goto err;
		vcu->clt_venc[i]->client.rx_callback = vcu_gce_rx_callback;
	}
	vcu->gce_dev = vcu->clt_vdec[0]->chan->mbox->dev;
	vcu_parse_gce_events(vcu);

	return 0;
err:
	vcu_free_gce(vcu);
	return -EPROBE_DEFER;
}

static void vcu_kvfree(void *p)
{
	kvfree(p);
}

static int mtk_vcu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_vcu *vcu;
	struct resource *res;
	u32 vcuid = 0;
	int i, ret;

	if (!enable) {
		dev_info(dev, "[VCU] disabled (boot with mtk_vcu.enable=1)\n");
		return -ENODEV;
	}

	if (of_property_read_u32(dev->of_node, "mediatek,vcuid", &vcuid) ||
	    vcuid != 0) {
		dev_info(dev, "[VCU] only the video VCU (vcuid 0) is supported\n");
		return -ENODEV;
	}
	if (vcu_ptr)
		return -EBUSY;

	vcu = devm_kzalloc(dev, sizeof(*vcu), GFP_KERNEL);
	if (!vcu)
		return -ENOMEM;
	vcu->dev = dev;
	vt_init(dev);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;

	for (i = 0; i < VCU_MAP_HW_REG_NUM; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			break;
		vcu->map_base[i].base = res->start;
		vcu->map_base[i].len = resource_size(res);
	}

	for (i = 0; i < VCU_CODEC_MAX; i++) {
		mutex_init(&vcu->vcu_mutex[i]);
		mutex_init(&vcu->vcu_gce_mutex[i]);
		mutex_init(&vcu->gce_cmds_mutex[i]);
		mutex_init(&vcu->ctx_ipi_binding[i]);
		init_waitqueue_head(&vcu->ack_wq[i]);
		init_waitqueue_head(&vcu->get_wq[i]);
		init_waitqueue_head(&vcu->gce_wq[i]);
		init_rwsem(&vcu->gce_chan_sem[i]);
		atomic_set(&vcu->ipi_got[i], 0);
		atomic_set(&vcu->ipi_done[i], 1);
		vcu->gce_cmds[i] = kvzalloc(sizeof(struct gce_cmds),
					    GFP_KERNEL);
		if (!vcu->gce_cmds[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(dev, vcu_kvfree,
					       vcu->gce_cmds[i]);
		if (ret)
			return ret;
	}
	mutex_init(&vcu->vcu_share);
	init_waitqueue_head(&vcu->reg_map_wq);
	init_waitqueue_head(&vcu->vdec_log_get_wq);
	atomic_set(&vcu->vdec_log_got, 0);
	sema_init(&vcu->vpud_killed, 1);
	for (i = 0; i < VCODEC_INST_MAX; i++)
		vcu_gce_clear_inst(&vcu->gce_info[i]);

	vcu->vdec_log_info = devm_kzalloc(dev, sizeof(*vcu->vdec_log_info),
					  GFP_KERNEL);
	/* page aligned and physically contiguous for remap_pfn_range() */
	vcu->dmem_va = (void *)devm_get_free_pages(dev, GFP_KERNEL | __GFP_ZERO,
						   get_order(VCU_SHMEM_SIZE));
	if (!vcu->vdec_log_info || !vcu->dmem_va)
		return -ENOMEM;

	ret = vcu_setup_gce(vcu);
	if (ret)
		return dev_err_probe(dev, ret, "[VCU] GCE mailbox not ready\n");

	platform_set_drvdata(pdev, vcu);
	ret = vcu_ipi_register(pdev, IPI_VCU_INIT, vcu_init_ipi_handler,
			       "vcu_init", vcu);
	if (ret)
		goto err_gce;

	vcu->misc.minor = MISC_DYNAMIC_MINOR;
	vcu->misc.name = "vcu";
	vcu->misc.fops = &vcu_fops;
	vcu->misc.parent = dev;
	vcu_ptr = vcu;
	ret = misc_register(&vcu->misc);
	if (ret) {
		vcu_ptr = NULL;
		goto err_gce;
	}

	dev_info(dev, "[VCU] /dev/vcu ready, gce threads dec %d enc %d\n",
		 vcu->gce_th_num[VCU_VDEC], vcu->gce_th_num[VCU_VENC]);

	return 0;

err_gce:
	platform_set_drvdata(pdev, NULL);
	vcu_free_gce(vcu);
	return ret;
}

static int mtk_vcu_suspend(struct device *dev)
{
	struct mtk_vcu *vcu = dev_get_drvdata(dev);

	if (atomic_read(&vcu->ipi_done[VCU_VDEC]) == 0 ||
	    atomic_read(&vcu->ipi_done[VCU_VENC]) == 0 || vcu_gce_busy(vcu)) {
		dev_info(dev, "[VCU] suspend refused, codec busy\n");
		return -EBUSY;
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mtk_vcu_pm_ops, mtk_vcu_suspend, NULL);

static const struct of_device_id mtk_vcu_match[] = {
	{ .compatible = "mediatek-vcu" },
	{ .compatible = "mediatek,mt6853-vcu" },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_vcu_match);

static struct platform_driver mtk_vcu_driver = {
	.probe = mtk_vcu_probe,
	.driver = {
		.name = "mtk_vcu",
		.pm = pm_sleep_ptr(&mtk_vcu_pm_ops),
		.of_match_table = mtk_vcu_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mtk_vcu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek Video Communication And Controller Unit driver");
MODULE_IMPORT_NS("DMA_BUF");
