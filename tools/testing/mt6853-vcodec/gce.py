#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the MT6853 completion, waiter and register lifetime paths on the host."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
vcu = (root / "drivers/misc/mtk-vcu/mtk_vcu.c").read_text()
enc = (root / "drivers/media/platform/mediatek/mt6853-vcodec/mtk_vcodec_enc.c").read_text()


def definition(text, name):
    start = re.search(r"^(?:static )?(?:int|void|bool) " + name + r"\(", text, re.M).start()
    return text[start:text.index("\n}", start) + 2] + "\n"


shim = r"""
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int atomic_t;
#define __user
#define GFP_KERNEL 0
#define kmalloc(n,g) malloc(n)
#define kfree(p) free(p)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define atomic_read(p) (*(p))
#define atomic_set(p,i) (*(p)=(i))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
#define atomic_dec_and_test(p) (!--*(p))
#define atomic_xchg(p,n) __atomic_exchange_n(p,n,__ATOMIC_SEQ_CST)
static int atomic_dec_if_positive(int *p) { return *p>0 ? --*p : -1; }
#define WRITE_ONCE(p,v) ((p)=(v))
#define READ_ONCE(p) (p)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define vcu_trace_log(...) ((void)0)
#define vcu_trace(...) ((void)0)
#define mtk_v4l2_debug(...) ((void)0)
#define copy_from_user(d,s,n) (memcpy(d,s,n),0)
#define mutex_lock(m) assert((*(m))++==0)
#define mutex_unlock(m) assert(--*(m)==0)
#define VCU_VDEC 0
#define VCU_VENC 1
#define VCU_CODEC_MAX 2
#define VCODEC_INST_MAX 3
struct vb2_queue {int errors; bool streaming;};
struct m2m {struct {struct vb2_queue q;} cap_q_ctx,out_q_ctx;};
struct mtk_video_enc_buf {int lastframe,vb;};
enum venc_lock {VENC_LOCK_NONE,VENC_LOCK_NORMAL,VENC_LOCK_SEC};
struct codec_dev {int is_codec_suspending,enc_sem[1];enum venc_lock enc_hw_locked[1];};
#define MTK_VENC_HW_NUM 1
static int down_trylock(int *s){if(!*s)return 1;--*s;return 0;}
struct mtk_vcodec_ctx {atomic_t gce_failed; int state, events; struct m2m *m2m_ctx;
    struct mtk_video_enc_buf *enc_flush_buf;struct codec_dev *dev;};
struct file {struct mtk_vcodec_ctx *ctx;};
#define MTK_STATE_ABORT 7
#define file_to_ctx(f) ((f)->ctx)
#define vb2_queue_error(q) (++(q)->errors)
#define mtk_venc_queue_error_event(c) (++(c)->events)
#define v4l2_m2m_get_vq(m,t) ((t)==V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE?&(m)->out_q_ctx.q:&(m)->cap_q_ctx.q)
#define vb2_is_streaming(q) ((q)->streaming)
#define NON_EOS 0
#define EOS 1
static int schedules;
#define v4l2_m2m_buf_queue_check(m,b) ((void)0)
#define v4l2_m2m_try_schedule(m) (++schedules)
#define vb2_clear_last_buffer_dequeued(q) ((void)0)
#define V4L2_PIX_FMT_H265 V4L2_PIX_FMT_HEVC
#ifndef V4L2_PIX_FMT_HEIF
#define V4L2_PIX_FMT_HEIF v4l2_fourcc('H','E','I','F')
#endif
struct mtk_vcodec_mem {size_t size;};
struct venc_done_result {u64 bs_va; u32 bs_size;};
struct work_struct {int unused;};
struct gce_obj {unsigned int codec_type; u64 gce_handle;};
struct vcu_codec_ops {
    void (*venc_gce_error)(void *,int);
    void (*pmqos_gce_end)(void *,int,int);
    void (*encode_unprepare)(void *,int,unsigned long *);
    void (*venc_unlock)(void *,int);
};
struct kref {int count;};
#define kref_init(k) ((k)->count=1)
#define kref_get(k) assert((k)->count++>0)
#define kref_put(k,f) do {if(!--(k)->count) (f)(k);} while(0)
struct vm_area_struct {int vm_page_prot; unsigned long vm_start; void *vm_private_data;
    const void *vm_ops;};
static int remap_status, vcu_reg_vm_ops;
#define vcu_reg_index(v,pa,n) ((int)(pa))
#define pgprot_noncached(p) (p)
#define vm_flags_set(...) ((void)0)
#define io_remap_pfn_range(...) remap_status
static bool vdec;
"""
for name in ("gce_ctx_info", "vcu_reg_mapping"):
    start = vcu.index("struct " + name + " {")
    shim += vcu[start:vcu.index("\n};", start) + 3] + "\n"
shim += re.search(r"^static bool venc_always_on[^;]*;", vcu, re.M)[0] + "\n"
shim += r"""
struct mtk_vcu {
    const struct vcu_codec_ops *codec_ops;
    struct gce_ctx_info gce_info[VCODEC_INST_MAX], *venc_owner[1];
    int vcu_gce_mutex[2],vcu_share,gce_job_cnt[2][1],gce_wq[2],reg_maps,reg_map_wq;
    unsigned long flags[2],gce_progress[2][1]; void *curr_ctx[2],*curr_src_vb[2],*curr_dst_vb[2];
    int gce_th_num[2];
    int (*reg_get)(void *); void (*reg_put)(void *); void *reg_priv; bool venc_held;
};
struct vcu_gce_job {struct work_struct work; struct mtk_vcu *vcu;
    struct {int codec_type,core_id;} obj; int sta,inst; u64 generation; bool pm_ref; int pkt;};
static struct mtk_vcu bridge;
struct platform_device {struct mtk_vcu *vcu;};
#define platform_get_drvdata(p) ((p)->vcu)
static int channel_resets;
#define wait_event_timeout(w,p,t) (!!(p))
#define vcu_gce_abort(v,t,c) (++channel_resets)
static struct mtk_vcodec_ctx *first;
static int released,unlocked,woken,packets,holds,get_error;
static void (*during_wait)(void);
#define wait_event_interruptible(w,p) ({assert(!(p));assert(during_wait);during_wait();assert(p);0;})
#define wake_up(q) (++woken)
#define wake_up_all(q) (++woken)
#define jiffies 11
#define pm_runtime_mark_last_busy(...) ((void)0)
#define pm_runtime_put_autosuspend(...) ((void)0)
#define cmdq_pkt_destroy(...) (++packets)
static void unprepare(void *c,int core,unsigned long *f) {
    assert(c==first && !core && f==&bridge.flags[1]); ++released;
}
static void unlock(void *c,int core) {assert(c==first && !core); ++unlocked;}
static int get(void *priv) {assert(priv==&holds); if(get_error)return -EIO; ++holds;return 0;}
static void put(void *priv) {assert(priv==&holds && holds>0); --holds;}
"""
source = shim
for name in ("mtk_venc_lock", "mtk_venc_gce_error", "mtk_venc_header_valid", "vidioc_try_encoder_cmd",
             "vidioc_encoder_cmd"):
    source += definition(enc, name)
for name in ("vcu_gce_clear_inst", "vcu_gce_get_inst_id", "vcu_gce_set_inst_id",
             "vcu_venc_release_core", "vcu_clear_codec_ctx", "vcu_gce_job_done", "vcu_wait_gce_callback",
             "vcu_venc_daemon_hold", "vcu_reg_mapping_release", "vcu_reg_vm_open",
             "vcu_reg_vm_close", "vcu_mmap_reg"):
    source += definition(vcu, name)
source += r"""
static void callback(void *ctx,int status) {
    assert(status==-ECONNABORTED && ctx==first && bridge.gce_info[0].flush_done==0);
    mtk_venc_gce_error(ctx,status);
}
static void reuse(void) {
    vcu_gce_clear_inst(&bridge.gce_info[0]);
    assert(vcu_gce_set_inst_id(&bridge,1,42)==0);
    bridge.gce_info[0].flush_done=1;
}
static void done(u64 generation,int status) {
    struct vcu_gce_job *job=calloc(1,sizeof(*job)); assert(job);
    job->vcu=&bridge; job->obj.codec_type=1; job->generation=generation;
    job->sta=status; vcu_gce_job_done(&job->work);
}
static void codec(void) {
    struct vcu_codec_ops ops={callback,NULL,unprepare,unlock};
    struct m2m m={0}; struct mtk_video_enc_buf flush={0};
    struct mtk_vcodec_ctx a={.m2m_ctx=&m,.enc_flush_buf=&flush},b=a;
    struct v4l2_encoder_cmd stop={.cmd=V4L2_ENC_CMD_STOP};
    struct platform_device pdev={&bridge};struct codec_dev dev={.is_codec_suspending=1,.enc_sem={1}};
    a.dev=&dev;assert(mtk_venc_lock(&a,0,false)==-EBUSY && dev.enc_sem[0]==1);
    dev.is_codec_suspending=0;assert(!mtk_venc_lock(&a,0,false) && dev.enc_sem[0]==0);
    struct file f={&a}; struct gce_obj wait={1,42};
    struct mtk_vcodec_mem bs={64}; struct venc_done_result h={(uintptr_t)&bs,8};
    bridge.codec_ops=&ops; bridge.curr_ctx[1]=first=&a;
    assert(vcu_gce_set_inst_id(&bridge,1,42)==0);
    assert(vcu_gce_set_inst_id(&bridge,0,42)==1);
    assert(vcu_gce_get_inst_id(&bridge,0,42)==1);
    struct gce_ctx_info *slot=&bridge.gce_info[0]; u64 g=slot->generation;
    bridge.venc_owner[0]=slot; slot->flush_pending=bridge.gce_job_cnt[1][0]=2;
    done(g,-ECONNABORTED);
    assert(a.gce_failed && a.events==1 && m.cap_q_ctx.q.errors==1 && m.out_q_ctx.q.errors==1);
    assert(slot->flush_done==1 && slot->flush_pending==1 && !released);
    assert(vcu_wait_gce_callback(&bridge,&wait)==0 && !slot->flush_done);
    done(g,-ECONNABORTED);
    assert(a.events==1 && packets==2 && released==1 && unlocked==1 && !bridge.venc_owner[0]);
    assert(vcu_wait_gce_callback(&bridge,&wait)==0 && !slot->flush_done);
    assert(vcu_wait_gce_callback(&bridge,&wait)==-EIO);
    assert(vidioc_encoder_cmd(&f,NULL,&stop)==-EIO && !schedules);
    f.ctx=&b; m.cap_q_ctx.q.streaming=m.out_q_ctx.q.streaming=true;
    assert(vidioc_encoder_cmd(&f,NULL,&stop)==0 && schedules==1 && flush.lastframe==EOS);
    bridge.curr_ctx[1]=&b; reuse(); g=slot->generation; slot->flush_done=0; during_wait=reuse;
    assert(vcu_wait_gce_callback(&bridge,&wait)==-ECANCELED && slot->flush_done==1);
    bridge.gce_job_cnt[1][0]=slot->flush_pending=1; bridge.venc_owner[0]=slot;
    done(g,-ECONNABORTED);
    assert(slot->flush_done==1 && slot->flush_pending==1 && !slot->failed && !b.gce_failed);
    assert(bridge.gce_job_cnt[1][0]==1 && released==1 && packets==3);
    first=&b;g=slot->generation;bridge.gce_th_num[1]=1;
    assert(!vcu_clear_codec_ctx(&pdev,&b,1) && channel_resets==2 && !bridge.venc_owner[0]);
    assert(!slot->used && bridge.curr_ctx[1]==NULL && !bridge.gce_job_cnt[1][0]);
    done(g,-ECONNABORTED);assert(unlocked==2 && released==2 && !bridge.gce_job_cnt[1][0]);
    assert(mtk_venc_header_valid(V4L2_PIX_FMT_HEIF,&h,&bs));
    h.bs_size=0; assert(!mtk_venc_header_valid(V4L2_PIX_FMT_H264,&h,&bs));
    assert(mtk_venc_header_valid(V4L2_PIX_FMT_H263,&h,&bs));
    h.bs_size=65;assert(!mtk_venc_header_valid(V4L2_PIX_FMT_HEIF,&h,&bs));
    h.bs_size=8;h.bs_va=0;assert(!mtk_venc_header_valid(V4L2_PIX_FMT_H265,&h,&bs));
}
static void maps(void) {
    struct mtk_vcu v={.reg_get=get,.reg_put=put,.reg_priv=&holds};
    struct vm_area_struct a={0},b;
    vcu_venc_daemon_hold(&v,true);assert(!holds);
    venc_always_on=true;vcu_venc_daemon_hold(&v,true);vcu_venc_daemon_hold(&v,true);
    assert(holds==1);vcu_venc_daemon_hold(&v,false);assert(!holds);
    get_error=1;assert(vcu_mmap_reg(&v,&a,1,4096)==-EIO && !v.reg_maps && !holds);
    get_error=0;remap_status=-EFAULT;
    assert(vcu_mmap_reg(&v,&a,1,4096)==-EFAULT && !v.reg_maps && !holds && !a.vm_ops);
    remap_status=0;assert(vcu_mmap_reg(&v,&a,1,4096)==0 && holds==1 && v.reg_maps==1);
    b=a;vcu_reg_vm_open(&b);vcu_reg_vm_close(&a);assert(holds==1 && v.reg_maps==1);
    v.reg_get=NULL;v.reg_put=NULL;v.reg_priv=NULL;
    vcu_reg_vm_close(&b);assert(!holds && !v.reg_maps);
    assert(vcu_mmap_reg(&v,&a,1,4096)==-ENODEV);
    assert(vcu_mmap_reg(&v,&a,0,4096)==-EPERM);
    vdec=true;assert(vcu_mmap_reg(&v,&a,0,4096)==0 && !holds);
}
int main(void) {
    codec();maps();puts("PASS: GCE abort, Stop, slot reuse, headers, daemon and VMA power lifetime");
}
"""
with tempfile.TemporaryDirectory() as work:
    exe = str(Path(work) / "gce")
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Werror", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", "-x", "c", "-", "-o", exe],
                   input=source, text=True, check=True)
    subprocess.run([exe], check=True)
