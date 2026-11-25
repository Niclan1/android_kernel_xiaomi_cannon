// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy MediaTek ION ABI (4.14 vendor kernel) on top of DMA-BUF heaps.
 *
 * The prebuilt MT6853 camera HAL and dpframework talk to /dev/ion with the
 * old handle based ION ioctls plus the MediaTek ION_IOC_CUSTOM extensions.
 * Buffers are allocated from the "system" DMA-BUF heap and exported as
 * ordinary dma-buf fds, so they can be passed to any 6.18 consumer.
 *
 * ION_MM_GET_IOVA*: when the DT has a "mediatek,mtk-ion-compat-iova" node
 * whose "iommus" points at a port of the M4U camera/MDP region (8G-12G,
 * larbs 2/9/11/13/14/16-20), the buffer is attached to that node and mapped
 * through mtk_iommu. All region users share one IOMMU domain, so the IOVA
 * is valid for every port of the region (the 4.14 MTK HAL programs it
 * straight into ISP/MDP DMA registers). Other ports, secure buffers and
 * physical address queries (ION_SYS_GET_PHYS) return -EOPNOTSUPP.
 * As on 4.14, the IOVA belongs to the buffer, not to the handle: the HAL
 * does IMPORT, GET_IOVA, FREE and keeps programming the IOVA. Mappings are
 * cached per dma-buf (holding a reference) and dropped once ours is the
 * last reference to the dma-buf file.
 *
 * Userspace caveat: Codec2's C2PlatformAllocatorStore uses its legacy ION
 * allocator whenever stat("/dev/ion") succeeds and, in processes without a
 * component store, dereferences NULL in UseComponentStoreForIonAllocator.
 * DAC modes do not help (stat needs no access to the node). The ROM must
 * set debug.c2.use_dmabufheaps=1 and/or give /dev/ion an SELinux type that
 * only the camera HAL may getattr, so stat() fails everywhere else.
 *
 * libion's ion_is_legacy() issues ION_IOC_FREE(0) and treats -ENOTTY as
 * "modern ION"; libdmabufheap then requires ION_IOC_HEAP_QUERY. FREE(0)
 * returns -EINVAL here for 64- and 32-bit callers, so both see legacy ION
 * and skip the heap query (which is implemented anyway).
 */

#define pr_fmt(fmt) "mtk_ion_compat: " fmt

#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "register /dev/ion (legacy MediaTek ION ABI)");

/* ---- 4.14 uapi (drivers/staging/android/uapi/ion.h, LP64 layout) ---- */

struct ion_allocation_data {
	size_t len;
	size_t align;
	u32 heap_id_mask;
	u32 flags;
	int handle;
};

struct ion_fd_data {
	int handle;
	int fd;
};

struct ion_handle_data {
	int handle;
};

struct ion_custom_data {
	u32 cmd;
	unsigned long arg;
};

#define MAX_HEAP_NAME 32

struct ion_heap_data {
	char name[MAX_HEAP_NAME];
	u32 type;
	u32 heap_id;
	u32 reserved0;
	u32 reserved1;
	u32 reserved2;
};

struct ion_heap_query {
	u32 cnt;
	u32 reserved0;
	u64 heaps;
	u32 reserved1;
	u32 reserved2;
};

#define ION_IOC_MAGIC		'I'
#define ION_IOC_ALLOC		_IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE		_IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_MAP		_IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_IOC_SHARE		_IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ION_IOC_IMPORT		_IOWR(ION_IOC_MAGIC, 5, struct ion_fd_data)
#define ION_IOC_CUSTOM		_IOWR(ION_IOC_MAGIC, 6, struct ion_custom_data)
#define ION_IOC_SYNC		_IOWR(ION_IOC_MAGIC, 7, struct ion_fd_data)
#define ION_IOC_HEAP_QUERY	_IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)

#define ION_FLAG_CACHED		BIT(0)

/* mtk_ion.h heap types; heap_id_mask bits are BIT(type) */
#define ION_HEAP_TYPE_SYSTEM			0
#define ION_HEAP_TYPE_SYSTEM_CONTIG		1
#define ION_HEAP_TYPE_DMA			4
#define ION_HEAP_TYPE_MULTIMEDIA		10
#define ION_HEAP_TYPE_FB			11
#define ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA	12
#define ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA	14

#define COMPAT_HEAP_MASK (BIT(ION_HEAP_TYPE_SYSTEM) |			\
			  BIT(ION_HEAP_TYPE_SYSTEM_CONTIG) |		\
			  BIT(ION_HEAP_TYPE_DMA) |			\
			  BIT(ION_HEAP_TYPE_MULTIMEDIA) |		\
			  BIT(ION_HEAP_TYPE_FB) |			\
			  BIT(ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA) |	\
			  BIT(ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA))

/* ---- MediaTek ION_IOC_CUSTOM (ion/mtk/ion_drv.h) ---- */

enum { ION_CMD_SYSTEM, ION_CMD_MULTIMEDIA, ION_CMD_MULTIMEDIA_SEC };

enum {
	ION_MM_CONFIG_BUFFER,
	ION_MM_SET_DEBUG_INFO,
	ION_MM_GET_DEBUG_INFO,
	ION_MM_SET_SF_BUF_INFO,
	ION_MM_GET_SF_BUF_INFO,
	ION_MM_CONFIG_BUFFER_EXT,
	ION_MM_ACQ_CACHE_POOL,
	ION_MM_QRY_CACHE_POOL,
	ION_MM_GET_IOVA,
	ION_MM_GET_IOVA_EXT,
};

enum {
	ION_SYS_CACHE_SYNC,
	ION_SYS_GET_PHYS,
	ION_SYS_GET_CLIENT,
	ION_SYS_SET_HANDLE_BACKTRACE,
	ION_SYS_SET_CLIENT_NAME,
	ION_SYS_DMA_OP,
};

enum {
	ION_CACHE_CLEAN_BY_RANGE,
	ION_CACHE_INVALID_BY_RANGE,
	ION_CACHE_FLUSH_BY_RANGE,
	ION_CACHE_CLEAN_BY_RANGE_USE_PA,
	ION_CACHE_INVALID_BY_RANGE_USE_PA,
	ION_CACHE_FLUSH_BY_RANGE_USE_PA,
	ION_CACHE_CLEAN_ALL,
	ION_CACHE_INVALID_ALL,
	ION_CACHE_FLUSH_ALL,
};

#define ION_MM_DBG_NAME_LEN	48
#define ION_MM_SF_BUF_INFO_LEN	16
#define BACKTRACE_SIZE		10

/* the handle is a union with a kernel pointer in the vendor headers */
union ion_uhandle {
	int handle;
	u64 kernel_handle;
};

struct ion_sys_cache_sync_param {
	union ion_uhandle h;
	u64 va;
	u32 size;
	u32 sync_type;
	u64 iova;
};

struct ion_sys_get_phys_param {
	union ion_uhandle h;
	unsigned long phy_addr;
	unsigned long len;
};

struct ion_sys_record_param {
	s32 group_id;
	s32 pid;
	u32 action;
	u32 address_type;
	u32 address;
	u32 length;
	u32 backtrace[BACKTRACE_SIZE];
	u32 backtrace_num;
	u64 handle;
	u64 client;
	u64 buffer;
	u64 file;
	int fd;
};

struct ion_dma_param {
	union ion_uhandle h;
	u64 va;
	u32 size;
	u32 dma_type;
	u32 dma_dir;
};

struct ion_sys_data {
	u32 sys_cmd;
	union {
		struct ion_sys_cache_sync_param cache_sync_param;
		struct ion_sys_get_phys_param get_phys_param;
		u32 client;
		char client_name[ION_MM_DBG_NAME_LEN];
		struct ion_sys_record_param record_param;
		struct ion_dma_param dma_param;
	};
};

struct ion_mm_config_buffer_param {
	union ion_uhandle h;
	int module_id;
	u32 security;
	u32 coherent;
	u32 reserve_iova_start;
	u32 reserve_iova_end;
};

struct ion_mm_buf_debug_info {
	union ion_uhandle h;
	char dbg_name[ION_MM_DBG_NAME_LEN];
	u32 value1;
	u32 value2;
	u32 value3;
	u32 value4;
};

struct ion_mm_sf_buf_info {
	union ion_uhandle h;
	u32 info[ION_MM_SF_BUF_INFO_LEN];
};

struct ion_mm_pool_info {
	size_t len;
	size_t align;
	u32 heap_id_mask;
	u32 flags;
	u32 ret;
};

struct ion_mm_get_iova_param {
	union ion_uhandle h;
	int module_id;
	u32 security;
	u32 coherent;
	u32 reserve_iova_start;
	u32 reserve_iova_end;
	u64 phy_addr;
	unsigned long len;
};

struct ion_mm_data {
	u32 mm_cmd;
	union {
		struct ion_mm_config_buffer_param config_buffer_param;
		struct ion_mm_buf_debug_info buf_debug_info_param;
		struct ion_mm_sf_buf_info sf_buf_info_param;
		struct ion_mm_pool_info pool_info_param;
		struct ion_mm_get_iova_param get_phys_param;
	};
};

static_assert(sizeof(struct ion_allocation_data) == 32);
static_assert(sizeof(struct ion_custom_data) == 16);
static_assert(sizeof(struct ion_heap_query) == 24);
static_assert(sizeof(struct ion_heap_data) == 52);
static_assert(sizeof(struct ion_mm_data) == 80);
static_assert(sizeof(struct ion_sys_data) == 120);

/* ---- implementation ---- */

struct ion_compat_buf {
	struct dma_buf *dmabuf;
	unsigned int refs;
	char dbg_name[ION_MM_DBG_NAME_LEN];
	u32 dbg_value[4];
	u32 sf_info[ION_MM_SF_BUF_INFO_LEN];
};

struct ion_compat_client {
	struct mutex lock;
	struct xarray handles;
};

static struct device *iova_dev;

static struct ion_compat_buf *ion_compat_lookup(struct ion_compat_client *c,
						int handle);

/* larbs whose ports translate through the 8G-12G camera/MDP domain */
#define ION_COMPAT_IOVA_LARBS \
	(BIT(2) | BIT(9) | BIT(11) | BIT(13) | BIT(14) | BIT(16) | BIT(17) | \
	 BIT(18) | BIT(19) | BIT(20))

struct ion_compat_iova {
	struct list_head node;
	struct dma_buf *dmabuf;		/* holds a reference */
	struct dma_buf_attachment *att;
	struct sg_table *sgt;
	dma_addr_t iova;
};

static LIST_HEAD(iova_cache);
static DEFINE_MUTEX(iova_lock);

#define ION_COMPAT_IOVA_REAP_MS 1000

static void ion_compat_iova_reap_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(iova_reap_work, ion_compat_iova_reap_fn);

static void ion_compat_iova_drop(struct ion_compat_iova *e)
{
	list_del(&e->node);
	dma_buf_unmap_attachment_unlocked(e->att, e->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(e->dmabuf, e->att);
	dma_buf_put(e->dmabuf);
	kfree(e);
}

/* caller holds iova_lock; drops mappings of buffers nobody else holds */
static void ion_compat_iova_reap(void)
{
	struct ion_compat_iova *e, *tmp;

	list_for_each_entry_safe(e, tmp, &iova_cache, node)
		if (file_count(e->dmabuf->file) == 1)
			ion_compat_iova_drop(e);
	if (!list_empty(&iova_cache))
		schedule_delayed_work(&iova_reap_work,
				      msecs_to_jiffies(ION_COMPAT_IOVA_REAP_MS));
}

static void ion_compat_iova_reap_fn(struct work_struct *work)
{
	mutex_lock(&iova_lock);
	ion_compat_iova_reap();
	mutex_unlock(&iova_lock);
}

static int ion_compat_map_iova(struct dma_buf *dmabuf, dma_addr_t *iova)
{
	struct dma_buf_attachment *att;
	struct ion_compat_iova *e;
	struct scatterlist *sg;
	struct sg_table *sgt;
	dma_addr_t next;
	int i, ret = 0;

	mutex_lock(&iova_lock);
	list_for_each_entry(e, &iova_cache, node) {
		if (e->dmabuf == dmabuf) {
			*iova = e->iova;
			goto out;
		}
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		ret = -ENOMEM;
		goto out;
	}
	att = dma_buf_attach(dmabuf, iova_dev);
	if (IS_ERR(att)) {
		ret = PTR_ERR(att);
		goto err_free;
	}
	sgt = dma_buf_map_attachment_unlocked(att, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	/* the HAL wants one linear IOVA range */
	next = sg_dma_address(sgt->sgl);
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != next) {
			ret = -EINVAL;
			goto err_unmap;
		}
		next += sg_dma_len(sg);
	}

	get_dma_buf(dmabuf);
	e->dmabuf = dmabuf;
	e->att = att;
	e->sgt = sgt;
	e->iova = sg_dma_address(sgt->sgl);
	list_add(&e->node, &iova_cache);
	*iova = e->iova;
	ion_compat_iova_reap();
	goto out;

err_unmap:
	dma_buf_unmap_attachment_unlocked(att, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dmabuf, att);
err_free:
	kfree(e);
out:
	mutex_unlock(&iova_lock);
	return ret;
}

static int ion_compat_get_iova(struct ion_compat_client *c,
			       struct ion_mm_get_iova_param *p)
{
	unsigned int larb = (unsigned int)p->module_id >> 5;
	struct dma_buf *dmabuf = NULL;
	struct ion_compat_buf *b;
	dma_addr_t iova;
	int ret;

	if (!iova_dev || p->module_id < 0 || larb > 31 ||
	    !(ION_COMPAT_IOVA_LARBS & BIT(larb)) || p->security) {
		pr_warn_ratelimited("%s: ION_MM_GET_IOVA (module %d sec %u) not supported\n",
				    current->comm, p->module_id, p->security);
		return -EOPNOTSUPP;
	}

	mutex_lock(&c->lock);
	b = ion_compat_lookup(c, p->h.handle);
	if (b) {
		dmabuf = b->dmabuf;
		get_dma_buf(dmabuf);
	}
	mutex_unlock(&c->lock);
	if (!dmabuf)
		return -EINVAL;

	ret = ion_compat_map_iova(dmabuf, &iova);
	if (!ret) {
		p->phy_addr = iova;
		p->len = dmabuf->size;
	} else {
		pr_warn_ratelimited("%s: ION_MM_GET_IOVA map failed %d\n",
				    current->comm, ret);
	}
	dma_buf_put(dmabuf);
	return ret;
}

static struct dma_heap *sys_heap;
static struct dma_heap *sys_uncached_heap;
static DEFINE_MUTEX(heap_lock);

static const struct {
	const char *name;
	u32 type;
} compat_heaps[] = {
	{ "ion_system_heap", ION_HEAP_TYPE_SYSTEM },
	{ "ion_mm_heap", ION_HEAP_TYPE_MULTIMEDIA },
	{ "ion_fb_heap", ION_HEAP_TYPE_FB },
	{ "ion_mm_heap_for_camera", ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA },
};

static struct dma_heap *ion_compat_pick_heap(bool cached)
{
	struct dma_heap *heap;

	mutex_lock(&heap_lock);
	if (!sys_heap)
		sys_heap = dma_heap_find("system");
	if (!sys_uncached_heap)
		sys_uncached_heap = dma_heap_find("system-uncached");
	heap = (!cached && sys_uncached_heap) ? sys_uncached_heap : sys_heap;
	mutex_unlock(&heap_lock);
	return heap;
}

/* caller holds client->lock */
static struct ion_compat_buf *ion_compat_lookup(struct ion_compat_client *c,
						int handle)
{
	if (handle <= 0)
		return NULL;
	return xa_load(&c->handles, handle);
}

/* takes ownership of the dmabuf reference */
static int ion_compat_insert(struct ion_compat_client *c,
			     struct dma_buf *dmabuf, int *handle)
{
	struct ion_compat_buf *b;
	unsigned long idx;
	u32 id;
	int ret;

	mutex_lock(&c->lock);
	xa_for_each(&c->handles, idx, b) {
		if (b->dmabuf == dmabuf) {
			b->refs++;
			*handle = idx;
			mutex_unlock(&c->lock);
			dma_buf_put(dmabuf);
			return 0;
		}
	}

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		ret = -ENOMEM;
		goto err;
	}
	b->dmabuf = dmabuf;
	b->refs = 1;
	ret = xa_alloc(&c->handles, &id, b, XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	if (ret) {
		kfree(b);
		goto err;
	}
	*handle = id;
	mutex_unlock(&c->lock);
	return 0;
err:
	mutex_unlock(&c->lock);
	dma_buf_put(dmabuf);
	return ret;
}

static int ion_compat_alloc(struct ion_compat_client *c,
			    struct ion_allocation_data *d)
{
	struct dma_heap *heap;
	struct dma_buf *dmabuf;

	if (!d->len || d->len > SZ_1G)
		return -EINVAL;
	if (!(d->heap_id_mask & COMPAT_HEAP_MASK)) {
		pr_warn_ratelimited("%s: unsupported heap mask 0x%x\n",
				    current->comm, d->heap_id_mask);
		return -ENODEV;
	}

	heap = ion_compat_pick_heap(d->flags & ION_FLAG_CACHED);
	if (!heap)
		return -ENODEV;

	dmabuf = dma_heap_buffer_alloc(heap, d->len, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	return ion_compat_insert(c, dmabuf, &d->handle);
}

static int ion_compat_free(struct ion_compat_client *c, int handle)
{
	struct ion_compat_buf *b;
	struct dma_buf *put = NULL;

	mutex_lock(&c->lock);
	b = ion_compat_lookup(c, handle);
	if (!b) {
		mutex_unlock(&c->lock);
		return -EINVAL;
	}
	if (--b->refs == 0) {
		xa_erase(&c->handles, handle);
		put = b->dmabuf;
		kfree(b);
	}
	mutex_unlock(&c->lock);
	if (put)
		dma_buf_put(put);
	return 0;
}

static int ion_compat_share(struct ion_compat_client *c, int handle)
{
	struct ion_compat_buf *b;
	struct dma_buf *dmabuf;
	int fd;

	mutex_lock(&c->lock);
	b = ion_compat_lookup(c, handle);
	if (!b) {
		mutex_unlock(&c->lock);
		return -EINVAL;
	}
	dmabuf = b->dmabuf;
	get_dma_buf(dmabuf);
	mutex_unlock(&c->lock);

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);
	return fd;
}

static struct dma_buf *ion_compat_get_buf(struct ion_compat_client *c,
					  int handle)
{
	struct ion_compat_buf *b;
	struct dma_buf *dmabuf = NULL;

	mutex_lock(&c->lock);
	b = ion_compat_lookup(c, handle);
	if (b) {
		dmabuf = b->dmabuf;
		get_dma_buf(dmabuf);
	}
	mutex_unlock(&c->lock);
	return dmabuf;
}

static int ion_compat_cache_sync(struct dma_buf *dmabuf, u32 type)
{
	int ret = 0;

	switch (type) {
	case ION_CACHE_CLEAN_BY_RANGE:
	case ION_CACHE_CLEAN_BY_RANGE_USE_PA:
		ret = dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
		break;
	case ION_CACHE_INVALID_BY_RANGE:
	case ION_CACHE_INVALID_BY_RANGE_USE_PA:
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		break;
	case ION_CACHE_FLUSH_BY_RANGE:
	case ION_CACHE_FLUSH_BY_RANGE_USE_PA:
		ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		if (!ret)
			ret = dma_buf_begin_cpu_access(dmabuf,
						       DMA_BIDIRECTIONAL);
		break;
	default:
		break;
	}
	return ret;
}

static int ion_compat_custom_sys(struct ion_compat_client *c,
				 struct ion_sys_data *d)
{
	struct dma_buf *dmabuf;
	int ret;

	switch (d->sys_cmd) {
	case ION_SYS_CACHE_SYNC:
		if (d->cache_sync_param.sync_type >= ION_CACHE_CLEAN_ALL)
			return 0;
		dmabuf = ion_compat_get_buf(c, d->cache_sync_param.h.handle);
		if (!dmabuf)
			return -EINVAL;
		ret = ion_compat_cache_sync(dmabuf,
					    d->cache_sync_param.sync_type);
		dma_buf_put(dmabuf);
		return ret;
	case ION_SYS_GET_PHYS:
		pr_warn_ratelimited("%s: ION_SYS_GET_PHYS not supported\n",
				    current->comm);
		return -EOPNOTSUPP;
	case ION_SYS_GET_CLIENT:
		d->client = task_tgid_nr(current);
		return 0;
	case ION_SYS_SET_HANDLE_BACKTRACE:
	case ION_SYS_SET_CLIENT_NAME:
	case ION_SYS_DMA_OP:
		return 0;
	default:
		return -EINVAL;
	}
}

static int ion_compat_custom_mm(struct ion_compat_client *c,
				struct ion_mm_data *d)
{
	struct ion_mm_buf_debug_info *dbg = &d->buf_debug_info_param;
	struct ion_mm_sf_buf_info *sf = &d->sf_buf_info_param;
	struct ion_compat_buf *b;
	int ret = 0;

	switch (d->mm_cmd) {
	case ION_MM_CONFIG_BUFFER:
	case ION_MM_CONFIG_BUFFER_EXT:
		mutex_lock(&c->lock);
		b = ion_compat_lookup(c, d->config_buffer_param.h.handle);
		mutex_unlock(&c->lock);
		return b ? 0 : -EINVAL;
	case ION_MM_SET_DEBUG_INFO:
	case ION_MM_GET_DEBUG_INFO:
		mutex_lock(&c->lock);
		b = ion_compat_lookup(c, dbg->h.handle);
		if (!b) {
			ret = -EINVAL;
		} else if (d->mm_cmd == ION_MM_SET_DEBUG_INFO) {
			strscpy(b->dbg_name, dbg->dbg_name,
				sizeof(b->dbg_name));
			b->dbg_value[0] = dbg->value1;
			b->dbg_value[1] = dbg->value2;
			b->dbg_value[2] = dbg->value3;
			b->dbg_value[3] = dbg->value4;
		} else {
			memcpy(dbg->dbg_name, b->dbg_name,
			       sizeof(dbg->dbg_name));
			dbg->value1 = b->dbg_value[0];
			dbg->value2 = b->dbg_value[1];
			dbg->value3 = b->dbg_value[2];
			dbg->value4 = b->dbg_value[3];
		}
		mutex_unlock(&c->lock);
		return ret;
	case ION_MM_SET_SF_BUF_INFO:
	case ION_MM_GET_SF_BUF_INFO:
		mutex_lock(&c->lock);
		b = ion_compat_lookup(c, sf->h.handle);
		if (!b)
			ret = -EINVAL;
		else if (d->mm_cmd == ION_MM_SET_SF_BUF_INFO)
			memcpy(b->sf_info, sf->info, sizeof(b->sf_info));
		else
			memcpy(sf->info, b->sf_info, sizeof(sf->info));
		mutex_unlock(&c->lock);
		return ret;
	case ION_MM_ACQ_CACHE_POOL:
	case ION_MM_QRY_CACHE_POOL:
		d->pool_info_param.ret = 0;
		return 0;
	case ION_MM_GET_IOVA:
	case ION_MM_GET_IOVA_EXT:
		return ion_compat_get_iova(c, &d->get_phys_param);
	default:
		return -EINVAL;
	}
}

static int ion_compat_custom(struct ion_compat_client *c,
			     struct ion_custom_data *cd)
{
	void __user *uarg = (void __user *)cd->arg;
	union {
		struct ion_sys_data sys;
		struct ion_mm_data mm;
	} *u;
	size_t sz;
	int ret;

	switch (cd->cmd) {
	case ION_CMD_SYSTEM:
		sz = sizeof(struct ion_sys_data);
		break;
	case ION_CMD_MULTIMEDIA:
		sz = sizeof(struct ion_mm_data);
		break;
	default:
		return -EOPNOTSUPP;
	}

	u = kzalloc(sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;
	if (copy_from_user(u, uarg, sz)) {
		ret = -EFAULT;
		goto out;
	}

	if (cd->cmd == ION_CMD_SYSTEM)
		ret = ion_compat_custom_sys(c, &u->sys);
	else
		ret = ion_compat_custom_mm(c, &u->mm);

	if (!ret && copy_to_user(uarg, u, sz))
		ret = -EFAULT;
out:
	kfree(u);
	return ret;
}

static int ion_compat_heap_query(struct ion_heap_query *q)
{
	struct ion_heap_data hd;
	void __user *uheaps = u64_to_user_ptr(q->heaps);
	u32 i, n = ARRAY_SIZE(compat_heaps);

	if (!q->heaps) {
		q->cnt = n;
		return 0;
	}
	n = min(n, q->cnt);
	for (i = 0; i < n; i++) {
		memset(&hd, 0, sizeof(hd));
		strscpy(hd.name, compat_heaps[i].name, sizeof(hd.name));
		hd.type = compat_heaps[i].type;
		hd.heap_id = compat_heaps[i].type;
		if (copy_to_user(uheaps + i * sizeof(hd), &hd, sizeof(hd)))
			return -EFAULT;
	}
	q->cnt = n;
	return 0;
}

static long ion_compat_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct ion_compat_client *c = filp->private_data;
	void __user *uarg = (void __user *)arg;
	union {
		struct ion_allocation_data alloc;
		struct ion_fd_data fd;
		struct ion_handle_data handle;
		struct ion_custom_data custom;
		struct ion_heap_query query;
	} d;
	struct dma_buf *dmabuf;
	int ret;

	if (_IOC_TYPE(cmd) != ION_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_SIZE(cmd) > sizeof(d))
		return -EINVAL;
	memset(&d, 0, sizeof(d));
	if ((_IOC_DIR(cmd) & _IOC_WRITE) &&
	    copy_from_user(&d, uarg, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case ION_IOC_ALLOC:
		ret = ion_compat_alloc(c, &d.alloc);
		break;
	case ION_IOC_FREE:
		/* libion probes legacy ION with FREE(0): must not be -ENOTTY */
		return ion_compat_free(c, d.handle.handle);
	case ION_IOC_MAP:
	case ION_IOC_SHARE:
		ret = ion_compat_share(c, d.fd.handle);
		if (ret < 0)
			return ret;
		d.fd.fd = ret;
		ret = 0;
		break;
	case ION_IOC_IMPORT:
		dmabuf = dma_buf_get(d.fd.fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);
		ret = ion_compat_insert(c, dmabuf, &d.fd.handle);
		break;
	case ION_IOC_SYNC:
		dmabuf = dma_buf_get(d.fd.fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);
		ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		dma_buf_put(dmabuf);
		return ret;
	case ION_IOC_CUSTOM:
		return ion_compat_custom(c, &d.custom);
	case ION_IOC_HEAP_QUERY:
		ret = ion_compat_heap_query(&d.query);
		break;
	default:
		return -ENOTTY;
	}

	if (ret)
		return ret;
	if ((_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user(uarg, &d, _IOC_SIZE(cmd))) {
		/* don't leak the handle/fd the caller never saw */
		if (cmd == ION_IOC_ALLOC || cmd == ION_IOC_IMPORT)
			ion_compat_free(c, cmd == ION_IOC_ALLOC ?
					d.alloc.handle : d.fd.handle);
		return -EFAULT;
	}
	return 0;
}

#ifdef CONFIG_COMPAT
/*
 * ILP32 layout of the same uapi (4.14 compat_ion.c). Without this every
 * 32-bit caller got -ENOTTY: libion's ion_is_legacy() FREE(0) probe then
 * reports "modern ION" and libdmabufheap aborts on the missing heap query.
 */
struct compat_ion_allocation_data {
	compat_size_t len;
	compat_size_t align;
	u32 heap_id_mask;
	u32 flags;
	s32 handle;
};

struct compat_ion_custom_data {
	u32 cmd;
	compat_ulong_t arg;
};

struct compat_ion_sys_data {
	u32 sys_cmd;
	union {
		struct {
			s32 handle;
			compat_uptr_t va;
			compat_size_t size;
			u32 sync_type;
			u64 iova;
		} cache_sync_param;
		struct {
			s32 handle;
			u32 phy_addr;
			compat_size_t len;
		} get_phys_param;
		u32 client;
		char client_name[ION_MM_DBG_NAME_LEN];
	};
};

struct compat_ion_mm_data {
	u32 mm_cmd;
	union {
		struct {
			s32 handle;
			s32 module_id;
			u32 security;
			u32 coherent;
			u32 reserve_iova_start;
			u32 reserve_iova_end;
		} config_buffer_param;
		struct {
			s32 handle;
			char dbg_name[ION_MM_DBG_NAME_LEN];
			u32 value[4];
		} buf_debug_info_param;
		struct {
			s32 handle;
			u32 info[ION_MM_SF_BUF_INFO_LEN];
		} sf_buf_info_param;
		struct {
			compat_size_t len;
			compat_size_t align;
			u32 heap_id_mask;
			u32 flags;
			u32 ret;
		} pool_info_param;
		struct {
			s32 handle;
			s32 module_id;
			u32 security;
			u32 coherent;
			u32 reserve_iova_start;
			u32 reserve_iova_end;
			u64 phy_addr;
			compat_size_t len;
		} get_phys_param;
	};
};

static_assert(sizeof(struct compat_ion_allocation_data) == 20);
static_assert(sizeof(struct compat_ion_custom_data) == 8);
static_assert(sizeof(struct compat_ion_sys_data) == 56);
static_assert(sizeof(struct compat_ion_mm_data) == 80);

#define COMPAT_ION_IOC_ALLOC	_IOWR(ION_IOC_MAGIC, 0, struct compat_ion_allocation_data)
#define COMPAT_ION_IOC_CUSTOM	_IOWR(ION_IOC_MAGIC, 6, struct compat_ion_custom_data)

static int ion_compat_custom_sys32(struct ion_compat_client *c,
				   void __user *uarg)
{
	struct compat_ion_sys_data d32;
	struct ion_sys_data *d;
	int ret;

	if (copy_from_user(&d32, uarg, sizeof(d32)))
		return -EFAULT;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->sys_cmd = d32.sys_cmd;
	switch (d32.sys_cmd) {
	case ION_SYS_CACHE_SYNC:
		d->cache_sync_param.h.handle = d32.cache_sync_param.handle;
		d->cache_sync_param.va = d32.cache_sync_param.va;
		d->cache_sync_param.size = d32.cache_sync_param.size;
		d->cache_sync_param.sync_type = d32.cache_sync_param.sync_type;
		d->cache_sync_param.iova = d32.cache_sync_param.iova;
		break;
	case ION_SYS_GET_PHYS:
		d->get_phys_param.h.handle = d32.get_phys_param.handle;
		break;
	default:
		break;
	}

	ret = ion_compat_custom_sys(c, d);
	if (!ret && d32.sys_cmd == ION_SYS_GET_CLIENT) {
		d32.client = d->client;
		if (copy_to_user(uarg, &d32, sizeof(d32)))
			ret = -EFAULT;
	}
	kfree(d);
	return ret;
}

static int ion_compat_custom_mm32(struct ion_compat_client *c,
				  void __user *uarg)
{
	struct compat_ion_mm_data d32;
	struct ion_mm_data *d;
	bool out = false;
	int ret;

	if (copy_from_user(&d32, uarg, sizeof(d32)))
		return -EFAULT;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->mm_cmd = d32.mm_cmd;
	switch (d32.mm_cmd) {
	case ION_MM_CONFIG_BUFFER:
	case ION_MM_CONFIG_BUFFER_EXT:
		d->config_buffer_param.h.handle =
			d32.config_buffer_param.handle;
		d->config_buffer_param.module_id =
			d32.config_buffer_param.module_id;
		d->config_buffer_param.security =
			d32.config_buffer_param.security;
		break;
	case ION_MM_SET_DEBUG_INFO:
	case ION_MM_GET_DEBUG_INFO:
		d->buf_debug_info_param.h.handle =
			d32.buf_debug_info_param.handle;
		memcpy(d->buf_debug_info_param.dbg_name,
		       d32.buf_debug_info_param.dbg_name, ION_MM_DBG_NAME_LEN);
		d->buf_debug_info_param.value1 = d32.buf_debug_info_param.value[0];
		d->buf_debug_info_param.value2 = d32.buf_debug_info_param.value[1];
		d->buf_debug_info_param.value3 = d32.buf_debug_info_param.value[2];
		d->buf_debug_info_param.value4 = d32.buf_debug_info_param.value[3];
		out = d32.mm_cmd == ION_MM_GET_DEBUG_INFO;
		break;
	case ION_MM_SET_SF_BUF_INFO:
	case ION_MM_GET_SF_BUF_INFO:
		d->sf_buf_info_param.h.handle = d32.sf_buf_info_param.handle;
		memcpy(d->sf_buf_info_param.info, d32.sf_buf_info_param.info,
		       sizeof(d32.sf_buf_info_param.info));
		out = d32.mm_cmd == ION_MM_GET_SF_BUF_INFO;
		break;
	case ION_MM_ACQ_CACHE_POOL:
	case ION_MM_QRY_CACHE_POOL:
		out = true;
		break;
	case ION_MM_GET_IOVA:
	case ION_MM_GET_IOVA_EXT:
		d->get_phys_param.h.handle = d32.get_phys_param.handle;
		d->get_phys_param.module_id = d32.get_phys_param.module_id;
		d->get_phys_param.security = d32.get_phys_param.security;
		d->get_phys_param.coherent = d32.get_phys_param.coherent;
		out = true;
		break;
	default:
		break;
	}

	ret = ion_compat_custom_mm(c, d);
	if (ret || !out)
		goto done;

	switch (d32.mm_cmd) {
	case ION_MM_GET_DEBUG_INFO:
		memcpy(d32.buf_debug_info_param.dbg_name,
		       d->buf_debug_info_param.dbg_name, ION_MM_DBG_NAME_LEN);
		d32.buf_debug_info_param.value[0] = d->buf_debug_info_param.value1;
		d32.buf_debug_info_param.value[1] = d->buf_debug_info_param.value2;
		d32.buf_debug_info_param.value[2] = d->buf_debug_info_param.value3;
		d32.buf_debug_info_param.value[3] = d->buf_debug_info_param.value4;
		break;
	case ION_MM_GET_SF_BUF_INFO:
		memcpy(d32.sf_buf_info_param.info, d->sf_buf_info_param.info,
		       sizeof(d32.sf_buf_info_param.info));
		break;
	case ION_MM_ACQ_CACHE_POOL:
	case ION_MM_QRY_CACHE_POOL:
		d32.pool_info_param.ret = d->pool_info_param.ret;
		break;
	default:
		d32.get_phys_param.phy_addr = d->get_phys_param.phy_addr;
		d32.get_phys_param.len = d->get_phys_param.len;
		break;
	}
	if (copy_to_user(uarg, &d32, sizeof(d32)))
		ret = -EFAULT;
done:
	kfree(d);
	return ret;
}

static long ion_compat_compat_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	struct ion_compat_client *c = filp->private_data;
	void __user *uarg = compat_ptr(arg);

	switch (cmd) {
	case COMPAT_ION_IOC_ALLOC: {
		struct compat_ion_allocation_data a32;
		struct ion_allocation_data a = {};
		int ret;

		if (copy_from_user(&a32, uarg, sizeof(a32)))
			return -EFAULT;
		a.len = a32.len;
		a.align = a32.align;
		a.heap_id_mask = a32.heap_id_mask;
		a.flags = a32.flags;
		ret = ion_compat_alloc(c, &a);
		if (ret)
			return ret;
		a32.handle = a.handle;
		if (copy_to_user(uarg, &a32, sizeof(a32))) {
			ion_compat_free(c, a.handle);
			return -EFAULT;
		}
		return 0;
	}
	case COMPAT_ION_IOC_CUSTOM: {
		struct compat_ion_custom_data cd;

		if (copy_from_user(&cd, uarg, sizeof(cd)))
			return -EFAULT;
		if (cd.cmd == ION_CMD_SYSTEM)
			return ion_compat_custom_sys32(c, compat_ptr(cd.arg));
		if (cd.cmd == ION_CMD_MULTIMEDIA)
			return ion_compat_custom_mm32(c, compat_ptr(cd.arg));
		return -EOPNOTSUPP;
	}
	default:
		/* FREE/MAP/SHARE/IMPORT/SYNC/HEAP_QUERY have one layout */
		return ion_compat_ioctl(filp, cmd, (unsigned long)uarg);
	}
}
#endif

static int ion_compat_open(struct inode *inode, struct file *filp)
{
	struct ion_compat_client *c;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	mutex_init(&c->lock);
	xa_init_flags(&c->handles, XA_FLAGS_ALLOC1);
	filp->private_data = c;
	return 0;
}

static int ion_compat_release(struct inode *inode, struct file *filp)
{
	struct ion_compat_client *c = filp->private_data;
	struct ion_compat_buf *b;
	unsigned long idx;

	xa_for_each(&c->handles, idx, b) {
		dma_buf_put(b->dmabuf);
		kfree(b);
	}
	xa_destroy(&c->handles);
	mutex_destroy(&c->lock);
	kfree(c);
	return 0;
}

static const struct file_operations ion_compat_fops = {
	.owner = THIS_MODULE,
	.open = ion_compat_open,
	.release = ion_compat_release,
	.unlocked_ioctl = ion_compat_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ion_compat_compat_ioctl,
#endif
};

static struct miscdevice ion_compat_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &ion_compat_fops,
	.mode = 0666,
};

static int ion_compat_iova_probe(struct platform_device *pdev)
{
	int ret;

	if (!device_iommu_mapped(&pdev->dev)) {
		dev_info(&pdev->dev, "no IOMMU attached, GET_IOVA disabled\n");
		return -ENODEV;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;
	dma_set_max_seg_size(&pdev->dev, UINT_MAX);
	iova_dev = &pdev->dev;
	dev_info(&pdev->dev, "ION_MM_GET_IOVA enabled (M4U camera/MDP domain)\n");
	return 0;
}

static const struct of_device_id ion_compat_iova_of_ids[] = {
	{ .compatible = "mediatek,mtk-ion-compat-iova" },
	{}
};

static struct platform_driver ion_compat_iova_driver = {
	.probe = ion_compat_iova_probe,
	.driver = {
		.name = "mtk-ion-compat-iova",
		.of_match_table = ion_compat_iova_of_ids,
		.suppress_bind_attrs = true,
	},
};

static int __init ion_compat_init(void)
{
	int ret;

	if (!enable) {
		pr_info("disabled (boot with mtk_ion_compat.enable=1)\n");
		return 0;
	}
	ret = platform_driver_register(&ion_compat_iova_driver);
	if (ret)
		pr_warn("iova driver register failed: %d\n", ret);
	ret = misc_register(&ion_compat_misc);
	if (ret)
		pr_err("misc_register failed: %d\n", ret);
	else
		pr_info("/dev/ion registered (legacy ABI over dma-heap \"system\"); needs debug.c2.use_dmabufheaps=1 or camera-only sepolicy\n");
	return 0;
}
/* the system heap registers at module_init; heaps are looked up lazily */
late_initcall(ion_compat_init);

MODULE_DESCRIPTION("Legacy MediaTek ION ABI over DMA-BUF heaps");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
